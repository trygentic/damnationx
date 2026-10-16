# HUD UI context bridge — code stage

Status: **CompileRuntimePending**. This is an opt-in C++17 value/callback contract.
It is not an active SDL3 adapter, Dawn backend, input interceptor or presentation
hook. No compiler, test executable, game, input or GPU was run for this stage.

The dependency is the sealed diablo_hud/types.hpp core supplied separately:
commit 237a4a19f51c68ba6e3136de1a357b21c33c79a7, public ZIP SHA256
77eaee5bf1724892184cc2188a96816ee726c3f89945bb5c566b38f00230e046.
Inherited Sustainable Use License v1.0 and contributor notices remain applicable.
Modification attribution: Douglas Pan, with AI-assisted implementation/review.
No assets, profiles or renderer sources are supplied here.

## Host boundary

Target: trygentic/damnationx@71aeea6ca32397906b806bce0ddc47962504d162.
Use actual target APIs; do not infer UI scaling from the ratio of dimensions:

- [UI API](https://github.com/trygentic/damnationx/blob/71aeea6ca32397906b806bce0ddc47962504d162/Source/engine/render/ui_scale.hpp#L32):
  GetUiWidth/Height, GetUiScaleQuarters, GetUiTransform, ScreenToUi and the
  Rectangle overload of UiToScreen.
- [Coordinate rules](https://github.com/trygentic/damnationx/blob/71aeea6ca32397906b806bce0ddc47962504d162/Source/engine/render/ui_scale.cpp#L145):
  point inverse, pixel centre and rectangle edges are distinct operations.
- [Exact world pointer](https://github.com/trygentic/damnationx/blob/71aeea6ca32397906b806bce0ddc47962504d162/Source/engine/render/ui_scale.cpp#L176):
  GetMouseScreenPosition preserves the original screen event where available.
- [Nullable renderer](https://github.com/trygentic/damnationx/blob/71aeea6ca32397906b806bce0ddc47962504d162/Source/engine/dx.cpp#L1094):
  WebGPU normally presents without an SDL renderer object.

The host supplies one coherent HostSnapshot: UI and logical-screen sizes,
window units and drawable pixels separately, actual UI quarter scale/offsets,
actual drawable viewport, availability, opaque owner identity and generations.
The owner token is independent of any nullable SDL_Renderer pointer. Bump the
presentation generation on backend/device/surface recreation; bump UI transform
generation on scale/transform changes, even when rounded dimensions stay equal.
Window and drawable dimension changes also invalidate reuse.

BuildUiContextBridge produces the core's numeric Context. Its float scale/origin
fields are **nominal physical metrics**, not an exact UI raster transform:
the target has rounded edges and a special zero edge. Exact pointer mapping and
rectangle projection go through host callbacks, once, into the appropriate space.
The host must validate that its snapshot describes the actual target transform.
The dimension bound of 65535 limits this contract's arithmetic, not GPU capacity.
Admission also rejects nonfinite or zero derived physical scales.

Events already converted by the target to logical screen pixels must not undergo
another output/window conversion. RouteScreenPointer always returns that exact
screen point for world picking. Its separate HUD point requires a caller-supplied
insideViewportBeforeTruncation: letterbox validity cannot be recovered from an
integer logical point. For example, window 1281x720/source 640x480 has origin X=160.5;
window X=160 is outside although truncating its negative logical fraction yields 0.
An already UI-space MousePosition stays UI-space; do not route it here again.

Callbacks map native Point/Rectangle values to core Point/Rect over the same stable
host snapshot. They must not dispatch gameplay, write pixels, alter transforms or
mutate generations during a call. CanReuseCoordinateMap checks lifecycle epochs
in addition to core sameTransform, excluding hasHudOverride from coordinate
comparison only. Appearance changes retain mapping; the original Context keeps
its metadata. This predicate does not approve a core ElementState transaction:
the core still checks its own context, including hasHudOverride, and the caller
must honor that result. On refusal, the caller cancels its visual
gesture and invalidates capture recipes, while retaining consumption of an owned
button pair until its matching release. This bridge does not reset layout,
visibility, world state or mouse globals.

ProjectUiRect returns clipped UI/screen bounds and drawable-pixel bounds.
Consumers can share them for visible drawing and coverage metadata. It does not
stamp or copy coverage. Native CompositeUiLayer/StampUnlit, FPV black-versus-world
handling, HD UI tracking, modal ordering and cursor/late overlay recovery remain
the host owner's work. Full offscreen bounds and native panel content transforms
also remain owned by that adapter.

## Prepared tests and next gate

tests/ui_context_bridge_tests.cpp uses the standard library and numeric mock
callbacks, no assets, SDL, Dawn or game globals. It covers fractional edge goldens,
exact world pointer preservation, pre-truncation validity, callback count/refusal,
clipping, invalid snapshots, renderer-independent identity, lifecycle epochs and
UI/window/drawable invalidation. Appearance toggles retain coordinate mapping
without bypassing core transaction checks; derived-scale underflow is refused.

For a later authorized CPU lane, supply the sealed core's include directory:

    c++ -std=c++17 -Wall -Wextra -Werror -I<sealed-core/include> tests/ui_context_bridge_tests.cpp -o <new-owned-output>/ui_context_bridge_tests

This command was not executed. The next native gate must replace mocks with
actual target mapping functions and compare rounding/output bounds over odd sizes
and DPI, then qualify drag/release, modal/FPV ownership and real coverage.
SDL3, actual host composition, WebGPU, physical input/DPI and all runtime paths
remain pending. No Source/CMake activation or API publication is part of this stage.
