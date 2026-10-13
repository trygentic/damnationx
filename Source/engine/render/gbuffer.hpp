// Source/engine/render/gbuffer.hpp
#pragma once
#include <array>
#include <cstdint>
#include <cstring> // std::memset in StampUnlitRun
#include <span>
#include "engine/lighting_defs.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"
#include "levels/gendung_defs.hpp" // dungeon_type
#include "utils/attributes.h"

struct SDL_Color;

namespace devilution::gbuffer {

enum class Kind : uint8_t {
	Unlit = 0,        // UI, cursor, text, outlines, OOB void, fully-dark tiles: GPU passes the frame through
	Floor = 1,        // z = 0, N = up. Also colour-0 sprite shadows (feet-row floor) and Left/RightTriangle micro frames.
	WallLeft = 2,     // SW face, N = tile +y: a wall running along x on the cell's centre line (drawn in the right half)
	WallRight = 3,    // SE face, N = tile +x: a wall running along y on the cell's centre line (drawn in the left half)
	Card = 4,         // upright billboard at the entity's feet, N from LUT index
	CardEmissive = 5, // self-lit card (TRN'd sprites, !_miLightFlag missiles, !applyLighting objects, player icons): pass-through
	WallBlended = 6,  // 50 %-blended wall pixel: lit like the wall face it continues (writes its own B)
	WallTop = 7,      // wall cap / pillar top / back-wall half without a visible face: N = up, z = h
};
constexpr uint8_t KindShift = 5;
constexpr uint8_t NormalMask = 0x1F;

[[nodiscard]] constexpr uint8_t PackAux(Kind kind, uint8_t normalIdx) { return static_cast<uint8_t>((static_cast<uint8_t>(kind) << KindShift) | (normalIdx & NormalMask)); }
[[nodiscard]] constexpr Kind AuxKind(uint8_t a) { return static_cast<Kind>(a >> KindShift); }
[[nodiscard]] constexpr uint8_t AuxNormal(uint8_t a) { return a & NormalMask; }

// Fixed LUT slots; 4..31 tile the tilted card hemisphere.
constexpr uint8_t NormalUp = 0, NormalWallLeft = 1, NormalWallRight = 2, NormalCardFlat = 3;
constexpr uint8_t UnlitByte = PackAux(Kind::Unlit, 0);
constexpr uint8_t ShadowByte = PackAux(Kind::Floor, NormalUp);
constexpr uint8_t FlatCardByte = PackAux(Kind::Card, NormalCardFlat);
constexpr uint8_t EmissiveCardByte = PackAux(Kind::CardEmissive, 0);

enum class Mode : uint8_t { Off = 0, CaptureWorld = 1, StampUnlit = 2 };
enum class FootKind : uint8_t { None = 0, SW = 1, SE = 2 };

struct Planes {
	uint8_t *aux = nullptr;           // plane A, pitch*height bytes
	uint8_t *base = nullptr;          // plane B, pitch*height bytes
	const uint8_t *outBase = nullptr; // PalSurface->pixels
	uint16_t pitch = 0, width = 0, height = 0;
	uint64_t pitchRecip40 = 0; // ((1 << 40) + pitch - 1) / pitch: exact row = (off * pitchRecip40) >> 40 for off < pitch*height (replaces per-run div/mod)
	[[nodiscard]] DVL_ALWAYS_INLINE uint8_t *auxAt(const uint8_t *dst) const { return aux + (dst - outBase); }
	[[nodiscard]] DVL_ALWAYS_INLINE uint8_t *baseAt(const uint8_t *dst) const { return base + (dst - outBase); }
	[[nodiscard]] DVL_ALWAYS_INLINE uint32_t rowOf(ptrdiff_t off) const { return static_cast<uint32_t>((static_cast<uint64_t>(off) * pitchRecip40) >> 40); }
	/** True when `out` is a (sub)region of the back buffer the planes mirror (subregions have begin() != outBase). */
	[[nodiscard]] bool covers(const Surface &out) const { return out.begin() >= outBase && out.begin() < outBase + static_cast<size_t>(pitch) * height; }
};

// ---- Global state (single-threaded renderer). Read by the hot blitters; latched once per frame. ----
extern Mode CurrentMode;                           // Off / CaptureWorld / StampUnlit
extern Planes CurrentPlanes;
extern uint8_t CurrentTileAux;                     // PackAux(kind, normal) for the tile command / arch sprite being rasterised
extern uint8_t CurrentTileAuxBlended;              // PackAux(WallBlended, normal) (or CurrentTileAux for non-walls)
extern std::array<uint8_t, 32> CurrentTileBaseRow; // B byte per tile-local column c: (anchorRow - foot(c)) & 0xFF
extern int16_t CurrentTileLeftX;                   // screen x of tile-local column 0 (may be negative when clipped)
extern Kind CurrentCardKind;                       // set by DrawWorldSprite before ClxDraw*: Card, CardEmissive, WallLeft or WallRight (arches)
extern bool CurrentArchBlended;                    // set with CurrentCardKind for arches: a see-through arch keeps the cards it is blended over

[[nodiscard]] DVL_ALWAYS_INLINE bool CaptureActive() { return CurrentMode == Mode::CaptureWorld; }
[[nodiscard]] DVL_ALWAYS_INLINE bool StampActive() { return CurrentMode == Mode::StampUnlit; }
[[nodiscard]] DVL_ALWAYS_INLINE bool Active() { return CurrentMode != Mode::Off; }

// ---- Lifecycle ----
void Allocate(const uint8_t *outBase, uint16_t pitch, uint16_t width, uint16_t height); // CreateBackBuffer (dx.cpp:346-373); frees a previous allocation first
void Free();                                                                              // dx_cleanup

/** Frame-latched enable. Called exactly once at the top of DrawAndBlit / scrollrt_draw_game_screen (never mid-frame). Resets CurrentMode = Off. */
struct LatchInput {
	bool optionEnabled;
	bool presenterReady;
	bool headless;
	bool gameRunning;
	dungeon_type level;
	const SDL_Color *logicalPalette; /* 256 entries */
};
void LatchForFrame(const LatchInput &in);        // Enabled() = optionEnabled && presenterReady && !headless && gameRunning && level != DTYPE_TOWN
[[nodiscard]] bool Enabled();                    // latched value for this frame
[[nodiscard]] uint32_t FrameCounter();           // presentation-only counter incremented by LatchForFrame
[[nodiscard]] const SDL_Color *CurrentPalette(); // logical palette latched for this frame (keeps clx_render / clx_normal_map palette-library-free)

/** True when the back buffer currently holds a full-bright hybrid world pass (set by BeginWorldPass from Enabled(), kept across
 *  scrollrt_draw_game_screen re-presents). The presenter's hybrid.enabled is THIS, not Enabled(): a settings-menu toggle between
 *  frames must not present a full-bright frame through the legacy composite or a classic frame through the ambient. */
[[nodiscard]] bool BackBufferIsHybrid();
/** BackBufferIsHybrid = false. Called whenever the back buffer stops holding the world pass while a game runs (ClearScreenBuffer
 *  before a loading screen, play_movie before an in-game movie): those presents must take the legacy path, never the composite
 *  over the previous frame's planes. */
void InvalidateBackBuffer();

void BeginWorldPass(int viewportRows); // BackBufferIsHybrid = Enabled(); if Enabled(): memset(A rows [0,viewportRows), Unlit) (ALL viewport rows, also under zoom); CurrentMode = CaptureWorld
void EndWorldPass();                   // CurrentMode = StampUnlit (no-op unless Enabled() or BackBufferIsHybrid(): a re-present of a hybrid frame stamps too)
void EndFrame();                       // CurrentMode = Off

// ---- Tile context (set by DrawCell/DrawFloorTile/DrawDirtTile before RenderTile*, and by DrawWorldSprite for arches; copied into TileRenderCommand) ----
void SetTileContext(Kind kind, uint8_t normalIdx, int anchorRow, int leftX, FootKind foot); // fills CurrentTile* incl. the 32-byte table
void BuildBaseRow(std::array<uint8_t, 32> &out, int anchorRow, FootKind foot);              // out[c] = (anchorRow - foot(c)) & 0xFF
[[nodiscard]] constexpr int FootRows(FootKind foot, int c) { return foot == FootKind::SW ? (31 - c) / 2 : foot == FootKind::SE ? c / 2 : 0; }
/** The same wall lines across a whole 64-px cell (c in [0, 64)): SE = c / 2 (plane x = cell + 0.5), SW = (63 - c) / 2 (plane y = cell + 0.5). */
[[nodiscard]] constexpr int ArchFootRows(FootKind foot, int c) { return foot == FootKind::SW ? (63 - c) / 2 : foot == FootKind::SE ? c / 2 : 0; }
/**
 * @brief Re-anchors an arch on its wall line.
 *
 * The arch sprite is blitted with a constant aux byte and the tile base as B (foot None), which put its ground point on the
 * cell's base row, a tile off the wall line it stands on. Call it right after the arch blit: every pixel inside `rect` (in
 * `out` coordinates) that holds `auxByte` and B == `anchorRow & 0xFF` gets B = anchorRow - ArchFootRows(foot, x - leftX), so the
 * presenter lights the arch like the wall pieces next to it. No-op unless CaptureActive() and `out` lies in the planes.
 */
void AnchorArchOnWallLine(const Surface &out, Rectangle rect, uint8_t auxByte, int anchorRow, int leftX, FootKind foot);

// ---- UI stamping (StampUnlit mode) ----
void StampUnlit(const Surface &out, Rectangle rect); // clipped to out and to the planes; no-op unless StampActive() && covers(out)
DVL_ALWAYS_INLINE void StampUnlitRun(const uint8_t *dst, unsigned n) { std::memset(CurrentPlanes.auxAt(dst), UnlitByte, n); }

// ---- Zoom / cursor ----
void ZoomInPlace(int viewportWidth, int viewportOffsetX, int outHeight, int zoomQuarters = 8); // scales plane A up exactly like Zoom() (scrollrt.cpp, HdUpscaleInPlace); plane B is NOT scaled
void SaveRect(Rectangle rect, std::span<uint8_t> auxOut);                // DrawCursor; plane A only (StampUnlit never writes B); no-op unless BackBufferIsHybrid()
void RestoreRect(Rectangle rect, std::span<const uint8_t> aux);          // UndrawCursor; same gate

// ---- Upload views ----
[[nodiscard]] std::span<const uint8_t> AuxRows(int rows);  // pitch*rows bytes
[[nodiscard]] std::span<const uint8_t> BaseRows(int rows); // under zoom the caller passes the world rows (HdWorldTransform::world.height)

// ---- Normals ----
struct NormalLutEntry {
	float x, y, z, pad;
};
[[nodiscard]] const std::array<NormalLutEntry, 32> &NormalLut();              // tile-space unit vectors
[[nodiscard]] uint8_t QuantizeNormal(float x, float y, float z);              // argmax dot over entries 3..31 (entry 3 = C' included)
void CardToTile(float cx, float cy, float cz, float &tx, float &ty, float &tz);

// ---- Palette emissive heuristic ----
void BuildPaletteEmissive(std::span<const SDL_Color, 256> logicalPalette, dungeon_type level, std::span<float, 256> out);

/**
 * @brief Classic darkening per light level: the mean luminance the light table of each level leaves of a palette colour.
 *
 * `out[level]` is the average over the palette of `lum(palette[lightTables[level][i]]) / lum(palette[i])`, so the hybrid
 * ambient follows the same ramp walk (and snap to black past the ramp end) as classic `dLight` shading instead of a
 * linear `(15 - dLight) / 15`. `out[0]` is 1 outside Hell, `out[15]` is 0 (the fully dark table).
 */
void BuildClassicAmbientCurve(std::span<const SDL_Color, 256> palette,
    std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels> lightTables, std::span<float, NumLightingLevels> out);

} // namespace devilution::gbuffer
