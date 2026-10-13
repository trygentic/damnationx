#!/usr/bin/env python3
"""modkit: validate, build and preview schema-v1 asset packages.

    modkit validate <package-dir | manifest.ini> [--search-path DIR]...
    modkit build    <package-dir | manifest.ini> --output pack.mpq [--search-path DIR]...
    modkit preview  <package-dir | manifest.ini> --output sheet.png [--light X,Y,Z] [--scale N]

Dependencies are resolved from sibling package directories (or --search-path, which may
also point directly at a package). `build` writes an uncompressed, deterministic MPQ with
generated atlas pages and an index, plus a `<output>.sha256` sidecar.
Requires Pillow and numpy.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

MAX_IMAGE_DIM = 4096
MAX_FRAMES = 1024
MAX_ARCHIVE_BYTES = 256 << 20
MAX_DECODED_BYTES = 512 << 20
ATLAS_PAGE_DIM = 2048
ATLAS_PADDING = 1

CATEGORIES = ("terrain", "player", "equipment", "monster", "item", "spell", "materials", "ui_frames")
SPRITE_CATEGORIES = tuple(c for c in CATEGORIES if c != "materials")
DIRECTIONS = {"s", "sw", "w", "nw", "n", "ne", "e", "se"}
ID_RE = re.compile(r"[a-z0-9][a-z0-9._-]*")
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")


class ModkitError(Exception):
    pass


# ---------------------------------------------------------------- identifiers / versions

def normalize_id(raw: str) -> str:
    """Mirror of NormalizeModLogicalId in Source/mods/mod_identity.cpp."""
    out, sep = "", False
    for ch in raw:
        if ch.isspace():
            continue
        c = ch.lower()
        if c in "/\\.":
            sep = bool(out)
            continue
        if sep:
            out += "."
            sep = False
        out += c
    return out.strip(".")


def check_id(raw: str, what: str) -> str:
    norm = normalize_id(raw)
    if not ID_RE.fullmatch(norm):
        raise ModkitError(f"{what}: invalid identifier {raw!r} (normalized {norm!r})")
    return norm


def parse_semver(v: str):
    m = SEMVER_RE.match(v)
    if not m:
        raise ModkitError(f"invalid SemVer {v!r}")
    pre = tuple((0, int(p)) if p.isdigit() else (1, p) for p in (m.group(4) or "").split(".") if p)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), (1,) if not pre else (0, pre))


def parse_constraint(text: str):
    parts = text.strip().split()
    if not parts:
        raise ModkitError("empty dependency constraint")
    ops = []
    for p in parts:
        m = re.match(r"^(>=|<=|=|>|<)?(.+)$", p)
        ops.append((m.group(1) or "=", parse_semver(m.group(2))))
    lo = max(((v, op == ">") for op, v in ops if op in (">", ">=", "=")), default=None)
    hi = min(((v, op == "<") for op, v in ops if op in ("<", "<=", "=")), default=None)
    if lo and hi and (lo[0] > hi[0] or (lo[0] == hi[0] and (lo[1] or hi[1]))):
        raise ModkitError(f"contradictory dependency constraint {text!r}")
    return ops


def satisfies(ver, ops) -> bool:
    cmp = {"=": ver.__eq__, ">": ver.__gt__, ">=": ver.__ge__, "<": ver.__lt__, "<=": ver.__le__}
    return all(cmp[op](v) for op, v in ops)


# ---------------------------------------------------------------- manifest

def parse_ini(text: str, where: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = None
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or s[0] in ";#":
            continue
        if s[0] == "[":
            if not s.endswith("]"):
                raise ModkitError(f"{where}:{n}: malformed section header")
            current = s[1:-1].strip()
            sections.setdefault(current, {})
            continue
        if current is None or "=" not in s:
            raise ModkitError(f"{where}:{n}: expected key = value inside a section")
        k, v = (x.strip() for x in s.split("=", 1))
        if k in sections[current]:
            raise ModkitError(f"{where}:{n}: duplicate key {k!r} in [{current}]")
        sections[current][k] = v
    return sections


def split_list(v: str) -> list[str]:
    return [x.strip() for x in v.split(",") if x.strip()]


def read_utf8(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8")
    except UnicodeDecodeError as e:
        raise ModkitError(f"{path}: invalid UTF-8 ({e.reason} at byte {e.start})")


# ---------------------------------------------------------------- package

class ImageInfo:
    """Decoded frame source: `palette` is 768 RGB bytes for indexed images, else None."""

    def __init__(self, image: Image.Image):
        self.size = image.size
        self.indexed = image.mode == "P"
        self.palette = None
        if self.indexed:
            pal = bytes(image.getpalette("RGB") or b"")
            self.palette = (pal + bytes(768))[:768]


def read_palette(path: Path) -> bytes:
    with Image.open(path) as im:
        return ImageInfo(im).palette


class Package:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.manifest_path = self.root / "manifest.ini"
        self.ini = parse_ini(read_utf8(self.manifest_path), str(self.manifest_path))
        self.legacy = "schema" not in self.ini
        if self.legacy:
            return
        if self.ini["schema"].get("version") != "1":
            raise ModkitError(f"{self.manifest_path}: unsupported schema version {self.ini['schema'].get('version')!r}")
        pkg = self.ini.get("package", {})
        self.id = check_id(pkg.get("id", ""), f"{self.manifest_path}: [package] id")
        self.version = pkg.get("version", "")
        self.semver = parse_semver(self.version)
        self.aliases = sorted({check_id(a, f"{self.manifest_path}: alias") for a in split_list(pkg.get("aliases", ""))})
        self.capabilities = sorted(set(split_list(self.ini.get("capabilities", {}).get("provides", ""))))
        self.dependencies = {}
        for name, constraint in self.ini.get("dependencies", {}).items():
            try:
                self.dependencies[check_id(name, f"{self.manifest_path}: dependency")] = parse_constraint(constraint)
            except ModkitError as e:
                raise ModkitError(f"{self.manifest_path}: dependency {name!r}: {e}")
        self.assets_path = self.root / "assets.json"
        if not self.assets_path.is_file():
            raise ModkitError(f"{self.assets_path}: missing (required by schema 1)")
        try:
            self.assets = json.loads(read_utf8(self.assets_path))
        except json.JSONDecodeError as e:
            raise ModkitError(f"{self.assets_path}: invalid JSON: {e}")
        self.resolved_deps: list[Package] = []   # direct deps, canonical-id order after resolution
        self.identity = ""
        self.records: dict[str, dict] = {}       # merged records keyed by "category:id"

    def names(self):
        return [self.id, *self.aliases]

    # -- paths ---------------------------------------------------------------

    def local_path(self, rel: str, what: str) -> Path:
        if not isinstance(rel, str) or not rel or rel.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", rel):
            raise ModkitError(f"{what}: path must be relative and package-local: {rel!r}")
        parts = rel.replace("\\", "/").split("/")
        if any(p in ("", ".", "..") for p in parts):
            raise ModkitError(f"{what}: path traversal or empty component: {rel!r}")
        p = self.root
        for part in parts:  # exact on-disk case, so case-insensitive hosts don't hide collisions
            if part not in os.listdir(p):
                raise ModkitError(f"{what}: file not found (case-sensitive): {rel!r}")
            p = p / part
        if not p.resolve().is_relative_to(self.root) or not p.is_file():
            raise ModkitError(f"{what}: path escapes the package or is not a file: {rel!r}")
        return p

    def image(self, rel: str, what: str, decoded: list[int]) -> ImageInfo:
        p = self.local_path(rel, what)
        # Frames of one sheet reference the same PNG; decode it (and charge the budget) once.
        cache = self.__dict__.setdefault("_image_cache", {})
        if p in cache:
            return cache[p]
        try:
            with Image.open(p) as im:
                im.verify()
            with Image.open(p) as im:
                info = ImageInfo(im)
        except Exception as e:
            raise ModkitError(f"{what}: malformed image {rel!r}: {e}")
        if max(info.size) > MAX_IMAGE_DIM:
            raise ModkitError(f"{what}: image {rel!r} exceeds {MAX_IMAGE_DIM}px: {info.size[0]}x{info.size[1]}")
        decoded[0] += info.size[0] * info.size[1] * 4
        if decoded[0] > MAX_DECODED_BYTES:
            raise ModkitError(f"{what}: decoded image budget of {MAX_DECODED_BYTES} bytes exceeded")
        cache[p] = info
        return info


# ---------------------------------------------------------------- resolution

def load_search_space(root: Package, search_paths: list[Path]) -> dict[str, Package]:
    """Every schema-v1 package found in the search paths (or at them), keyed by id and alias."""
    by_name: dict[str, Package] = {}
    candidates = []
    for base in search_paths:
        candidates.append(base)
        candidates += sorted(base.iterdir()) if base.is_dir() else []
    for child in candidates:
        if not (child / "manifest.ini").is_file():
            continue
        try:
            pkg = root if child.resolve() == root.root else Package(child)
        except ModkitError as e:
            print(f"note: ignoring package at {child}: {e}", file=sys.stderr)
            continue
        if pkg.legacy:
            continue
        for name in pkg.names():
            if name in by_name and by_name[name].root != pkg.root:
                raise ModkitError(f"package name {name!r} is claimed by both {by_name[name].root} and {pkg.root}")
            by_name[name] = pkg
    return by_name


def resolve(root: Package, space: dict[str, Package]) -> list[Package]:
    """Topological order (deps first), lexical canonical-ID tie-break. Sets identities."""
    graph: dict[str, Package] = {}

    def visit(pkg: Package, stack: list[str]):
        if pkg.id in stack:
            raise ModkitError(f"dependency cycle: {' -> '.join(stack + [pkg.id])}")
        if pkg.id in graph:
            return
        for name, ops in pkg.dependencies.items():
            dep = space.get(name)
            if dep is None:
                raise ModkitError(f"{pkg.manifest_path}: missing dependency {name!r}")
            if not satisfies(dep.semver, ops):
                raise ModkitError(f"{pkg.manifest_path}: dependency {name!r} resolved to {dep.id} {dep.version}, which does not satisfy the constraint")
            visit(dep, stack + [pkg.id])
            pkg.resolved_deps.append(dep)
        graph[pkg.id] = pkg

    visit(root, [])
    order: list[Package] = []
    remaining = dict(graph)
    while remaining:
        ready = sorted(pid for pid, p in remaining.items() if all(d.id not in remaining for d in p.resolved_deps))
        for pid in ready:
            order.append(remaining.pop(pid))
    all_names = [n for p in order for n in p.names()]
    dupes = sorted({n for n in all_names if all_names.count(n) > 1})
    if dupes:
        raise ModkitError(f"package names/aliases not unique across the dependency set: {dupes}")
    for pkg in order:
        pkg.resolved_deps.sort(key=lambda d: d.id)
        pkg.identity = package_identity(pkg)
    return order


def package_identity(pkg: Package) -> str:
    """sha256 over the same input string as VersionedModIdentityInput (mod_identity.cpp)."""
    lines = ["1", pkg.id, pkg.version, *pkg.aliases, *pkg.capabilities, *(d.identity for d in pkg.resolved_deps)]
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


# ---------------------------------------------------------------- assets.json

def validate_assets(pkg: Package) -> None:
    where = str(pkg.assets_path)
    a = pkg.assets
    if not isinstance(a, dict) or a.get("schema") != 1:
        raise ModkitError(f"{where}: must be an object with \"schema\": 1")
    unknown = set(a) - set(CATEGORIES) - {"schema"}
    if unknown:
        raise ModkitError(f"{where}: unknown top-level keys {sorted(unknown)}")
    decoded = [0]
    seen: dict[str, str] = {}
    for cat in ("materials", *SPRITE_CATEGORIES):  # materials first: sprites reference them
        arr = a.get(cat, [])
        if not isinstance(arr, list):
            raise ModkitError(f"{where}: {cat!r} must be an array")
        for i, rec in enumerate(arr):
            ctx = f"{where}: {cat}[{i}]"
            if not isinstance(rec, dict) or "id" not in rec:
                raise ModkitError(f"{ctx}: record must be an object with an \"id\"")
            rid = check_id(str(rec["id"]), f"{ctx} id")
            ctx = f"{ctx} {rid!r}"
            for name in [rid, *(check_id(str(x), f"{ctx} alias") for x in rec.get("aliases", []))]:
                if name in seen:
                    raise ModkitError(f"{ctx}: duplicate logical ID/alias {name!r} (also {seen[name]})")
                seen[name] = ctx
            nulls = [k for k, v in rec.items() if v is None]
            if nulls:
                raise ModkitError(f"{ctx}: explicit null is not a fallback: {nulls}")
            base = next((d.records.get(f"{cat}:{rid}") for d in pkg.resolved_deps if f"{cat}:{rid}" in d.records), None)
            merged = {**(base or {}), **rec}   # field-level merge; arrays replace whole
            merged["_package"] = pkg.id
            if cat == "materials":
                for ch in ("albedo", "normal", "emission"):
                    if ch in rec:  # only channels supplied by this package are package-local files
                        pkg.image(rec[ch], f"{ctx} {ch}", decoded)
            else:
                validate_sprite(pkg, merged, ctx, decoded, rec)
            pkg.records[f"{cat}:{rid}"] = merged


def validate_sprite(pkg: Package, r: dict, ctx: str, decoded, rec: dict) -> None:
    frames = r.get("frames")
    if not isinstance(frames, list) or not frames:
        raise ModkitError(f"{ctx}: \"frames\" must be a non-empty array (own or inherited)")
    if len(frames) > MAX_FRAMES:
        raise ModkitError(f"{ctx}: {len(frames)} frames exceeds {MAX_FRAMES}")
    dims = None
    representation = None  # ("rgba", None) or ("indexed", palette bytes)
    for j, f in enumerate(frames):
        fctx = f"{ctx} frame {j}"
        if not isinstance(f, dict):
            raise ModkitError(f"{fctx}: frame must be an object")
        w, h = f.get("width"), f.get("height")
        x, y = f.get("x", 0), f.get("y", 0)
        if not all(isinstance(v, int) and not isinstance(v, bool) for v in (w, h, x, y)) or w <= 0 or h <= 0 or x < 0 or y < 0:
            raise ModkitError(f"{fctx}: width/height must be positive integers and x/y non-negative integers")
        if dims and dims != (w, h):
            raise ModkitError(f"{fctx}: dimensions {w}x{h} differ from frame 0 {dims[0]}x{dims[1]}")
        dims = (w, h)
        if "direction" in f and f["direction"] not in DIRECTIONS:
            raise ModkitError(f"{fctx}: bad animation direction {f['direction']!r}; expected one of {sorted(DIRECTIONS)}")
        if "frames" in rec:  # validate files only for frames this package supplies
            info = pkg.image(f.get("path"), f"{fctx} ({f.get('path')!r})", decoded)
            if x + w > info.size[0] or y + h > info.size[1]:
                raise ModkitError(f"{fctx}: rectangle {x},{y} {w}x{h} exceeds sheet {f['path']!r} ({info.size[0]}x{info.size[1]})")
            rep = ("indexed", info.palette) if info.indexed else ("rgba", None)
            if representation and representation != rep:
                why = "mixes indexed and RGBA sources" if representation[0] != rep[0] else "uses a different palette than frame 0"
                raise ModkitError(f"{fctx}: {f['path']!r} {why}; one record must share one representation")
            representation = rep
    if representation:
        r["_representation"] = representation[0]
        r["_palette"] = hashlib.sha256(representation[1]).hexdigest() if representation[1] else None
    pivot = r.get("pivot")
    if pivot is not None:
        if not isinstance(pivot, dict) or not all(isinstance(pivot.get(k), int) for k in ("x", "y")):
            raise ModkitError(f"{ctx}: pivot must be {{\"x\": int, \"y\": int}}")
        if not (0 <= pivot["x"] < dims[0] and 0 <= pivot["y"] < dims[1]):
            raise ModkitError(f"{ctx}: pivot ({pivot['x']},{pivot['y']}) outside frame {dims[0]}x{dims[1]}")
    if "material" in r:
        r["_material"], r["_material_id"] = resolve_material(pkg, r["material"], ctx)


def resolve_material(pkg: Package, ref, ctx: str) -> tuple[str, str]:
    """Returns (owning package id, material id). Unqualified refs resolve in this package first,
    then in dependencies; qualified refs are "dep.id:material.id" (':' is outside the id charset)."""
    if not isinstance(ref, str):
        raise ModkitError(f"{ctx}: material reference must be a string")
    if ":" in ref:
        dep_name, mid = ref.split(":", 1)
        mid = normalize_id(mid)
        dep = next((d for d in pkg.resolved_deps if normalize_id(dep_name) in d.names()), None)
        if dep is None:
            raise ModkitError(f"{ctx}: material {ref!r} names a package that is not a declared dependency")
        if f"materials:{mid}" not in dep.records:
            raise ModkitError(f"{ctx}: material {ref!r} not found in {dep.id}")
        return dep.id, mid
    mid = normalize_id(ref)
    if f"materials:{mid}" in pkg.records:
        return pkg.id, mid
    hits = [d for d in pkg.resolved_deps if f"materials:{mid}" in d.records]
    if len(hits) == 1:
        return hits[0].id, mid
    if not hits:
        raise ModkitError(f"{ctx}: material {ref!r} not found in this package or its dependencies")
    raise ModkitError(f"{ctx}: material {ref!r} is ambiguous across dependencies {[d.id for d in hits]}; qualify it as \"<package>:{ref}\"")


# ---------------------------------------------------------------- driver

def load(target: Path, search_paths: list[str]) -> tuple[Package, list[Package]]:
    root_dir = target.parent if target.is_file() else target
    pkg = Package(root_dir)
    if pkg.legacy:
        return pkg, []
    paths = [Path(p) for p in search_paths] or [pkg.root.parent]
    order = resolve(pkg, load_search_space(pkg, paths) if pkg.dependencies else {})
    for p in order:  # deps first, so overrides can inherit
        validate_assets(p)
    return pkg, order


def own_sprites(pkg: Package):
    """(record key, merged record) for sprite records whose frames this package supplies."""
    for key, rec in sorted(pkg.records.items()):
        cat, rid = key.split(":", 1)
        own = next(r for r in pkg.assets[cat] if normalize_id(str(r["id"])) == rid)
        if cat != "materials" and "frames" in own:
            yield key, rec


def collect_files(pkg: Package) -> dict[str, bytes]:
    files = {"manifest.ini": pkg.manifest_path.read_bytes(), "assets.json": pkg.assets_path.read_bytes()}
    for cat in CATEGORIES:
        for rec in pkg.assets.get(cat, []):
            if cat == "materials":
                paths = [rec[ch] for ch in ("albedo", "normal", "emission") if ch in rec]
            else:
                paths = [f["path"] for f in rec.get("frames", [])]
            for rel in paths:
                files[rel.replace("\\", "/")] = pkg.local_path(rel, "build").read_bytes()
    low = {}
    for name in files:
        if name.lower() in low:
            raise ModkitError(f"case-colliding paths {low[name.lower()]!r} and {name!r}")
        low[name.lower()] = name
    return files


# ---------------------------------------------------------------- frames, atlases, re-encoding

def frame_pixels(pkg: Package, frame: dict) -> np.ndarray:
    """Frame rectangle as (h, w) palette indices for indexed sources or (h, w, 4) RGBA."""
    x, y, w, h = frame.get("x", 0), frame.get("y", 0), frame["width"], frame["height"]
    with Image.open(pkg.local_path(frame["path"], "frame")) as im:
        region = im.crop((x, y, x + w, y + h))
        return np.asarray(region if im.mode == "P" else region.convert("RGBA"))


def to_rgba(pixels: np.ndarray, palette: bytes | None) -> np.ndarray:
    """Indexed frames become RGBA with index 0 transparent (classic sprite semantics)."""
    if pixels.ndim == 3:
        return pixels
    lut = np.frombuffer(palette, dtype=np.uint8).reshape(256, 3)
    rgba = np.concatenate([lut[pixels], np.full(pixels.shape + (1,), 255, np.uint8)], axis=-1)
    rgba[pixels == 0] = 0
    return rgba


def png_bytes(image: Image.Image) -> bytes:
    buf = BytesIO()
    image.save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def pack_atlases(pkg: Package) -> tuple[dict[str, bytes], dict[tuple[str, int], dict]]:
    """Shelf-packs every own frame into pages keyed by representation (one page set per palette).
    Returns (page files, placement per (record key, frame index)). Deterministic: frames are
    ordered by height, width, record key and index; pages are cropped to their used extent.
    Indexed frames are re-encoded into indexed pages carrying the shared palette, RGBA into RGBA."""
    items = []
    palettes: dict[str, bytes] = {}
    for key, rec in own_sprites(pkg):
        page_key = "rgba"
        if rec["_representation"] == "indexed":
            page_key = f"indexed-{rec['_palette'][:16]}"
            palettes.setdefault(page_key, read_palette(pkg.local_path(rec["frames"][0]["path"], key)))
        for j, f in enumerate(rec["frames"]):
            items.append((page_key, -f["height"], -f["width"], key, j, f))
    files: dict[str, bytes] = {}
    placements: dict[tuple[str, int], dict] = {}
    for page_key in sorted({it[0] for it in items}):
        pages: list[list] = []  # [used width, used height, [(x, y, key, j, frame)]]
        shelf = None            # (page index, shelf y, shelf height, next x)
        for _, nh, nw, key, j, f in sorted(it for it in items if it[0] == page_key):
            w, h = -nw + ATLAS_PADDING, -nh + ATLAS_PADDING
            if w > ATLAS_PAGE_DIM or h > ATLAS_PAGE_DIM:
                raise ModkitError(f"{key} frame {j}: {f['width']}x{f['height']} does not fit an atlas page of {ATLAS_PAGE_DIM}px")
            if shelf is None or shelf[3] + w > ATLAS_PAGE_DIM:  # new shelf, maybe new page
                if shelf is None or shelf[1] + shelf[2] + h > ATLAS_PAGE_DIM:
                    pages.append([0, 0, []])
                    shelf = (len(pages) - 1, 0, h, 0)
                else:
                    shelf = (shelf[0], shelf[1] + shelf[2], h, 0)
            page = pages[shelf[0]]
            x, y = shelf[3], shelf[1]
            page[2].append((x, y, key, j, f))
            page[0], page[1] = max(page[0], x + w), max(page[1], y + h)
            shelf = (shelf[0], shelf[1], max(shelf[2], h), x + w)
        for n, (pw, ph, placed) in enumerate(pages):
            name = f"atlas/{page_key}-{n}.png"
            canvas = np.zeros((ph, pw, 4) if page_key == "rgba" else (ph, pw), np.uint8)
            for x, y, key, j, f in placed:
                canvas[y:y + f["height"], x:x + f["width"]] = frame_pixels(pkg, f)
                placements[(key, j)] = {"page": name, "x": x, "y": y}
            image = Image.fromarray(canvas, "RGBA" if page_key == "rgba" else "P")
            if page_key != "rgba":
                image.putpalette(palettes[page_key])
            files[name] = png_bytes(image)
    return files, placements


def build_index(pkg: Package, files: dict[str, bytes], placements: dict[tuple[str, int], dict]) -> bytes:
    records = []
    for key, rec in sorted(pkg.records.items()):
        out = {k: v for k, v in rec.items() if not k.startswith("_")} | {"category": key.split(":")[0]}
        if rec.get("_material"):
            out["material_package"] = rec["_material"]
        if rec.get("_representation"):
            out["representation"] = rec["_representation"]
        if rec.get("_palette"):
            out["palette"] = rec["_palette"]
        if (key, 0) in placements:
            out["frames"] = [f | {"atlas": placements[(key, j)]} for j, f in enumerate(rec["frames"])]
        records.append(out)
    index = {
        "schema": 1,
        "package": pkg.id,
        "version": pkg.version,
        "identity": pkg.identity,
        "dependencies": [{"id": d.id, "version": d.version, "identity": d.identity} for d in pkg.resolved_deps],
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())},
        "records": records,
    }
    return (json.dumps(index, sort_keys=True, indent=1, ensure_ascii=False) + "\n").encode()


# ---------------------------------------------------------------- preview

def checkerboard(w: int, h: int, cell: int = 8) -> np.ndarray:
    yy, xx = np.mgrid[0:h, 0:w]
    light = ((yy // cell + xx // cell) % 2).astype(bool)
    board = np.where(light[..., None], np.uint8(200), np.uint8(140)).repeat(3, axis=-1)
    return np.concatenate([board, np.full((h, w, 1), 255, np.uint8)], axis=-1)


def composite(dst: np.ndarray, src: np.ndarray, x: int, y: int) -> None:
    h, w = src.shape[:2]
    a = src[..., 3:4].astype(np.float32) / 255
    region = dst[y:y + h, x:x + w]
    region[..., :3] = (src[..., :3] * a + region[..., :3] * (1 - a)).astype(np.uint8)


def material_channel(pkg: Package, rec: dict, channel: str, size: tuple[int, int]) -> np.ndarray | None:
    """The material's `channel` image resized (nearest) to the frame size, or None when omitted."""
    if not rec.get("_material"):
        return None
    owner = pkg if rec["_material"] == pkg.id else next(d for d in pkg.resolved_deps if d.id == rec["_material"])
    mat = owner.records[f"materials:{rec['_material_id']}"]
    if channel not in mat:
        return None
    with Image.open(owner.local_path(mat[channel], "preview")) as im:
        return np.asarray(im.convert("RGBA").resize(size, Image.NEAREST))


