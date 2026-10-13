# modkit

Validates, packs and previews schema-v1 asset packages.
Python 3 with Pillow and numpy.

```sh
python3 tools/modkit/modkit.py validate mods/torchlight            # or mods/torchlight/manifest.ini
python3 tools/modkit/modkit.py build mods/torchlight --output build/torchlight.mpq
python3 tools/modkit/modkit.py preview mods/torchlight --output build/torchlight.png --light -0.5,-0.7,0.5
python3 tools/modkit/test_modkit.py                                 # executable fixture matrix
```

Dependencies are resolved from sibling directories of the package, or from `--search-path DIR` (repeatable;
a search path may also be a package directory itself). A package without a `[schema]` section is reported as
**legacy** and left alone; a package that declares `[schema]` and fails any check is **rejected** with a
diagnostic naming the file, record, frame and path.

`build` emits a deterministic MPQ v1 (uncompressed sectors, files sorted by path, no timestamps) containing
the manifest, `assets.json`, every referenced source file, generated `atlas/*.png` pages and an `index.json`
(merged records, resolved material owners, representation, atlas placement per frame, per-file sha256).
A `<output>.mpq.sha256` sidecar carries the artifact hash so the archive never hashes itself.

`preview` renders a contact sheet on a checkerboard: one row per record, unlit frames on the left, the same
frames lit on the right with the record material's normal and emission maps (flat normal / zero emission
fallbacks), pivots marked in magenta. `--light X,Y,Z` moves the test light; UI frames stay unlit.

## Wire format

The package schema lists constraints but not byte-level shapes. These are the choices, each localized in `modkit.py`.

| Topic | Choice |
|---|---|
| Frame | `{"path": "sprites/a.png", "x": 0, "y": 0, "width": W, "height": H, "direction": "s"?}`; `x`/`y` default 0; sheet rect must fit the PNG |
| Pivot | record-level `{"x": int, "y": int}`, must lie inside the frame |
| Directions | `s sw w nw n ne e se` |
| Representation | a record's frames are all indexed (`P` PNGs sharing one palette, index 0 transparent) or all RGBA; mixing is rejected |
| Atlases | shelf packing into pages of at most 2048 px with 1 px padding; one page set for RGBA and one per palette (indexed pages keep the palette) |
| Material ref | unqualified `"stone"` (this package first, then deps, ambiguity rejected); qualified `"dep.id:stone"` |
| Partial override | same category + logical id as a dependency record: fields merge, arrays replace whole; explicit `null` rejected |
| Manifest lists | `aliases` / `provides` are comma-separated; duplicate keys rejected |
| Identity | sha256 of the same input string as `VersionedModIdentityInput` (`Source/mods/mod_identity.cpp`), dependency identities in canonical-id order |
| Budgets | 4096 px per side, 1024 frames/record, 256 MiB archive, 512 MiB decoded |

The repository root is the original sample package (`manifest.ini` + `assets.json`); `assets/samples/partial/`
is a partial-override package depending on it (`--search-path .`).
