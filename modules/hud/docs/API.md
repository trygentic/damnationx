# Core API and contracts

The standalone core is header-only C++17 in namespace `diablo_hud`. It consumes explicit values; it does not own an SDL renderer, a GPU device, native game state or profile files. This document follows the actual headers of the reviewed candidate. The extraction is experimental and does not establish a stable binary/plugin ABI.

## Header inventory

The inspected candidate contains `types.hpp`, `authored.hpp`, `layout.hpp`, `persistence.hpp`, `windows.hpp`, `input.hpp` and `frame.hpp` under `include/diablo_hud/`. Their presence establishes code availability, not a build/runtime PASS. Native wrapper wiring and final validation are recorded separately. The optional C++23 theme manifest component is independent of these C++17 headers.

## Value types

`include/diablo_hud/types.hpp` defines the following contracts:

| Type/value | Meaning |
|---|---|
| `Point`, `Size`, `Delta`, `Rect` | Integer geometry. `Rect::contains` uses positive-size, half-open bounds; endpoint checks use a wider integer. |
| `Context` | Numeric logical/output sizes, viewport/origin/scale, generations and validity. `rendererIdentity` is an opaque number supplied by the host, never a dereferenced or owned SDL object. |
| `sameTransform` | Compares the transform while disregarding layout/customization generation changes caused by the gesture itself. Host transform/identity changes must still invalidate a gesture. |
| `ElementCount`, `ClockElement`, `MinimapElement`, `ElementNames` | Eighteen stable identities, with Clock16 and Minimap17 appended. Map9 remains the button. |
| `NormalizedRect`, `Overrides`, `NormalizedUnits` | Optional per-element profile overrides in 0..10000 units. Serialization and filesystem writes stay with the host. |
| `ResizeEdge` | Four side bits; combine adjacent bits for corners. `ResizeNone` describes movement rather than resizing. |
| `WindowPreference`, `WindowPreferences` | Legacy seven-slot position/scale preferences, separate from the eighteen-element IDs. |
| `Eligibility` | Explicit enabled/focused/modal/held-item/visible inputs. `allows()` requires an available, focused, non-modal owner with no held item. |
| `GeometryMode`, `Metrics`, `Geometry` | Authored/compact geometry, thirteen authored nodes, eight belt slots, frame and glyph/design metrics. Native glyph buffer dimensions must not be scaled twice. |

## Geometry and element state — `layout.hpp`

`authored.hpp` contains the original thirteen-entry logical layout table and its `defaultRect`, `placeRect`, joint-edge `scaleRect` and `authoredDefaults` helpers. It contains no scene, image or font data. `resolveGeometry(context, authored)` accepts thirteen host-supplied authored rectangles; the context-only overload uses `authoredDefaults(context)`. The explicit-array overload lets a host retain its own authored override without a second scale. `beltSlotRect`, `elementMinimum`, `clampElement`, `storeOverride`, `applyOverride`, `resizeEdges` and `resizePreview` share deterministic geometry. Clock/Minimap normalization uses edge differences; old IDs retain their previous normalized math.

`minimapDefaultBounds`, `minimapViewport`, `minimapCoverageWidth` and `clockDefaultBounds` provide the auxiliary geometry helpers. Map coverage uses the larger of useful width and twice useful height; these helpers do not draw or modify exploration.

`ElementState` owns overrides, an optional gesture and a generation:

| Method | Contract |
|---|---|
| `replace(values)` | Cancel any gesture, replace the complete override snapshot and increment generation. No file I/O. |
| `begin(id, rect, pointer, edges, context, minimum)` | Capture previous state and clamped original bounds. If already dragging, cancel and return false rather than replacing a preview. Caller checks initial eligibility. |
| `update(pointer, context, eligibility, minimum)` | Preview only when eligibility and transform remain valid; otherwise cancel. |
| `end(pointer, context, eligibility, minimum)` | Revalidate/update the final pointer even without a MOVE, then finish and return true. Host persists after success. |
| `cancel()` | Restore previous override and release the gesture; no write. |
| `reset(id)`, `resetAll()` | Cancel first and deliberately restore defaults. Host chooses when to persist. |
| `overrides`, `generation`, `dragging`, `customized`, `dragElement` | Read state for host caching/eligibility/persistence; they do not execute native actions. |

The host supplies positive dimensions and positive finite output scales for a valid numeric context. `Context` is data, not an SDL query or a complete input validator. Supply a generation/opaque identity that invalidates a gesture when its actual output transform changes.

## Profile codecs — `persistence.hpp`

`ParseElementLayout` returns `optional<Overrides>` with whole-file acceptance, `D3DHUDLAYOUT1`, stable names and a bound of 8192 bytes. A failure leaves the caller's active snapshot untouched. `SerializeElementLayout` takes a host-validated snapshot. Existing records remain readable by the new codec; an older executable may reject newly added Clock/Minimap names, so this is not a backward-loading guarantee.

`ParseWindowLayout` preserves the legacy headerless rows: invalid rows are ignored, valid duplicates replace position, and invalid/absent scale keeps that slot's previous scale. Oversized input is declined. `SerializeWindowLayout` retains native row order/stream precision; scale-only edits must supply the displayed body position. Codecs use strings and no filesystem. Profile paths, load/backup/replace and write errors remain host responsibilities.

## Window and pointer helpers