def shade(albedo: np.ndarray, normal: np.ndarray | None, emission: np.ndarray | None, light: tuple[float, float, float]) -> np.ndarray:
    """Lambert lighting with a flat +Z normal fallback, zero emission fallback, ambient floor 0.25."""
    lvec = np.array(light, np.float32)
    lvec /= np.linalg.norm(lvec) or 1.0
    if normal is None:
        n = np.zeros(albedo.shape[:2] + (3,), np.float32)
        n[..., 2] = 1.0
    else:
        n = normal[..., :3].astype(np.float32) / 127.5 - 1.0
        n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    intensity = 0.25 + 0.75 * np.clip(n @ lvec, 0, 1)
    rgb = albedo[..., :3].astype(np.float32) * intensity[..., None]
    if emission is not None:
        rgb += emission[..., :3].astype(np.float32) * (emission[..., 3:4] / 255)
    return np.concatenate([np.clip(rgb, 0, 255).astype(np.uint8), albedo[..., 3:4]], axis=-1)


def render_preview(pkg: Package, light: tuple[float, float, float], scale: int) -> Image.Image:
    """Contact sheet on a checkerboard: one row per record, frames unlit on the left and lit on
    the right, pivot marked in magenta. UI frames stay unlit."""
    rows = []
    for key, rec in own_sprites(pkg):
        palette = read_palette(pkg.local_path(rec["frames"][0]["path"], key)) if rec["_representation"] == "indexed" else None
        frames = [to_rgba(frame_pixels(pkg, f), palette) for f in rec["frames"]]
        size = (rec["frames"][0]["width"], rec["frames"][0]["height"])
        if key.startswith("ui_frames:"):
            lit = frames
        else:
            normal, emission = material_channel(pkg, rec, "normal", size), material_channel(pkg, rec, "emission", size)
            lit = [shade(f, normal, emission, light) for f in frames]
        rows.append((key, rec, frames, lit))
    if not rows:
        raise ModkitError(f"{pkg.id}: nothing to preview (no records with own frames)")
    gap, label_h = 4, 12
    columns = max(len(r[2]) for r in rows) + 1  # lit column starts after the widest unlit row
    width = max(2 * columns * (r[2][0].shape[1] + gap) * scale for r in rows) + gap
    height = sum(r[2][0].shape[0] * scale + gap + label_h for r in rows) + gap
    canvas = checkerboard(width, height)
    labels = []
    y = gap
    for key, rec, frames, lit in rows:
        labels.append(((gap, y), f"{key}  {rec['_representation']}  material={rec.get('material', '-')}"))
        y += label_h
        fh, fw = frames[0].shape[:2]
        for column, x in ((frames, gap), (lit, gap + columns * (fw + gap) * scale)):
            for f in column:
                big = np.asarray(Image.fromarray(f).resize((fw * scale, fh * scale), Image.NEAREST))
                composite(canvas, big, x, y)
                if rec.get("pivot"):
                    px, py = x + rec["pivot"]["x"] * scale, y + rec["pivot"]["y"] * scale
                    canvas[py:py + scale, px:px + scale, :3] = (255, 0, 255)
                x += (fw + gap) * scale
        y += fh * scale + gap
    sheet = Image.fromarray(canvas)
    draw = ImageDraw.Draw(sheet)
    for pos, text in labels:
        draw.text(pos, text, fill=(0, 0, 0, 255))
    return sheet


