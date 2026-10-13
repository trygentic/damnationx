#!/usr/bin/env python3
"""Executable fixture matrix for modkit. Run: python3 test_modkit.py"""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import modkit  # noqa: E402


def png(path: Path, w=16, h=16, mode="RGBA"):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new(mode, (w, h), (1, 2, 3, 255) if mode == "RGBA" else 0).save(path)


def indexed_png(path: Path, palette: bytes, fill: int, w=16, h=16):
    path.parent.mkdir(parents=True, exist_ok=True)
    im = Image.new("P", (w, h), fill)
    im.putpalette(palette)
    im.save(path)


PALETTE_A = bytes(v for i in range(256) for v in (i, i, i))
PALETTE_B = bytes(v for i in range(256) for v in (255 - i, 0, i))


def frame(path, w=16, h=16, **kw):
    return {"path": path, "width": w, "height": h, **kw}


def write_pkg(root: Path, name: str, manifest: str, assets: dict | None, images=()):
    d = root / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "manifest.ini").write_text(manifest)
    if assets is not None:
        (d / "assets.json").write_text(json.dumps(assets))
    for img in images:
        png(d / img)
    return d


BASE_MANIFEST = "[schema]\nversion = 1\n[package]\nid = base.art\nversion = 1.2.0\naliases = base\n[capabilities]\nprovides = terrain,monster\n"
BASE_ASSETS = {
    "schema": 1,
    "terrain": [{"id": "dungeon.floor", "frames": [frame("t/floor.png")], "pivot": {"x": 8, "y": 8}, "material": "stone"}],
    "player": [{"id": "player.warrior", "frames": [frame("p/w.png", direction="s")], "pivot": {"x": 8, "y": 15}}],
    "equipment": [{"id": "equip.sword", "frames": [frame("e/s.png")]}],
    "monster": [{"id": "monster.gloomling", "frames": [frame("m/g.png"), frame("m/g.png", x=0, y=0)], "material": "organic"}],
    "item": [{"id": "item.shard", "frames": [frame("i/s.png")]}],
    "spell": [{"id": "spell.bolt", "frames": [frame("s/b.png")]}],
    "materials": [{"id": "stone", "albedo": "t/floor.png", "normal": "t/floor.normal.png"}, {"id": "organic"}],
    "ui_frames": [{"id": "ui.book", "frames": [frame("u/b.png")], "material": "stone"}],
}
BASE_IMAGES = ["t/floor.png", "t/floor.normal.png", "p/w.png", "e/s.png", "m/g.png", "i/s.png", "s/b.png", "u/b.png"]


def manifest(pid, version="1.0.0", deps=(), aliases=""):
    m = f"[schema]\nversion = 1\n[package]\nid = {pid}\nversion = {version}\n"
    if aliases:
        m += f"aliases = {aliases}\n"
    if deps:
        m += "[dependencies]\n" + "".join(f"{k} = {v}\n" for k, v in deps)
    return m


