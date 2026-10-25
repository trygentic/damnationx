# DamnationX art pack

Players download one archive, `damnationx-art-pack-<version>.zip`, containing the
2D HD artwork, 3D characters and props, and FREE 3D models. Import it through
**Asset library > HD art pack > Import ZIP**, or extract it and choose its `hd`
folder. There are no separate FREE and HD downloads to install. D2R game data
and converted D2R artwork are excluded; those remain an optional local import.

The internal directories preserve component overrides and their existing terms:

| Component | Contents | License | Internal path |
| --- | --- | --- | --- |
| FREE 3D | Douglas Opan's diablo-3d models: cottages, huts, church, chest resources, arches, door and brazier | Sustainable Use License 1.0; full NOTICE.md and LICENSE.md retained; free of charge only | `hd/3d/free/` |
| HD 3D | Characters and HD props | Existing DamnationX HD pack terms, retained in its manifest | `hd/3d/hd/`, `hd/3d/warrior/`, `hd/3d/rogue/` |
| 2D HD | Sprites, UI, items and tiles from the existing export | Existing HD art terms | `hd/` |

The combined archive preserves each component's permissions; it does not
relicense the art. In particular, the FREE component's notice prohibits selling
it, putting it behind a paywall, or bundling it with something sold. The archive
contains a root README and `ART-PACK.json` identifying its component manifests.
Art and model bytes stay in ignored local folders and are never committed.

Use the existing WebP export for a smaller complete download. The builder copies
its images and indexes unchanged; it does not recompress or generate 2D artwork.
Back up and replace an existing manually installed `hd` folder instead of merging
PNG and WebP exports. The browser reads `hd/` beside the page or from the selected
local HD folder.

## What the game does

* Neither pack: everything as before (sprites, the built town houses, door panels and arch lintels).
* FREE only: the FREE models.
* HD installed: for every model id the HD pack has, the HD model; the FREE one for the rest.
* A model stands in for the thing it replaces only once it is on the GPU, so a missing or broken file falls back to
  the sprite by itself.

Runtime: the shared placement loader and static mesh renderer (`Source/render3d/placement*`, `static_mesh*`,
[design/static-mesh-placement.md](design/static-mesh-placement.md)). Each pack carries its placement files,
`hd/3d/<tier>/placements/<level type>.ini`; the loader reads hd/3d/placements, then hd/3d/free, then hd/3d/hd and
merges them by section id, so an HD `[mesh.X]` swaps the FREE model X everywhere it is placed.

## Manifest (`hd/3d/<tier>/manifest.json`, written by the build tool)

```json
{
  "schema": "damnationx.3d-pack/1",
  "pack": "damnationx-3d-free", "tier": "free", "version": "20261018",
  "attribution": "...", "license": "...", "max_texture": 1024,
  "models": [
    { "id": "chest_wooden", "file": "models/chest_wooden.glb", "tier": "free",
      "source": "wooden+treasure+chest+3d+model.glb",
      "scale": [0.6, 0.6, 0.6], "pivot": [0, 0, 0], "yaw": 0, "footprint": [0.6, 0.518],
      "height": 0.554, "bounds": { "min": [...], "max": [...] }, "triangles": 4086, "bytes": 5490000, "sha256": "..." }
  ]
}
```

World units are tiles (1 tile = 1 unit, y up, `fpv_contract.hpp`). `scale` maps model units to tiles, `pivot` is the
model-space point placed on the anchor (default: the centre of the base), `yaw` turns the model so its front faces +z,
`footprint` is the collision / stand-in rectangle in tiles. The game reads `id`, `file`, `tier`, `scale`, `pivot`,
`yaw` and `footprint`; the rest is information for people and tools.

## Placement source (`tools/packs/3d/placement.json`)

The pack builder turns these rules into the placement files (damnationx.placement v1). They are keyed to the 2D map:

```json
{ "model": "church_stone", "levels": ["town"], "anchor": "tile", "at": [24.5, 20.5], "scale": [20.05, 10, 17], "replaces": "town_house" }
{ "model": "brazier_metal", "levels": ["cathedral"], "anchor": "object", "objects": ["OBJ_L1LIGHT"] }
{ "model": "door_arch_wooden", "levels": ["cathedral"], "anchor": "door", "objects": ["OBJ_L1LDOOR", "OBJ_L1RDOOR"], "show": "closed" }
{ "model": "arch_stone", "levels": ["cathedral"], "anchor": "arch" }
```

