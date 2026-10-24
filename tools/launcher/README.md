# DamnationX launcher

The standalone egui launcher sets up local assets and starts a separately built
DamnationX game executable. Building this crate does not build the C++ game or
the Toxoid host. Launcher distributions contain no owned game art or game archives.

## Native setup

Extract the launcher archive into a writable folder. Keep its `resources` folder
beside the executable when importer recipes are included.

1. Select the game executable under Launch options using Browse when needed. Discovery
   tries a neighboring `damnationx_host` (`damnationx_host.exe` on Windows), then
   legacy `devilutionx`. The chosen path is remembered. The native host's renderer
   choices include 3D game (Toxoid) and Classic game; the legacy executable uses its own
   argument adapter.
   The 3D renderer is labeled **3D Resurrect Update**. Optional raytracing remains
   off by default and offers quality presets through Ultra on supported GPUs.
2. Select the folder containing your own `DIABDAT.MPQ` or `SPAWN.MPQ`. Base game
   readiness is separate from optional HD and D2R art readiness.
3. Choose the art setup you need below, then use Play. Readiness and worker errors
   are shown in the launcher so a missing conversion can be repaired there.

### HD packs

In Asset library, choose an existing HD folder with Use folder…, or install a
local pack with Import ZIP… or Import folder….
The setup accepts the `hd` folder itself, a pack root containing `hd`, or one
archive wrapper directory. A base pack needs usable indexes and the image files
those indexes reference. Import a full base pack first; later Import ZIP or
Import folder actions merge additions into that managed pack. A 3D-only add-on
requires a usable base pack. Use folder selects an external pack; when you then
import an add-on, the launcher first copies that selected base into the managed
library and queues the addition. Your external source remains in place.

Existing HD libraries may contain local links in their root, indexes,
directories or images. Use folder reads and validates those references without
changing their sources. When a validated existing reference becomes the base for
an overlay, the launcher copies the resolved files into regular staging files
before merging the addition. New folder and ZIP imports reject links and unsafe
paths; reference compatibility does not relax those import checks.

Installation uses a managed per-user copy and validates the staged result before
publishing it. Selecting a folder does not grant permission to redistribute its art.
The Use HD art checkbox explicitly disables the pack when off, including an
installed pack that the game would otherwise discover automatically. D2R
Characters and World have separate controls.

### D2R assets

An **owned D2R installation** and a **converted D2R cache** are different inputs:

- The install is the local game directory containing `.build.info` and `Data/`.
  Select it for Import from D2R. CrossOver/Wine installs can be used as local
  sources on macOS/Linux.
- The converted cache contains `glb/mapping.resolved.json`, `glb/env/`,
  `glb/ui/` and `glb/vfx/town_portal/`. Select either that cache parent or its
  `glb` directory to reuse a completed conversion.

The importer runs setup, model mapping, mapping resolution, environment
conversion, UI conversion and Town Portal resource conversion on a background
worker. The launcher checks these components separately and publishes the staged
`cache/glb` tree after its required manifests and referenced files are ready. Its
progress/log view and cancellation action are available during setup.

Existing raw extracts and helper binaries stay in their cache. New raw extracts
needed for an import are isolated in staging and discarded afterward; only
converted playable outputs replace the previous `glb` tree. A working packaged
or cached helper is reused, and a newly built helper can be retained separately.

Import from an owned installation requires:

- Python 3.11 or newer. Select the Python executable if discovery does not find it.
- NumPy `>=1.24,<3` and Pillow `>=10,<13`. The managed virtual-environment option
  creates an isolated Python environment and installs these requirements. That
  dependency installation requires internet access; an existing prepared Python
  environment can be used instead.
- The source importer recipe, discovered at
  `resources/d2r-import` beside the launcher, or `tools/d2r-import` in a checkout.
- A native `d2rtool` helper. Packages can supply it at
  `resources/d2r-import/bin/d2rtool` (`d2rtool.exe` on Windows). Without a prebuilt
  helper, conversion needs CMake 3.20+ and a C/C++17 compiler. The helper build
  fetches a pinned, SHA256-verified CascLib source archive and uses its bundled
  zlib; it does not download game data.

For a prepared environment, install the Python requirements with:

```sh
python3 -m pip install -r tools/d2r-import/requirements.txt
```