class Fixtures(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.base = write_pkg(self.tmp, "base", BASE_MANIFEST, BASE_ASSETS, BASE_IMAGES)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def load(self, d):
        return modkit.load(d, [])

    def rejects(self, d, fragment):
        with self.assertRaises(modkit.ModkitError) as cm:
            self.load(d)
        self.assertIn(fragment, str(cm.exception), str(cm.exception))

    def test_accepted_all_eight_arrays(self):
        pkg, order = self.load(self.base)
        self.assertEqual([p.id for p in order], ["base.art"])
        self.assertEqual(len(pkg.records), 9)
        self.assertEqual(pkg.records["ui_frames:ui.book"]["_material"], "base.art")

    def test_legacy_manifest_is_not_upgraded(self):
        d = write_pkg(self.tmp, "legacy", "[mod]\nname=Old\n", {"schema": 1})
        pkg, _ = self.load(d)
        self.assertTrue(pkg.legacy)

    def test_partial_override_inherits_fields_and_replaces_arrays(self):
        d = write_pkg(self.tmp, "over", manifest("over.pack", deps=[("base", ">=1.0.0 <2.0.0")]),
                      {"schema": 1, "terrain": [{"id": "dungeon.floor", "frames": [frame("f.png", 32, 32), frame("f.png", 32, 32)]}],
                       "player": [{"id": "player.warrior", "material": "stone"}]},
                      ["f.png"])
        png(d / "f.png", 32, 32)
        pkg, order = self.load(d)
        self.assertEqual([p.id for p in order], ["base.art", "over.pack"])
        floor = pkg.records["terrain:dungeon.floor"]
        self.assertEqual(len(floor["frames"]), 2)
        self.assertEqual(floor["pivot"], {"x": 8, "y": 8})       # inherited
        self.assertEqual(floor["_material"], "base.art")          # material resolved via dependency
        self.assertEqual(pkg.records["player:player.warrior"]["frames"][0]["direction"], "s")

    def test_same_package_material_wins_over_dependency(self):
        d = write_pkg(self.tmp, "mine", manifest("mine", deps=[("base.art", "=1.2.0")]),
                      {"schema": 1, "materials": [{"id": "stone"}], "item": [{"id": "x", "frames": [frame("x.png")], "material": "stone"}]},
                      ["x.png"])
        pkg, _ = self.load(d)
        self.assertEqual(pkg.records["item:x"]["_material"], "mine")

    def test_qualified_material_and_bad_explicit_reference(self):
        d = write_pkg(self.tmp, "q", manifest("q", deps=[("base", ">=1.0.0")]),
                      {"schema": 1, "item": [{"id": "x", "frames": [frame("x.png")], "material": "base:stone"}]}, ["x.png"])
        self.assertEqual(self.load(d)[0].records["item:x"]["_material"], "base.art")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame("x.png")], "material": "nope"}]}))
        self.rejects(d, "material 'nope' not found")

    def test_missing_normal_and_emission_are_fine_but_present_paths_must_exist(self):
        d = write_pkg(self.tmp, "m", manifest("m"), {"schema": 1, "materials": [{"id": "a"}, {"id": "b", "emission": "e.png"}]})
        self.rejects(d, "file not found")
        png(d / "e.png")
        self.load(d)

    def test_invalid_dimensions_and_pivots(self):
        d = write_pkg(self.tmp, "bad", manifest("bad"), {"schema": 1, "item": [{"id": "x", "frames": [frame("x.png", 16, 32)]}]}, ["x.png"])
        self.rejects(d, "exceeds sheet")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame("x.png"), frame("x.png", 8, 8)]}]}))
        self.rejects(d, "differ from frame 0")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame("x.png")], "pivot": {"x": 16, "y": 0}}]}))
        self.rejects(d, "pivot (16,0) outside")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame("x.png", direction="up")]}]}))
        self.rejects(d, "bad animation direction")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame("x.png")], "pivot": None}]}))
        self.rejects(d, "explicit null")

    def test_sheet_frames_charge_the_decode_budget_once(self):
        # 64 frames cut from one 1024x1024 sheet: 4 MiB decoded once, not 256 MiB per reference.
        frames = [frame("sheet.png", 128, 128, x=(k % 8) * 128, y=(k // 8) * 128) for k in range(64)]
        d = write_pkg(self.tmp, "sheet", manifest("sheet"), {"schema": 1, "monster": [{"id": "m", "frames": frames}]})
        png(d / "sheet.png", 1024, 1024)
        old = modkit.MAX_DECODED_BYTES
        modkit.MAX_DECODED_BYTES = 8 * 1024 * 1024
        try:
            pkg, _ = self.load(d)
        finally:
            modkit.MAX_DECODED_BYTES = old
        self.assertEqual(len(pkg.records["monster:m"]["frames"]), 64)

    def test_duplicate_and_case_colliding_ids(self):
        d = write_pkg(self.tmp, "dup", manifest("dup"), {"schema": 1, "item": [{"id": "A.b", "frames": [frame("x.png")]}, {"id": "a/B", "frames": [frame("x.png")]}]}, ["x.png"])
        self.rejects(d, "duplicate logical ID")

    def test_path_traversal_and_absolute_paths(self):
        d = write_pkg(self.tmp, "esc", manifest("esc"), {"schema": 1, "item": [{"id": "x", "frames": [frame("../base/t/floor.png")]}]})
        self.rejects(d, "path traversal")
        (d / "assets.json").write_text(json.dumps({"schema": 1, "item": [{"id": "x", "frames": [frame(str(self.base / "t/floor.png"))]}]}))
        self.rejects(d, "package-local")

    def test_missing_dependency_cycle_and_bad_constraint(self):
        d = write_pkg(self.tmp, "nodep", manifest("nodep", deps=[("ghost", ">=1.0.0")]), {"schema": 1})
        self.rejects(d, "missing dependency 'ghost'")
        a = write_pkg(self.tmp, "cyc.a", manifest("cyc.a", deps=[("cyc.b", ">=1.0.0")]), {"schema": 1})
        write_pkg(self.tmp, "cyc.b", manifest("cyc.b", deps=[("cyc.a", ">=1.0.0")]), {"schema": 1})
        self.rejects(a, "dependency cycle")
        c = write_pkg(self.tmp, "con", manifest("con", deps=[("base", ">=2.0.0 <1.0.0")]), {"schema": 1})
        self.rejects(c, "contradictory")
        (c / "manifest.ini").write_text(manifest("con", deps=[("base", "~1.0")]))
        self.rejects(c, "invalid SemVer")
        (c / "manifest.ini").write_text(manifest("con", deps=[("base", ">=2.0.0")]))
        self.rejects(c, "does not satisfy")

    def test_malformed_manifest_never_falls_back_to_legacy(self):
        d = write_pkg(self.tmp, "bad", "[schema]\nversion = 2\n[package]\nid = x\nversion = 1.0.0\n", {"schema": 1})
        self.rejects(d, "unsupported schema version")
        (d / "manifest.ini").write_text("[schema]\nversion = 1\n[package]\nid = x\nversion = 1.0\n")
        self.rejects(d, "invalid SemVer")
        (d / "manifest.ini").write_text("[schema]\nversion = 1\nversion = 1\n[package]\nid = x\nversion = 1.0.0\n")
        self.rejects(d, "duplicate key")
        (d / "manifest.ini").write_bytes(b"[schema]\nversion = 1\n\xff\n")
        self.rejects(d, "invalid UTF-8")

    def test_identity_depends_on_capabilities_not_paths(self):
        pkg, _ = self.load(self.base)
        moved = self.tmp / "elsewhere" / "base"
        shutil.copytree(self.base, moved)
        pkg2, _ = self.load(moved)
        self.assertEqual(pkg.identity, pkg2.identity)
        (moved / "manifest.ini").write_text(BASE_MANIFEST.replace("terrain,monster", "terrain"))
        self.assertNotEqual(pkg.identity, self.load(moved)[0].identity)

    def test_build_is_deterministic_and_mpq_is_readable(self):
        out = self.tmp / "out" / "base.mpq"
        self.assertEqual(modkit.main(["build", str(self.base), "--output", str(out)]), 0)
        first = out.read_bytes()
        os.utime(self.base / "manifest.ini", (0, 0))
        modkit.main(["build", str(self.base), "--output", str(out)])
        self.assertEqual(first, out.read_bytes())
        self.assertIn(modkit.hashlib.sha256(first).hexdigest(), (out.parent / "base.mpq.sha256").read_text())
        files = read_mpq(first)
        self.assertEqual(files["manifest.ini"], (self.base / "manifest.ini").read_bytes())
        self.assertEqual(files["t/floor.png"], (self.base / "t/floor.png").read_bytes())
        index = json.loads(files["index.json"])
        self.assertEqual(index["identity"], self.load(self.base)[0].identity)
        self.assertEqual(len(index["records"]), 9)
        floor = next(r for r in index["records"] if r["id"] == "dungeon.floor")
        self.assertEqual(floor["representation"], "rgba")
        self.assertEqual(floor["frames"][0]["atlas"]["page"], "atlas/rgba-0.png")
        page = Image.open(modkit.BytesIO(files["atlas/rgba-0.png"]))
        self.assertEqual(page.mode, "RGBA")
        placed = {(r["id"], f["atlas"]["x"], f["atlas"]["y"]) for r in index["records"] if "frames" in r for f in r["frames"]}
        self.assertEqual(len(placed), 8, "every frame gets its own atlas slot")
        p = floor["frames"][0]["atlas"]
        self.assertEqual(page.getpixel((p["x"], p["y"])), (1, 2, 3, 255))

    def test_indexed_frames_are_reencoded_into_palette_pages(self):
        d = write_pkg(self.tmp, "idx", manifest("idx"), {"schema": 1, "monster": [
            {"id": "m.a", "frames": [frame("a0.png"), frame("a1.png")]},
            {"id": "m.b", "frames": [frame("b.png", 8, 8)]}]})
        indexed_png(d / "a0.png", PALETTE_A, 7)
        indexed_png(d / "a1.png", PALETTE_A, 9)
        indexed_png(d / "b.png", PALETTE_B, 3, 8, 8)
        pkg, _ = self.load(d)
        self.assertEqual(pkg.records["monster:m.a"]["_representation"], "indexed")
        self.assertNotEqual(pkg.records["monster:m.a"]["_palette"], pkg.records["monster:m.b"]["_palette"])
        pages, placements = modkit.pack_atlases(pkg)
        self.assertEqual(len(pages), 2, "one indexed page set per palette")
        (name, data), = [(n, b) for n, b in pages.items() if placements[("monster:m.a", 0)]["page"] == n]
        page = Image.open(modkit.BytesIO(data))
        self.assertEqual(page.mode, "P")
        self.assertEqual(bytes(page.getpalette("RGB"))[:768], PALETTE_A)
        a0, a1 = placements[("monster:m.a", 0)], placements[("monster:m.a", 1)]
        self.assertEqual(page.getpixel((a0["x"], a0["y"])), 7)
        self.assertEqual(page.getpixel((a1["x"], a1["y"])), 9)
        self.assertNotEqual((a0["x"], a0["y"]), (a1["x"], a1["y"]))
        rgba = modkit.to_rgba(np.asarray(page)[a0["y"]:a0["y"] + 16, a0["x"]:a0["x"] + 16], PALETTE_A)
        self.assertEqual(tuple(rgba[0, 0]), (7, 7, 7, 255))
        self.assertEqual(tuple(modkit.to_rgba(np.zeros((2, 2), np.uint8), PALETTE_A)[0, 0]), (0, 0, 0, 0), "index 0 is transparent")

    def test_mixed_representations_are_rejected(self):
        d = write_pkg(self.tmp, "mix", manifest("mix"), {"schema": 1, "item": [{"id": "x", "frames": [frame("i.png"), frame("r.png")]}]}, ["r.png"])
        indexed_png(d / "i.png", PALETTE_A, 1)
        self.rejects(d, "mixes indexed and RGBA")
        indexed_png(d / "r.png", PALETTE_B, 1)
        self.rejects(d, "different palette")

    def test_atlas_shelves_wrap_and_pages_split(self):
        d = write_pkg(self.tmp, "big", manifest("big"), {"schema": 1, "item": [
            {"id": "wide", "frames": [frame("w.png", 1500, 10)] * 2},
            {"id": "tall", "frames": [frame("t.png", 10, 2040)] * 2}]})
        png(d / "w.png", 1500, 10)
        png(d / "t.png", 10, 2040)
        pkg, _ = self.load(d)
        pages, placements = modkit.pack_atlases(pkg)
        self.assertGreaterEqual(len(pages), 2)
        boxes = [(p["page"], p["x"], p["y"]) for p in placements.values()]
        self.assertEqual(len(set(boxes)), 4)
        for data in pages.values():
            im = Image.open(modkit.BytesIO(data))
            self.assertLessEqual(max(im.size), modkit.ATLAS_PAGE_DIM)

    def test_preview_renders_lit_and_unlit_columns(self):
        out = self.tmp / "preview.png"
        self.assertEqual(modkit.main(["preview", str(self.base), "--output", str(out), "--scale", "2", "--light", "1,0,1"]), 0)
        im = Image.open(out)
        self.assertEqual(im.mode, "RGBA")
        self.assertGreater(im.width, 100)
        self.assertEqual(modkit.main(["preview", str(self.base), "--output", str(out), "--light", "bad"]), 1)
        # a shaded frame with a flat normal and light from straight ahead keeps full albedo
        albedo = np.full((2, 2, 4), (100, 100, 100, 255), np.uint8)
        self.assertEqual(tuple(modkit.shade(albedo, None, None, (0, 0, 1))[0, 0]), (100, 100, 100, 255))
        self.assertLess(modkit.shade(albedo, None, None, (1, 0, 0))[0, 0, 0], 100)

    def test_search_path_may_point_at_a_package(self):
        d = write_pkg(self.tmp / "nested" / "deep", "over", manifest("over", deps=[("base", ">=1.0.0")]),
                      {"schema": 1, "terrain": [{"id": "dungeon.floor"}]})
        pkg, order = modkit.load(d, [str(self.base)])
        self.assertEqual([p.id for p in order], ["base.art", "over"])
        self.assertEqual(pkg.records["terrain:dungeon.floor"]["frames"], BASE_ASSETS["terrain"][0]["frames"])

    def test_repository_sample_pack_validates_builds_and_previews(self):
        repo = Path(__file__).resolve().parents[2]
        pkg, _ = modkit.load(repo, [])
        self.assertEqual(pkg.id, "damnationx.original.samples")
        self.assertEqual(len(pkg.records), 12)
        partial, order = modkit.load(repo / "assets/samples/partial", [str(repo)])
        self.assertEqual([p.id for p in order], ["damnationx.original.samples", "damnationx.partial.samples"])
        self.assertEqual(len(partial.records["monster:monster.gloomling"]["frames"]), 4)
        self.assertEqual(partial.records["item:item.starshard"]["_material"], "damnationx.original.samples")
        out = self.tmp / "samples.mpq"
        self.assertEqual(modkit.main(["build", str(repo / "manifest.ini"), "--output", str(out)]), 0)
        files = read_mpq(out.read_bytes())
        self.assertIn("atlas/rgba-0.png", files)
        self.assertEqual(modkit.main(["preview", str(repo), "--output", str(self.tmp / "samples.png")]), 0)


def read_mpq(data: bytes) -> dict[str, bytes]:
    """Independent decode of the archive using the standard MPQ v1 layout."""
    sig, hsize, asize, ver, shift, hoff, boff, hcount, bcount = struct.unpack_from("<4sIIHHIIII", data, 0)
    assert sig == b"MPQ\x1a" and asize == len(data)
    hashes = decrypt(list(struct.unpack_from(f"<{hcount * 4}I", data, hoff)), modkit.mpq_hash("(hash table)", 3))
    blocks = decrypt(list(struct.unpack_from(f"<{bcount * 4}I", data, boff)), modkit.mpq_hash("(block table)", 3))
    index = json.loads(find(data, hashes, blocks, hcount, "index.json"))
    return {name: find(data, hashes, blocks, hcount, name) for name in index["files"]} | {"index.json": find(data, hashes, blocks, hcount, "index.json")}


def find(data, hashes, blocks, hcount, name):
    slot = modkit.mpq_hash(name, 0) % hcount
    a, b = modkit.mpq_hash(name, 1), modkit.mpq_hash(name, 2)
    for _ in range(hcount):
        e = hashes[slot * 4:slot * 4 + 4]
        assert e[3] != 0xFFFFFFFF, f"{name} not in archive"
        if e[0] == a and e[1] == b:
            off, csize, fsize, flags = blocks[e[3] * 4:e[3] * 4 + 4]
            assert flags == 0x80000000 and csize == fsize
            return data[off:off + fsize]
        slot = (slot + 1) % hcount
    raise AssertionError(f"{name}: probe wrapped")


def decrypt(words, key):
    seed, out = 0xEEEEEEEE, []
    for w in words:
        seed = (seed + modkit._crypt()[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        c = w ^ ((key + seed) & 0xFFFFFFFF)
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (c + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out.append(c)
    return out


if __name__ == "__main__":
    unittest.main()