* `tile` becomes an `[instance.*]` at `at` (tile coordinates, integers are tile corners; the model's pivot lands on
  it, folded into `offset`). `replaces: "town_house"` adds `hide` = the tile rectangle under the model's footprint
  and `occluder = auto`. Tristram's 12 houses are covered (footprints from `DVL_W3D_DUMP_TOWN`).
* `object`, `door` and `arch` become `[rule.*]` with `object = ...` or `arch = any`, `facing`, `show`, `hide = self`
  and `occluder = none`.
* The final `scale` is the model's manifest scale times the rule's (`x,y,z` when not uniform).
* The HD pack writes only `[mesh.*]` sections, for the models it redefines.

Chest meshes remain in the combined archive but are not placed automatically.
The old `show = unopened` rules removed the pack chest on interaction and
revealed a differently oriented native or D2R fallback, making it appear to
rotate. Chests now use the same native sprite or D2R model for both states.
Static pack chest replacements can return once matching open variants are
authored; a closed mesh must not conceal the game's opened state.

## Building the combined pack

```bash
pip install pillow
tools/build_3d_packs.py --hd-art /path/to/existing/webp/hd
# One damnationx-art-pack-<version>.zip in assets/packs/ (gitignored).
# --install /path/to/folder-with-hd also unpacks the complete archive for testing.
# --pack free, --pack hd and --pack all retain legacy component build modes.
```

Sources: `assets/downloaded/free/` (FREE), `assets/downloaded/hd/props/` (HD props) and the local
`assets/3d/warrior/*.glb` (the warrior); definitions in `tools/packs/3d/{free,hd}.pack.json` (ids, source files,
scale, yaw, footprint overrides). Textures are re-encoded as PNG, at most `--max-texture` (1024) pixels, because the
browser and native builds decode PNG but not JPEG; the FREE NOTICE.md states this modification as the license requires.
The default combined build requires `--hd-art DIR` and merges the folder's
**2D art only** (sprites, UI, items and other art) with both current 3D tiers.
Directory symlinks in an installed library are followed and their files are packed under the linked names;
broken links and directory cycles stop the build with an error. The existing top-level `3d/` tree is excluded:
models, character manifests and placements always come from the current pack definitions and source models.
This keeps an older combined installation from overwriting new models or restoring obsolete placement rules.
The default writes one combined archive and `SHA256SUMS`; legacy `--pack all`
writes two component archives for developer use. Player distribution uses the
combined archive. A missing 2D sprite/UI index or FREE license notice stops the
combined build. Model source failures stop all builds unless the explicit
`--allow-missing` development option is used.

## Keeping art out of git

`.gitignore` excludes `assets/downloaded/`, `assets/packs/`, `*.blend` and every `.glb`/`.gltf`/`.fbx` under
`assets/`. `tools/check_no_art.py` fails on committed model files under `assets/` and on texture files there that
`tools/art_guard_allowlist.txt` does not list; it runs in CI (`.github/workflows/art_guard.yml`) and as a pre-commit
hook after `git config core.hooksPath tools/hooks`.

## Developer knobs

`DVL_W3D_AMBIENT=0.6` lights a
dungeon for inspection, `DVL_W3D_POS=x,z` / `DVL_W3D_YAW` / `DVL_W3D_PITCH` place the camera (world_renderer.hpp). The
verbose log lists every placed prop per level (`3D props: level ...`).

## 3D characters (HD pack)

The HD pack carries one skinned model per class, built from `assets/downloaded/hd/damnationx_3d_characters_*`
(`characters` in `tools/packs/3d/hd.pack.json`; the .blend sources are never packed):

| File | Model |
| --- | --- |
| `hd/3d/warrior/warrior.glb`, `warrior_1024.glb` | Warrior v10b: 81 meshes, 121 joints, 37 clips |
| `hd/3d/rogue/rogue.glb`, `rogue_1024.glb` | Rogue v24d: 77 meshes, 121 joints, 56 clips |
| `hd/3d/warrior/warrior_viewmodel.glb`, `warrior_viewmodel_1024.glb` | the older warrior (rigid sword and shield) for the first-person weapon |
| `hd/3d/characters.json` | manifest: class, version, file, variants, joints, meshes, clips, sha256 |

The `_1024` variants (textures at most 1024 px) are used in the browser and on Android. `render3d/skinned_renderer`
picks the model by class, shows the meshes of the equipped items (`<Item>_RIGHT` / `_TWOHANDED` in the dungeon,
`_Town_Stowed` in town, `_Sheath_Empty` or the scabbard, `<Shield>_LEFT`, bow parts and quiver) and plays
`<action>_<family>` clips with `<action>` as the fallback. A class without a death clip draws its HD sprite when dead.