`windows.hpp` defines `UniformWindowScale`, `ClampWindowBody`, `WindowBounds`, `VisualToNative`, `NativeToVisual`, `ContinuousWindowScale` and `SurfaceSourcePoint`. Renderer resampling and pointer mapping share the integer inverse; forward mapping selects a pixel whose inverse resolves to that native pixel. The baseline scale minimum remains 1 even when the viewport cannot contain the native content; the host must handle clipping/availability rather than treating that as a fit guarantee.

`WindowRecord` holds native, visual and body bounds supplied by the host. `FindWindowHit` chooses the topmost visible record; a foreground header blocks a body-only hit behind it. `FocusWindows` rotates one visible owner to the end. `WindowStack` provides `isOpen`, `close`, `reopen`, `focus` and `hit` over transient records; it does not own native game flags or persistent preferences. Supply unique IDs and fresh bounds.

`input.hpp` defines `ResolvePhysicalOwner`: modal first, then Clock, topmost window, Minimap and lower elements; relative-captured world input declines ordinary UI ownership. “Physical” here means the pointer before panel-native inverse mapping, in the **same declared logical space as the bounds**. Convert OS/output/screen to that host space at its boundary.

`PointerScope` captures/maps only the outer scope and restores its pointer on exit; `IsNativePointer` and `CapturedWindowAt(state, bodyOnly, ownerVisible)` retain the captured identity in nested handlers. Pass current native visibility as `ownerVisible`; the captured record alone cannot observe a handler closing its owner. Resolve modal ownership first and bypass the window-pointer scope for native modal dispatch. Native visibility remains a host authority.

`InputPairs` tracks owned button masks through `own`, `owns`, `release` and `cancel`. This latch does not dispatch gameplay or cancel an ElementState automatically; the host coordinates both lifecycles and consumes each pair correctly. Cancelling an edit does not by itself authorize clearing a latch before its matching UP; a full input reset belongs to the host.

## Frame sequencing — `frame.hpp`

`FrameContract` is an advisory sequence contract: `begin`, `beginPanel`, `sealPanel`, `sealPanels`, `cursorDrawn`, `lateOverlay`, `prepare`, `present`, `materialize`, `recover` and `invalidate`, plus `phase`, `preservesCursor`, `materialized` and `identity` queries. Frame identity must match at prepare/present. Panel sealing precedes cursor, and cursor precedes late overlays. Size admission is bounded to 16Mi pixels.

This object owns no pixel snapshot, texture, decoder, native callback or cursor buffer, and does not automatically enforce the native adapter's calls. Its cursor flag records sequencing; it does not prove that cursor pixels were preserved. Actual capture/recovery/upload/presentation and disposal remain adapter work. `materialize()` marks a transport fallback without ending the composition phase, so later panels and overlays can still be composed. `recover()` ends the frame in the recovered phase; it is not a substitute for continued materialized-panel painting.

## Excluded components

This core-only package does not include the optional C++23 theme, native compatibility wrappers or any renderer adapter. The following lifecycle table describes responsibilities of a future host integration; those engine entry points are reference boundaries from the separate native draft, not APIs shipped by this module.

## Native compatibility lifecycle

The originating native draft retains engine-facing APIs under namespace `devilution` and a `hud_core_bridge.hpp` conversion layer. That draft is excluded and has no native runtime qualification for this extraction. A host must implement equivalent integration responsibilities explicitly; these reference entries are not standalone-core entry points:

| Lifecycle | Retained entry points / host responsibility |
|---|---|
| Initialize/profile replace | Construct core state, then `ElementState::replace` with an accepted codec result. Native profile loading remains adapter-owned; no new core singleton or filesystem initializer exists. |
| Begin/update/end edit | `BeginD3dHudCustomization` / `BeginD3dHudEdgeCustomization`, `UpdateD3dHudCustomization`, `EndD3dHudCustomization`; floating windows use their `BeginD3dWindowDrag`, `UpdateD3dFloatingPanelDrag`, `EndD3dFloatingPanelDrag` equivalents. |
| Cancel/reset | `CancelD3dHudCustomization`, `CancelD3dFloatingPanelDrag`; deliberate profile resets use `ResetD3dHudCustomization`, `ResetAllD3dHudCustomizations` or `ResetD3dFloatingPanelLayout`. Keep a consumed input pair owned until its release. |
| Frame visibility/capture | `BeginD3dHudAuxiliaryFrame`, `BeginD3dHudPresentationFrame`, `BeginD3dPanelPresentationFrame`; the native frame path supplies real visibility, surfaces and cursor hooks. |
| Seal/finalize/present | HUD foreground seal is separate from panel seal; panel finalize follows actual cursor drawing. Prepare/Render APIs own admission and frozen-pixel recovery, as host-owned integration work outside this package. |
| Invalidate/dispose | `InvalidateD3dHudPresentationFrame` / `InvalidateD3dPanelPresentationFrame` discard captures; `ResetD3dHudPresentationResources` / `ResetD3dPanelPresentationResources` dispose caches before renderer teardown. They do not reset profile layout. |

A host chooses and names its coordinate spaces. For the native baseline, lower/floating HUD rectangles use the host's logical surface space. For a host with UI Scale, `MousePosition` may already be in UI space; physical/screen pointer, UI pointer and scoped panel-native pointer must remain distinct. A valid WebGPU host can have `SDL_Renderer == nullptr`; context cannot depend on the presence of that object.

API availability, native adapter wiring, test commands and result claims must match the exact candidate headers and [BUILD-CORE.md](BUILD-CORE.md). Standalone tests and frame flags do not certify full host composition or renderer behavior.
