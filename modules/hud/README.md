# Portable HUD core

Experimental C++17 extraction of the Diablo3D / DevilutionX HUD. It supplies authored/compact geometry, normalized layout codecs, reversible gestures, floating-window geometry/order/hit/resampling helpers, pointer/input ownership and an advisory frame sequencing contract. State and resource lifetimes belong to the host.

Include `diablo_hud/layout.hpp`, `windows.hpp`, `persistence.hpp`, `input.hpp` and `frame.hpp`; link the header-only `diablo_hud::core` CMake target. See [API](docs/API.md), [build/tests](docs/BUILD-CORE.md), [license](LICENSE.md), [notices](NOTICE.md) and [provenance](PROVENANCE.json).

This sealed delivery contains seven standard-library-only headers, tests, a headless example and opt-in CMake integration. Native adapters, theme component, private game fixture, art/fonts, HD packs, MPQs, profiles, saves, GPU and world/camera/actor code are excluded. It does not claim native composition, SDL3 or Dawn/WebGPU qualification. A host must preserve its overlay coverage/unlit stamps, UI layer transform and relative-input owner; a WebGPU host may have no SDL renderer.
