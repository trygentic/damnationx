# DamnationX

DamnationX is a build of [DevilutionX](https://github.com/diasurgical/devilutionX) with optional HD art, a native 3D runtime, an online lobby and sound packs imported from your own copy of Diablo II or Diablo II: Resurrected.

Download the game from [Releases](https://github.com/trygentic/damnationx/releases), or play the browser build at [damnationx.net](https://damnationx.net). You must supply your own Diablo game data.

## Beta 8.2 hotfix

[Beta 8.2 is available](https://github.com/trygentic/damnationx/releases/tag/v1.6.0-beta.8.2). It fixes walking toward buildings, repeated path changes while holding the mouse, and movement drift after stopping. Cathedral walls and archways fade around the hero when they block the camera. Dungeon doors and arches are larger, and permanent dungeon portals are farther from stair arrivals.

The native Graphics menu includes a hover-outline toggle and thickness slider, with a thinner default outline. Show FPS is on by default and can be changed in that menu or with `--fps on` / `--fps off` when launching the packaged game. The 3D loading screen waits for models, meshes and textures to finish loading and reports progress. Town lighting uses the original trailer baseline again. Starting the legacy look with ray tracing disabled also avoids a render-target crash on ray-capable graphics devices.

This release also includes the beta 8.1 fixes for Windows startup, Python discovery, resumable D2R imports, hover names, ground item labels and chest poses.

The native game supports 3D, Classic and HD 2D rendering. Its Toxoid engine ships as compiled code with a license notice. This repository contains the game core and launcher; it does not contain the proprietary engine implementation.

The browser uses a separate C++ WASM/WebGPU renderer. It supports local HD packs and D2R sound imports, but it does not run the native engine or convert D2R characters and environments. Android remains on beta 7.

## Install the desktop game

The table below lists the beta 8.2 downloads. Each desktop download contains the game and launcher. Extract the complete archive into a writable folder and keep its files together.

| Platform | Package | Requirements |
| --- | --- | --- |
| Windows | `damnationx-game-and-launcher-1.6.0-beta.8.2-windows-x64.zip` | 64-bit Windows 10 or later; Direct3D 12 or Vulkan GPU and driver |
| Linux x64 | `damnationx-game-and-launcher-1.6.0-beta.8.2-linux-x64.tar.gz` | Ubuntu 24.04 or compatible; glibc 2.39 or later; X11 or Wayland; Vulkan driver |
| Linux ARM64 | `damnationx-game-and-launcher-1.6.0-beta.8.2-linux-arm64.tar.gz` | The same Linux requirements on 64-bit ARM hardware |
| macOS | `damnationx-game-and-launcher-1.6.0-beta.8.2-macos26.2-arm64.tar.gz` | Apple Silicon; macOS 26.2 or later |

Run `launcher.exe` on Windows. On Linux or macOS, open a terminal in the extracted folder and run `./launcher`. The launcher finds the neighboring game executable. Select your Diablo data folder, import any optional art or sound packs, then press Play.

The macOS package is ad-hoc signed and is not notarized by Apple.

Native ray tracing starts disabled. You can enable it on supported GPUs, select a hero model, or switch to Classic or HD 2D. Singleplayer pauses while the pause menu is open; multiplayer continues running.

## Supply your Diablo data

The downloads do not include Diablo's art, music or sounds. Choose the folder containing `DIABDAT.MPQ` from a copy you own:

- [GOG.com](https://www.gog.com/game/diablo) or Battle.net: find it in the installed game folder. The DevilutionX wiki also explains how to [extract the GOG installer](https://github.com/diasurgical/devilutionX/wiki/Extracting-MPQs-from-the-GoG-installer).
- Original CD: copy the file from the disc.
- Shareware: `SPAWN.MPQ` lets you play the shareware part of Diablo.

Keep your saves when updating. Extract a new release into a new folder and select the same data folder in its launcher.

## Optional art pack

HD art is optional, distributed separately and not attached to GitHub releases. A compatible combined pack contains HD art, 3D props and character models. If you already have it, import the single archive; the game selects the installed models automatically. You can play with classic art without an HD pack.

In the desktop launcher, open Asset library, choose Import ZIP under HD art pack, and select the archive. Existing packs can be selected with Use folder. If installing by hand, preserve the archive's `hd/` folder structure.

The pack uses the existing WebP export, with PNG files where that export retains them. The archive includes the notices for its 3D content. It contains no D2R assets.

## Import Diablo II: Resurrected art

Use Import installed D2R in the desktop launcher and select your owned D2R installation. Converted models, environments, interface art and portal fields remain on your computer.

The importer requires Python 3.11 or later. It checks the saved interpreter and searches for another installed version if that selection is missing or too old. Windows Store execution aliases are skipped. You can choose an interpreter under Launch options > Python. Managed environments are rebuilt when broken; upgrading Python does not require deleting launcher settings in AppData.

The native import helper can be supplied in the package. If it is absent, conversion requires CMake 3.20 or later and a C/C++17 compiler. The launcher creates a Python environment and installs NumPy and Pillow by default, which requires internet access. A failed or cancelled import preserves the previous playable assets and keeps completed conversion work for a retry.

D2R art is optional and independent of the HD pack. The browser cannot run this conversion.

## Play in a browser

At [damnationx.net](https://damnationx.net), choose your game files and press Play. Use Chrome or Edge with WebGPU enabled.

To host the browser download yourself:

1. Extract `DamnationX-1.6.0-beta.8.2-web.zip`.
2. Serve its folder over HTTP on localhost, or HTTPS on another host. Opening `index.html` directly from disk does not work.
3. Open the page in Chrome or Edge and select your own game data.

For a local server:

```sh
cd DamnationX-1.6.0-beta.8.2-web
python3 -m http.server 8000
```

Open `http://localhost:8000`. A web server must serve `.wasm` files as `application/wasm`. A copy you host yourself supports singleplayer; use the hosted site or desktop game for the configured online lobby.

## Diablo II sound packs

The desktop launcher imports both Classic and Remastered sound packs from an owned D2R installation. Select the imported pack under Sounds. The browser can import sounds from a locally selected game folder; use Chrome or Edge for D2R imports. Conversion runs locally and does not upload game files.

## Build the public game core

See [docs/building.md](docs/building.md). Native C++ WebGPU builds use `-DUSE_SDL3=ON -DDEVILUTIONX_WEBGPU=ON`; their first configure downloads and builds Dawn. The browser build has its own instructions. Building this repository produces the public C++ client and launcher, not the proprietary native runtime.

## Report a problem

Open an issue with your operating system, package filename, selected art and sound options, steps to reproduce, and the error text or launcher activity log. For targeting problems, include the rendering mode and whether D2R or HD art is enabled.

## License

DamnationX is based on DevilutionX and is distributed free for non-commercial use under the [Sustainable Use License](LICENSE.md). Individual art files and bundled libraries retain their own notices. Diablo and Diablo II: Resurrected are Blizzard Entertainment trademarks. This project is not affiliated with or endorsed by Blizzard Entertainment.