On Windows, use the chosen Python executable or `py -3.11` in place of `python3`.
Asset paths and converted bytes stay in local user storage. A D2R import reads
the installation you select; no D2R account sign-in or game-data download occurs.

The converted resources are used by DamnationX's renderer. This does not run the
D2R renderer or its original particle simulation.

### D2R sound packs

Open **Sounds** and select **Import D2R sounds…** to read your owned D2R
installation. This separate action imports both the Classic and Remastered sound
packs through the game's native C++ importer. It does not need Python or a D2R
graphics conversion. Both packs show their imported sample count and readiness.

Choose **Diablo (original)**, **D2R Classic**, or **D2R Remastered** in the sound
pack selector. A successful import selects Remastered; returning to original
audio disables the imported packs. The launcher keeps the native sound option
and enabled sound mods in sync for both the Toxoid and classic runtimes.

The sound worker stages both packs outside the owned installation, validates
their completed manifests and audio containers, and holds the destination's
installation lock while publishing. A failure or cancellation preserves the
previous packs. Unrelated mods, saves, graphics caches and other launcher
settings remain in place. Imported bytes stay in the native game's existing
per-user `diasurgical/devilution/mods` directory.

For an unattended import without opening a window:

```sh
launcher --game /path/to/damnationx_host \
  --import-d2r-sounds "/path/to/Diablo II Resurrected" \
  --settings "/path/to/launcher/settings.json"
```

The command selects Remastered in the chosen settings file only after both packs
validate and publish. It preserves every other saved JSON value. Use a current
game build with D2R sound import enabled; a missing archive or unsupported game
executable reports an error instead of marking the sound pack ready.

### Settings and managed storage

The launcher saves its native configuration as per-user JSON and migrates an old
adjacent `launcher-settings.txt` when present. `--settings PATH` selects a separate
configuration for a portable setup or test profile. `--game PATH` overrides the
game executable; `launcher [path/to/game]` is also accepted. `--help` prints usage.
`--page library`, `--page options`, and `--page sounds` select the initial native
page, including for recording the actual launcher without UI automation.

Managed HD and D2R output locations follow the platform:

| Platform | HD pack | D2R cache parent |
| --- | --- | --- |
| Windows | `%LOCALAPPDATA%/damnationx/hd-combined/hd` | `%LOCALAPPDATA%/damnationx/d2r-models` |
| macOS | `~/Library/Application Support/damnationx/hd-combined/hd` | `~/Library/Application Support/damnationx/d2r-models` |
| Linux | `$XDG_DATA_HOME/damnationx/hd-combined/hd` | `$XDG_CACHE_HOME/damnationx/d2r-models` |

Linux defaults are `~/.local/share` and `~/.cache` when those XDG variables are
unset. Existing external packs/caches can be selected without moving them.
The importer recognizes `D2R_GAME_DIR`, `D2R_IMPORT_CACHE` and `D2RTOOL`; the
launcher passes the selected install, staged cache and optional helper to its
worker environment.

## Build the launcher

Install Rust with Cargo and the platform's native build tools. The launcher is its
own Cargo workspace and has its own lockfile. The pinned egui/eframe dependencies
currently require Rust 1.95 or newer; repository commands also respect the root
`rust-toolchain.toml` when run from this checkout.

- Windows: the MSVC Rust toolchain and Visual Studio C++ Build Tools/Windows SDK.
- macOS: Xcode Command Line Tools.
- Linux: a C toolchain, `pkg-config`, and X11/Wayland/OpenGL development libraries.
  Native file dialogs use XDG Desktop Portal, so the running desktop session also
  needs an appropriate `xdg-desktop-portal` backend.

For Debian/Ubuntu, the CI dependency set is:

```sh
sudo apt-get install -y build-essential pkg-config libwayland-dev libxkbcommon-dev \
  libx11-dev libxi-dev libxcursor-dev libxrandr-dev libxinerama-dev \
  libxcb-render0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libssl-dev
```

From the repository root:

```sh
cargo test --manifest-path tools/launcher/Cargo.toml --locked --all-targets
cargo build --manifest-path tools/launcher/Cargo.toml --locked --release
cargo run --manifest-path tools/launcher/Cargo.toml --locked --release
```

An explicit game and separate configuration can be supplied after Cargo's `--`:

