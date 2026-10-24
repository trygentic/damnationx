# DamnationX
<p align="center">
  <img width="512" height="768"  alt="image" src="https://github.com/user-attachments/assets/64de5c9a-e64f-4c9c-8c89-ede3522c0b18" />
</p>

DamnationX is a build of [DevilutionX](https://github.com/diasurgical/devilutionX), the open source port of Diablo. It adds optional HD art drawn with WebGPU, a lobby for playing online, and sound packs you can import from your own copy of Diablo II or Diablo II: Resurrected.

This repository holds the source code and the release builds. Download the builds from the [Releases](https://github.com/trygentic/damnationx/releases) page.

## Latest beta

Beta 8 adds a desktop launcher and the native 3D Resurrect Update, built on the custom Toxoid game engine. The original game simulation still handles combat, quests and saves. The native runtime supports 3D, Classic and HD 2D modes. Toxoid is proprietary and ships only compiled into the native game; its source is not included here.

The launcher selects your game executable and Diablo data, installs local HD packs, and converts art and sounds from your own Diablo II: Resurrected installation. Imported game assets stay on your computer. See the [release notes](https://github.com/trygentic/damnationx/releases/tag/v1.6.0-beta.8) for the files available in this beta and their requirements.

The browser build at [damnationx.net](https://damnationx.net) uses the separate C++ WASM/WebGPU renderer. Native 3D engine features are not available in that build. Android remains on beta 7; its download is on that release page.

## Install the game and launcher

Each desktop download combines the native game and launcher. Extract the complete archive into a writable folder and keep its files together.

| Platform | Beta 8 package | Requirements |
| --- | --- | --- |
| Windows (build in progress) | `damnationx-game-and-launcher-1.6.0-beta.8-windows-x64.zip` is not attached yet | 64-bit Windows 10 or later, with a GPU and driver that support Direct3D 12 or Vulkan |
| Linux x64 | `damnationx-game-and-launcher-1.6.0-beta.8-linux-x64.tar.gz` | Ubuntu 24.04 or a compatible system with glibc 2.39 or later, X11 or Wayland, and a Vulkan driver |
| Linux ARM64 | `damnationx-game-and-launcher-1.6.0-beta.8-linux-arm64.tar.gz` | The same Linux requirements, on 64-bit ARM hardware |
| macOS | `damnationx-game-and-launcher-1.6.0-beta.8-macos26.2-arm64.tar.gz` | Apple Silicon and macOS 26.2 or later |

Run `launcher.exe` on Windows, or open a terminal in the extracted folder and run `./launcher` on Linux or macOS. The launcher finds the neighboring game executable. Select the folder containing your own `DIABDAT.MPQ`, install an optional local HD pack under Asset library, then press Play. D2R conversion requires an owned installation; the launcher shows any Python or helper requirements before importing.

The macOS binaries are ad-hoc signed and are not notarized by Apple. If the system blocks them, use macOS Privacy & Security to approve the application you downloaded.

The native runtime starts with ray tracing disabled. You can enable it on supported GPUs, choose a hero model per character, or switch to Classic or HD 2D. The new pause menu supports mouse, keyboard and controller. Singleplayer pauses while it is open; multiplayer stays live.

The platform instructions below describe the legacy C++ builds. The launcher can start a legacy executable as well as the native 3D runtime. Building the public source does not build the proprietary native engine.

## You need the Diablo game data

DamnationX doesn't include Diablo's art, music or sounds. You need `DIABDAT.MPQ` from a copy you own:

- Diablo from [GOG.com](https://www.gog.com/game/diablo) or Battle.net: the file is in the game folder. With the GOG installer you can also [extract it](https://github.com/diasurgical/devilutionX/wiki/Extracting-MPQs-from-the-GoG-installer).
- The original CD: the file is on the disc.
- No copy? `spawn.mpq` from the [shareware version](https://github.com/diasurgical/devilutionx-assets/releases/latest/download/spawn.mpq) lets you play the shareware part of the game.

## Legacy C++ build on macOS

This build needs an Apple Silicon Mac (M1 or newer) running macOS 26 or later.

1. Download `DamnationX-<version>-macos-arm64.zip` from the latest release and unzip it.
2. Move `DamnationX.app` to your Applications folder.
3. Copy `DIABDAT.MPQ` into `~/Library/Application Support/diasurgical/devilution/`. In Finder, press Cmd+Shift+G and paste that path. Create the folder if it doesn't exist.
4. The app isn't signed by Apple, so macOS blocks it the first time. Right-click `DamnationX.app`, choose Open, then confirm. If macOS still refuses, run this once in Terminal:

   ```
   xattr -dr com.apple.quarantine /Applications/DamnationX.app
   ```

5. Open the app. Single Player works offline. Multi Player connects to the lobby at `lobby.damnationx.net`.

## Play in a browser

The simplest option is the hosted version at [damnationx.net](https://damnationx.net). Click Game files…, choose your `DIABDAT.MPQ`, then click Play.

To host the browser build yourself:

1. Download `DamnationX-<version>-web.zip` and unzip it.
2. Serve the folder over HTTP. Opening `index.html` straight from disk doesn't work. With Python, for example:

   ```
   cd DamnationX-<version>-web
   python3 -m http.server 8000
   ```

3. Open `http://localhost:8000` in Chrome or Edge. WebGPU needs a secure context, and `localhost` counts as one. On another host, use HTTPS.
4. Click Game files…, choose your `DIABDAT.MPQ`, then click Play.

If you use another web server, make sure it sends `.wasm` files as `application/wasm`.

A copy you host yourself is for single player. For online games, play at [damnationx.net](https://damnationx.net) or use the macOS app.

## Legacy C++ build on Windows

The Windows build needs 64-bit Windows 10 or later and a graphics card with Direct3D 12.

1. Download `DamnationX-<version>-windows-x64.zip` and unzip it anywhere.
2. Copy `DIABDAT.MPQ` into the unzipped folder, next to `devilutionx.exe`. You can also put it in `%APPDATA%\diasurgical\devilution\`.
3. Run `devilutionx.exe`. The program isn't signed, so Windows SmartScreen may warn you the first time. Click More info, then Run anyway.

Keep `d3dcompiler_47.dll` next to the exe. Without it the game starts with the classic graphics and no HD.

## Legacy C++ build on Linux

There are builds for x86_64 and for 64-bit ARM (aarch64). They need glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or later), an X11 or Wayland desktop, and a working Vulkan driver. On Debian and Ubuntu that means `libvulkan1` and `mesa-vulkan-drivers`, or the NVIDIA driver. SDL is built in, so you don't need to install it.

1. Download `DamnationX-<version>-linux-x86_64.tar.xz` or `DamnationX-<version>-linux-arm64.tar.xz` and extract it.
2. Copy `DIABDAT.MPQ` into `~/.local/share/diasurgical/devilution/`, or into the extracted folder next to `devilutionx`.
3. Run `./devilutionx`.

If you'd rather use your package manager, the release also has `.deb` and `.rpm` packages. If Vulkan doesn't work on your machine, `./devilutionx --no-webgpu` starts the game with the classic graphics.

## Legacy C++ build on Android

The Android build needs Android 7.0 or later on a 64-bit ARM or x86_64 device with Vulkan.

1. Download `DamnationX-<version>-android.apk` on the device and open it. Allow installs from your browser or file manager if Android asks.
2. Open DamnationX (Dev). It asks for the game data the first time: pick your `DIABDAT.MPQ`, or download the shareware data.

This APK is a debug build signed with a development key. It installs as a separate app from earlier betas and from DevilutionX, so you'll need to add your game data again. A later release signed with a different key may need you to uninstall it first. Your files are kept under `Android/data/net.damnationx.dev/`.

## HD art

The HD art pack isn't distributed here. If you have it, here's where each build looks for it. The folder must be named `hd` and contain `sprites.txt`.

- Windows: next to `devilutionx.exe`, or `%APPDATA%\diasurgical\devilution\hd`.
- Linux: next to `devilutionx`, or `~/.local/share/diasurgical/devilution/hd`.
- Android: long-press the DamnationX icon and choose Import Data, or tap Import Data on the game data screen, then pick the HD pack zip. The WebP version of the pack is the better fit for phones: it's about 830 MB, against 3 GB for the PNG one. The app unpacks it and shows HD the next time it starts.

If the HD pack has a `3d/warrior` folder inside `hd`, Warriors are drawn as an animated 3D model instead of HD sprites. Settings > Graphics > 3D Warrior Model switches that off.

If the `hd` folder is in place but WebGPU can't start, the game falls back to the classic graphics and says so in the message log. Updating the graphics driver usually fixes that. The HD fonts aren't used yet, so text stays in the classic font.

## Diablo II sounds

DamnationX can replace Diablo's sounds with the ones from Diablo II or Diablo II: Resurrected, if you own either game.

- On macOS, open Settings > Audio and choose a Sound Pack. The game looks for the install, including inside CrossOver and Whisky bottles, and imports the sounds. If it can't find it, set its folder as `Diablo II Path` or `Diablo II Resurrected Path` under `[Audio]` in `diablo.ini`.
- In the browser, choose a Sound Pack in Settings > Audio, then pick the game folder in the dialog that opens. Use Chrome or Edge for Diablo II: Resurrected. Only the sounds are saved, in your browser.

Nothing from Diablo II is uploaded anywhere.

## Building from source

DamnationX builds the same way as DevilutionX. See [docs/building.md](docs/building.md) for each platform. The HD art needs the WebGPU build. On Windows, macOS and Linux, configure CMake with `-DUSE_SDL3=ON -DDEVILUTIONX_WEBGPU=ON`. The first configure downloads and builds Dawn, which takes a while. For Android, run `./gradlew assembleDebug -Pwebgpu` in `android-project`. The browser build has its own section on that page.

The lobby and relay servers that online games connect to are not part of this repository.

## Reporting problems

Open an issue on this repository. Say which build you used (macOS, Windows, Linux, Android or browser), what you did, and what happened.

## License

DamnationX is based on DevilutionX and is distributed under the [Sustainable Use License](LICENSE.md). Diablo is a trademark of Blizzard Entertainment. This project is not affiliated with or endorsed by Blizzard.
