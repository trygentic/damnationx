#!/usr/bin/env python3
"""Validate a DamnationX static mesh placement file and the meshes it names.

Usage: tools/placement/validate_placement.py <placement.ini> [--pack <HD pack root>] [--json]

Checks the same rules as the game's parser (Source/render3d/placement.cpp, docs/design/static-mesh-placement.md), then,
with --pack, opens every mesh (path relative to the pack root, e.g. hd/3d/town/tavern.glb) and reports its sha256, its
bounds in world units after each placement's transform, ground contact and footprint against the hidden tile rects.

Never writes anything. Mesh checks need NumPy (tools/placement/inspect_glb.py, from Douglas Pan's diablo-3d).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path

GRID = 112
METERS_TO_WORLD = 0.36
ID = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
ASSET = re.compile(r"^[A-Za-z0-9/._-]{1,256}$")
SHA = re.compile(r"^[0-9a-f]{64}$")


class PlacementError(Exception):
    pass


def fail(line: int, message: str) -> None:
    raise PlacementError(f"line {line}: {message}")


def parse(text: str) -> dict:
    """Mirror of ParsePlacementFile: returns {meshes, instances, rules} or raises PlacementError."""
    if text.startswith("﻿"):
        text = text[1:]
    sections: list[dict] = []
    for number, raw in enumerate(text.split("\n"), start=1):
        line = re.split(r"[;#]", raw, maxsplit=1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            if not line.endswith("]"):
                fail(number, "malformed section header")
            sections.append({"name": line[1:-1].strip(), "line": number, "entries": []})
            continue
        if "=" not in line:
            fail(number, "expected key = value")
        if not sections:
            fail(number, "key outside a section")
        key, value = line.split("=", 1)
        sections[-1]["entries"].append((key.strip(), value.strip(), number))
    if not sections or sections[0]["name"] != "Placement":
        fail(sections[0]["line"] if sections else 1, "the file must start with [Placement]")
    names = set()
    for section in sections:
        if section["name"] in names:
            fail(section["line"], f"duplicate section [{section['name']}]")
        names.add(section["name"])
    header = {key: value for key, value, _ in sections[0]["entries"]}
    if set(header) - {"format", "version"} or header.get("format") != "damnationx.placement" or header.get("version") != "1":
        fail(sections[0]["line"], "expected format = damnationx.placement and version = 1")

    result = {"environment": {}, "meshes": {}, "instances": [], "rules": []}

    def check_keys(section: dict, allowed: set[str], repeatable: set[str] = frozenset()) -> None:
        seen = set()
        for key, _, number in section["entries"]:
            if key not in allowed:
                fail(number, f"unknown key '{key}'")
            if key in seen and key not in repeatable:
                fail(number, f"duplicate key '{key}'")
            seen.add(key)

    def number(value: str, line: int, what: str) -> float:
        try:
            parsed = float(value)
        except ValueError:
            fail(line, what)
        if not math.isfinite(parsed):
            fail(line, what)
        return parsed

    def integer(value: str, line: int, what: str) -> int:
        if not re.fullmatch(r"[+-]?\d+", value.strip()):
            fail(line, what)
        return int(value)

    def common(entry: tuple, item: dict) -> bool:
        key, value, line = entry
        if key == "mesh":
            if value not in result["meshes"]:
                fail(line, f"unknown mesh '{value}' (declare [mesh.<id>] before using it)")
            item["mesh"] = value
        elif key == "offset":
            parts = value.split(",")
            if len(parts) != 3:
                fail(line, "offset must be x,y,z within +-64")
            item["offset"] = [number(p, line, "offset must be x,y,z within +-64") for p in parts]
            if any(abs(v) > 64 for v in item["offset"]):
                fail(line, "offset must be x,y,z within +-64")
        elif key == "yaw":
            item["yaw"] = number(value, line, "yaw must be degrees within +-360")
            if abs(item["yaw"]) > 360:
                fail(line, "yaw must be degrees within +-360")
        elif key == "scale":
            what = "scale must be s or x,y,z, each in (0, 100]"
            parts = value.split(",")
            if len(parts) not in (1, 3):
                fail(line, what)
            values = [number(p, line, what) for p in parts]
            if not all(0 < v <= 100 for v in values):
                fail(line, what)
            item["scale"] = values * 3 if len(values) == 1 else values
        elif key == "occluder":
            if value in ("auto", "none"):
                item["occluder"] = value
            else:
                top = number(value, line, "occluder must be auto, none or a height in (0, 60]")
                if not 0 < top <= 60:
                    fail(line, "occluder must be auto, none or a height in (0, 60]")
                item["occluder"] = top
        elif key == "levels":
            parts = value.split("-")
            if len(parts) not in (1, 2):
                fail(line, "levels must be N or N-M")
            lo = integer(parts[0], line, "levels must be N or N-M")
            hi = integer(parts[-1], line, "levels must be N or N-M")
            if lo < 0 or hi < lo or hi > 255:
                fail(line, "levels out of range")
            item["levels"] = [lo, hi]
        elif key == "setlevel":
            item["setlevel"] = integer(value, line, "setlevel must be 0 (normal levels) or a set level number")
        else:
            return False
        return True

    def tile(value: int) -> bool:
        return 0 <= value < GRID

    for section in sections[1:]:
        if section["name"] == "environment":
            check_keys(section, {"ambient", "exposure", "moon_direction", "moon_strength"})
            for key, value, line in section["entries"]:
                if key == "moon_direction":
                    parts = value.split(",")
                    what = "moon_direction must be x,height,z within +-4 with height > 0"
                    if len(parts) != 3:
                        fail(line, what)
                    x, height, z = (number(p, line, what) for p in parts)
                    if height <= 0.0001 or abs(x) > 4 or height > 4 or abs(z) > 4:
                        fail(line, what)
                else:
                    low = 0.1 if key == "exposure" else 0.0
                    if not low <= number(value, line, f"{key} must be in [{low:g}, 4]") <= 4:
                        fail(line, f"{key} must be in [{low:g}, 4]")
            result["environment"] = {key: value for key, value, _ in section["entries"]}
            continue
        kind, _, ident = section["name"].partition(".")
        if not ID.match(ident):
            fail(section["line"], "section must be [mesh.<id>], [instance.<id>] or [rule.<id>]")
        if kind == "mesh":
            check_keys(section, {"file", "units", "sha256"})
            mesh = {"id": ident, "units": "tiles", "sha256": ""}
            for key, value, line in section["entries"]:
                if key == "file":
                    if not ASSET.match(value) or value.startswith("/") or ".." in value or "//" in value or not value.endswith((".glb", ".gltf")):
                        fail(line, "file must be a relative .glb / .gltf asset path")
                    mesh["file"] = value
                elif key == "units":
                    if value not in ("tiles", "meters"):
                        fail(line, "units must be tiles or meters")
                    mesh["units"] = value
                else:
                    if not SHA.match(value):
                        fail(line, "sha256 must be 64 lowercase hex digits")
                    mesh["sha256"] = value
            if "file" not in mesh:
                fail(section["line"], "[mesh.*] needs file")
            result["meshes"][ident] = mesh
        elif kind == "instance":
            check_keys(section, {"mesh", "tile", "offset", "yaw", "scale", "hide", "occluder", "levels", "setlevel"}, {"hide"})
            item = {"id": ident, "offset": [0.5, 0, 0.5], "yaw": 0.0, "scale": [1.0] * 3, "occluder": "auto", "hide": [], "line": section["line"]}
            for entry in section["entries"]:
                if common(entry, item):
                    continue
                key, value, line = entry
                if key == "tile":
                    parts = value.split(",")
                    if len(parts) != 2:
                        fail(line, "tile must be x,y inside the 112 x 112 grid")
                    item["tile"] = [integer(p, line, "tile must be x,y inside the 112 x 112 grid") for p in parts]
                    if not all(tile(v) for v in item["tile"]):
                        fail(line, "tile must be x,y inside the 112 x 112 grid")
                else:
                    parts = value.split(",")
                    what = "hide must be an inclusive tile rect x0,y0,x1,y1 inside the 112 x 112 grid"
                    if len(parts) != 4:
                        fail(line, what)
                    rect = [integer(p, line, what) for p in parts]
                    if not all(tile(v) for v in rect) or rect[2] < rect[0] or rect[3] < rect[1]:
                        fail(line, what)
                    item["hide"].append(rect)
            if "mesh" not in item or "tile" not in item:
                fail(section["line"], "[instance.*] needs mesh and tile")
            result["instances"].append(item)
        elif kind == "rule":
            check_keys(section, {"mesh", "object", "piece", "arch", "offset", "yaw", "scale", "hide", "occluder", "levels", "setlevel", "facing", "show"})
            item = {"id": ident, "offset": [0.5, 0, 0.5], "yaw": 0.0, "scale": [1.0] * 3, "occluder": "auto", "hideSelf": False,
                    "facing": "none", "show": "always", "line": section["line"]}
            for entry in section["entries"]:
                if common(entry, item):
                    continue
                key, value, line = entry
                if key == "object":
                    names = [p.strip() for p in value.split(",")]
                    if not all(n.startswith("OBJ_") and ID.match(n) for n in names):
                        fail(line, "object must be a list of _object_id names such as OBJ_CHEST1, OBJ_TCHEST1")
                    item["object"] = names
                elif key == "arch":
                    if value != "any":
                        fail(line, "arch must be any")
                    item["arch"] = True
                elif key == "facing":
                    if value not in ("none", "open_side", "passage"):
                        fail(line, "facing must be none, open_side or passage")
                    item["facing"] = value
                elif key == "show":
                    if value not in ("always", "unopened", "closed"):
                        fail(line, "show must be always, unopened or closed")
                    item["show"] = value
                elif key == "piece":
                    item["pieces"] = [integer(p, line, "piece must be a list of dPiece numbers") for p in value.split(",")]
                    if any(not 0 <= p <= 0xFFFF for p in item["pieces"]):
                        fail(line, "piece must be a list of dPiece numbers")
                else:
                    if value != "self":
                        fail(line, "rules only accept hide = self")
                    item["hideSelf"] = True
            if "mesh" not in item:
                fail(section["line"], "[rule.*] needs mesh")
            if sum(key in item for key in ("object", "pieces", "arch")) != 1:
                fail(section["line"], "[rule.*] needs exactly one of object, piece or arch")
            if item["show"] != "always" and "object" not in item:
                fail(section["line"], "show applies to object rules only")
            result["rules"].append(item)
        else:
            fail(section["line"], f"unknown section kind '{kind}'")
    return result


def transform(point, item: dict, units: str, tile_xy) -> list[float]:
    """PlacementModelMatrix: T(tile corner + offset) * RotationY(yaw) * S(scale * unit scale)."""
    unit = METERS_TO_WORLD if units == "meters" else 1.0
    x, y, z = (c * s * unit for c, s in zip(point, item["scale"]))
    yaw = math.radians(item["yaw"])
    rx = x * math.cos(yaw) + z * math.sin(yaw)
    rz = -x * math.sin(yaw) + z * math.cos(yaw)
    return [tile_xy[0] + item["offset"][0] + rx, item["offset"][1] + y, tile_xy[1] + item["offset"][2] + rz]


def check_meshes(placement: dict, pack: Path) -> list[dict]:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import inspect_glb  # noqa: E402  (needs NumPy)

    reports = []
    bounds_by_mesh = {}
    for ident, mesh in placement["meshes"].items():
        path = pack / mesh["file"]
        report = {"mesh": ident, "file": mesh["file"]}
        if not path.is_file():
            report["error"] = "missing from the pack (its placements keep the tile geometry)"
            reports.append(report)
            continue
        data = path.read_bytes()
        report["sha256"] = hashlib.sha256(data).hexdigest()
        if mesh["sha256"] and mesh["sha256"] != report["sha256"]:
            report["error"] = f"sha256 mismatch: file has {report['sha256']}, placement pins {mesh['sha256']}"
        try:
            document, binary, _ = inspect_glb.read_glb(path)
            arrays, _, warnings = inspect_glb.flatten(inspect_glb.Reader(path, document, binary), allow_skinned=True)
            vertices = arrays["world_vertices"]
            lo, hi = vertices.min(axis=0).tolist(), vertices.max(axis=0).tolist()
            report.update({"triangles": int(len(arrays["triangles"])), "boundsMin": lo, "boundsMax": hi, "warnings": warnings})
            if document.get("skins"):
                report.setdefault("notes", []).append("has a skin: drawn (and checked here) in its rest pose")
            scale = METERS_TO_WORLD if mesh["units"] == "meters" else 1.0
            if abs(lo[1]) * scale > 0.05:
                report.setdefault("notes", []).append(f"lowest point is y = {lo[1]:.3f} glTF units, not on the ground (origin = ground contact)")
            bounds_by_mesh[ident] = (lo, hi)
        except Exception as error:  # noqa: BLE001  (report any inspector failure per mesh)
            report["error"] = f"unreadable: {error}"
        reports.append(report)

    for item in placement["instances"]:
        if item["mesh"] not in bounds_by_mesh:
            continue
        lo, hi = bounds_by_mesh[item["mesh"]]
        corners = [transform((x, y, z), item, placement["meshes"][item["mesh"]]["units"], item["tile"])
                   for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
        world_lo = [min(c[i] for c in corners) for i in range(3)]
        world_hi = [max(c[i] for c in corners) for i in range(3)]
        entry = {"instance": item["id"], "worldMin": [round(v, 3) for v in world_lo], "worldMax": [round(v, 3) for v in world_hi]}
        if item["hide"]:
            hx0 = min(r[0] for r in item["hide"])
            hy0 = min(r[1] for r in item["hide"])
            hx1 = max(r[2] for r in item["hide"]) + 1
            hy1 = max(r[3] for r in item["hide"]) + 1
            if world_hi[0] < hx0 or world_lo[0] > hx1 or world_hi[2] < hy0 or world_lo[2] > hy1:
                entry["warning"] = "the mesh footprint does not overlap the tiles it hides (wrong tile, yaw or units?)"
        reports.append(entry)
    return reports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("placement", type=Path)
    parser.add_argument("--pack", type=Path, help="HD pack root the mesh paths are relative to")
    parser.add_argument("--json", action="store_true", help="machine-readable report")
    args = parser.parse_args()
    try:
        placement = parse(args.placement.read_text(encoding="utf-8"))
    except (PlacementError, OSError, UnicodeDecodeError) as error:
        print(f"{args.placement}: rejected: {error}", file=sys.stderr)
        return 1
    summary = {"meshes": len(placement["meshes"]), "instances": len(placement["instances"]), "rules": len(placement["rules"])}
    reports = check_meshes(placement, args.pack) if args.pack else []
    problems = [r for r in reports if "error" in r or "warning" in r]
    if args.json:
        print(json.dumps({"file": str(args.placement), "summary": summary, "reports": reports}, indent=2))
    else:
        print(f"{args.placement}: ok ({summary['meshes']} meshes, {summary['instances']} instances, {summary['rules']} rules)")
        for report in reports:
            label = report.get("mesh") or report.get("instance")
            detail = report.get("error") or report.get("warning") or ", ".join(report.get("notes", [])) or "ok"
            print(f"  {label}: {detail}")
    return 1 if any("error" in r for r in reports) else 0


if __name__ == "__main__":
    raise SystemExit(main())