```sh
cargo run --manifest-path tools/launcher/Cargo.toml --locked --release -- \
  --game /absolute/path/to/damnationx_host --settings /absolute/path/to/launcher.json
```

The native binary is `tools/launcher/target/release/launcher` (`launcher.exe` on
Windows), unless `CARGO_TARGET_DIR` or Cargo configuration selects another target
directory. Game builds are separate. The existing `tools/launcher/Makefile` also
builds the legacy macOS game and is not the cross-platform launcher build path.

## Browser frontend

The browser frontend retains display/settings controls and starts the browser
game through the embedding page's `launch.start(args)` integration. Its file
manager handles browser uploads. Native filesystem discovery, Python execution,
D2R conversion, native process launch and desktop ZIP installation are unavailable
in WebAssembly.

```sh
rustup target add wasm32-unknown-unknown
cargo check --manifest-path tools/launcher/Cargo.toml --locked --target wasm32-unknown-unknown
```

With Trunk installed, run `trunk build --release` from `tools/launcher`.
`Trunk.toml` writes the frontend to `build-webgpu/launcher` for the game page;
the launcher page alone does not provide the game-side `launch.start` function.

## Distribution archives

The standard-library Python packager makes a ZIP on Windows and a `.tar.gz` on
macOS/Linux. It preserves Unix executable permissions and writes `PACKAGE.json`
with per-file SHA256 hashes plus an archive checksum sidecar.

```sh
python3 tools/launcher/packaging/package.py \
  --binary tools/launcher/target/release/launcher \
  --platform macos-arm64 --output-dir dist/launcher --importer auto
```

Use the actual build's platform label (`windows-x64`, `macos-arm64`,
`macos-x64`, `linux-x64`, etc.) and executable suffix. `--importer auto` includes
source recipes when the checkout contains them; `--importer include` requires
them; `--importer omit` creates a launcher-only package. Importer files are copied
from an explicit source allowlist. Extracted assets, converted art and caches are
excluded.

To supply a prebuilt helper, build it outside the repository and pass it explicitly:

```sh
python3 tools/d2r-import/native/build.py --cache /absolute/external/d2r-helper --config Release
python3 tools/launcher/packaging/check_helper.py /absolute/external/d2r-helper/bin/d2rtool
python3 tools/launcher/packaging/package.py \
  --binary tools/launcher/target/release/launcher --platform macos-arm64 \
  --output-dir dist/launcher --importer include \
  --helper /absolute/external/d2r-helper/bin/d2rtool
```

The helper package requires its sibling `licenses` directory, carrying the exact
CascLib, bundled zlib and BitKnit2 notices written by `native/build.py`. The
archive includes the project license. These are unsigned archives, without a
game executable or owned art. The private `trainer` feature is excluded from the
default build and CI artifacts.

## Verification boundaries

`.github/workflows/launcher.yml` defines independent native test/build/archive jobs
on Windows x64, macOS ARM64 and Linux x64, plus a wasm32 frontend check. Native
tests use synthetic archives and a real process/grandchild fixture for worker
cancellation. A complete six-step D2R transaction test runs a source-free fake
importer through real subprocesses, covering successful publication and a failed
conversion that preserves the previous playable cache. It exercises worker
orchestration and rollback, without reading an owned D2R archive. Importer Python
tests use synthetic resources and mocked native build commands. The optional
workflow-dispatch helper build compiles pinned
CascLib and runs `d2rtool --version`/`--help` without owned data.
Set the manual workflow's `upload_artifacts` input to false for a validation-only
run: tests, builds, helper checks and packaging still run, without uploading the
archives to GitHub storage. Normal push/PR runs upload the resulting packages.

Local macOS checks have exercised library readiness, the native file picker,
game launch, live conversion progress and cancellation that retains the old cache.
These checks are separate from Windows/Linux CI and real owned-archive conversion.
The workflow definitions are not evidence that remote CI jobs have already run.

These jobs establish compilation and the covered automated setup behavior when
they pass.
They do not exercise a real desktop's file picker, an owned D2R archive, a GPU
driver or a game launch on every OS.

Packaging checks can be run without Cargo or game data:

```sh
python3 -m unittest discover -s tools/launcher/packaging -p 'test_*.py'
```