# ---------------------------------------------------------------- MPQ writer (v1, uncompressed)

_CRYPT: list[int] = []


def _crypt():
    if not _CRYPT:
        seed = 0x00100001
        table = [0] * 0x500
        for i in range(0x100):
            j = i
            for _ in range(5):
                seed = (seed * 125 + 3) % 0x2AAAAB
                t1 = (seed & 0xFFFF) << 16
                seed = (seed * 125 + 3) % 0x2AAAAB
                table[j] = t1 | (seed & 0xFFFF)
                j += 0x100
        _CRYPT.extend(table)
    return _CRYPT


def mpq_hash(name: str, kind: int) -> int:
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in name.replace("/", "\\").upper().encode("latin-1"):
        s1 = (_crypt()[(kind << 8) + ch] ^ ((s1 + s2) & 0xFFFFFFFF)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1


def mpq_encrypt(words: list[int], key: int) -> list[int]:
    seed, out = 0xEEEEEEEE, []
    for w in words:
        seed = (seed + _crypt()[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        out.append(w ^ ((key + seed) & 0xFFFFFFFF))
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (w + seed + (seed << 5) + 3) & 0xFFFFFFFF  # seed advances on plaintext
    return out


def write_mpq(files: dict[str, bytes]) -> bytes:
    names = sorted(files)
    hash_count = 1
    while hash_count < len(names) * 2:
        hash_count <<= 1
    header_size, shift = 32, 3
    body, blocks = bytearray(), []
    for name in names:
        data = files[name]
        blocks.append((header_size + len(body), len(data), len(data), 0x80000000))
        body += data
    hashes = [[0xFFFFFFFF] * 4 for _ in range(hash_count)]  # hash_a, hash_b, locale|platform, block index
    for idx, name in enumerate(names):
        slot = mpq_hash(name, 0) % hash_count
        while hashes[slot][3] != 0xFFFFFFFF:
            slot = (slot + 1) % hash_count
        hashes[slot] = [mpq_hash(name, 1), mpq_hash(name, 2), 0, idx]
    hash_words = mpq_encrypt([w for e in hashes for w in e], mpq_hash("(hash table)", 3))
    block_words = mpq_encrypt([w for b in blocks for w in b], mpq_hash("(block table)", 3))
    hash_off = header_size + len(body)
    block_off = hash_off + len(hash_words) * 4
    total = block_off + len(block_words) * 4
    if total > MAX_ARCHIVE_BYTES:
        raise ModkitError(f"archive of {total} bytes exceeds {MAX_ARCHIVE_BYTES}")
    header = struct.pack("<4sIIHHIIII", b"MPQ\x1a", header_size, total, 0, shift, hash_off, block_off, hash_count, len(blocks))
    return header + bytes(body) + struct.pack(f"<{len(hash_words)}I", *hash_words) + struct.pack(f"<{len(block_words)}I", *block_words)


# ---------------------------------------------------------------- CLI

def cmd_validate(args) -> int:
    pkg, order = load(Path(args.package), args.search_path)
    if pkg.legacy:
        print(f"{pkg.manifest_path}: legacy package (no [schema]); not validated against schema 1")
        return 0
    print(f"ok: {pkg.id} {pkg.version} new-schema identity={pkg.identity}")
    for d in order:
        if d is not pkg:
            print(f"  dependency {d.id} {d.version} identity={d.identity}")
    return 0


def require_new_schema(args) -> Package:
    pkg, _ = load(Path(args.package), args.search_path)
    if pkg.legacy:
        raise ModkitError(f"{pkg.manifest_path}: legacy packages are not handled by modkit {args.cmd}")
    return pkg


def cmd_build(args) -> int:
    pkg = require_new_schema(args)
    files = collect_files(pkg)
    pages, placements = pack_atlases(pkg)
    files.update(pages)
    files["index.json"] = build_index(pkg, files, placements)
    mpq = write_mpq(files)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(mpq)
    digest = hashlib.sha256(mpq).hexdigest()
    Path(str(out) + ".sha256").write_text(f"{digest}  {out.name}\n")  # sidecar avoids hashing a self-reference
    print(f"wrote {out} ({len(mpq)} bytes, {len(files)} files, {len(pages)} atlas pages) sha256={digest} identity={pkg.identity}")
    return 0


def cmd_preview(args) -> int:
    pkg = require_new_schema(args)
    light = tuple(float(v) for v in args.light.split(",")) if args.light.count(",") == 2 else ()
    if len(light) != 3:
        raise ModkitError(f"--light must be X,Y,Z (got {args.light!r})")
    sheet = render_preview(pkg, light, args.scale)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"wrote {out} ({sheet.width}x{sheet.height}) light={light}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="modkit", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("validate", cmd_validate), ("build", cmd_build), ("preview", cmd_preview)):
        p = sub.add_parser(name)
        p.add_argument("package", help="package directory or its manifest.ini")
        p.add_argument("--search-path", action="append", default=[], help="directory containing dependency packages, or a package (default: package's parent)")
        p.set_defaults(fn=fn)
        if name == "build":
            p.add_argument("--output", required=True, help="output .mpq path")
        if name == "preview":
            p.add_argument("--output", required=True, help="output .png contact sheet")
            p.add_argument("--light", default="-0.5,-0.7,0.5", help="test light direction X,Y,Z in normal-map space (default: upper-left)")
            p.add_argument("--scale", type=int, default=4, help="integer zoom (default 4)")
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except ModkitError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
