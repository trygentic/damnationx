# Build and test the portable core

Use C++17 and a standard library; no SDL, Dawn, game assets or profile are needed.

```
cmake -S modules/hud -B build-hud -DDIABLO_HUD_BUILD_TESTS=ON
cmake --build build-hud --parallel 1
ctest --test-dir build-hud --output-on-failure
```

The exact headers and tests passed direct MSVC compilation with /std:c++17 /W4 /WX /MP1, then runtime tests and the headless example on 2026-10-14. CMake integration is provided for host review; this record is not a CMake or cross-platform qualification.

Tests cover stable identities/codecs, all eight resize directions, normalized auxiliary edges, commit/rollback and viewport changes, mixed buttons, topmost ownership, captured pointer identity/visibility and phase-preserving materialization. The belt oracle uses the native final repartition after overrides, retaining independent intermediate and final goldens. The earlier intermediate-slot failure is retained in private evidence.

Input ownership and geometry are data contracts. The host supplies availability, native close/action policy, output transforms and profile I/O. Rendering pixels, resource loading and game composition are outside this core-only delivery.
