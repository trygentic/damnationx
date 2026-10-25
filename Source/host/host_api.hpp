#pragma once

/**
 * @file host_api.hpp
 *
 * Embedding API for running DamnationX as a library inside another host (the Bevy host under host/).
 *
 * The host owns the window, the frame loop, input devices, audio output and all world rendering. The game owns
 * simulation, the classic UI and save data. Each frame the host submits one input frame, advances the game, then
 * reads one snapshot. Everything in the snapshot is plain data with fixed layouts so it can be bound from Rust
 * (cxx ExternType Trivial) without hand-written C wrappers.
 *
 * Contract version 3. Changes are additive only: new fields go at the end of a struct and bump
 * kHostApiVersion; existing fields keep their meaning and offset.
 *
 * Version 2 (additive, no struct changed): kHostMonsterSoundBase, HostSoundPath, HostReadAsset; HostInit accepts
 * any apiVersion from 1 to kHostApiVersion; the entity id encoding is spelled out (index + 1, see HostEntity::id).
 *
 * Version 3 (additive, no existing field moved): render modes (HostRenderMode, HostSetRenderMode, HostGetRenderMode,
 * HostSetRenderModeCallback; HostTickResult::reserved[0] reports a mode change), installed pack tiers
 * (HostInstalledPacks, HostHdDirectory), the Hd2D draw list (HostHdSnapshot: HostTexture, HostHdDraw, HostHdMask,
 * HostHdFrame), the World3D extras (HostWorldSnapshot: HostPlacement, HostEnvironment, HostModelHint,
 * HostWorldFrame), the entity flag kEntityReplacedByMesh, and HostReadAsset reading "hd/..." from the HD pack folder.
 *
 * Version 4: HostInputFrame grows at its end (wheelX, wheelY, inputFlags, reserved4; read only from hosts that pass
 * apiVersion >= 4), HostInputFlag (kInputNoWorldPick), the classic sprite art of every entity (HostEntityArtSnapshot:
 * HostSpriteAtlas, HostEntitySprite, HostModelClip, HostEntityArt). Text input no longer goes through SDL's event
 * queue, and outside World3D the game picks the world from the pointer itself.
 *
 * Version 5 (additive, no existing field moved): which modal UI is open (HostUiStateSnapshot: HostUiFlag,
 * HostUiState), the 2D view transform and the entities' screen rectangles (HostView2DSnapshot: HostView2D,
 * HostEntityScreenRect, HostView2DFrame), the audio settings (HostAudioSnapshot: HostAudioState; the settings menu shows
 * sound and music as available when embedded), and game state checkpoints (HostSaveState, HostLoadState).
 */

#include <cstdint>

namespace devilution::host {

constexpr uint32_t kHostApiVersion = 5;

/**
 * Besides the game's own options (--data-dir, --save-dir, --config-dir, --lang, --spawn, --diablo, --hellfire, ...),
 * argv may carry options for the embedding, which the game's parser never sees:
 *   --host-hero <name>       skip the main menu and start a single player game with this hero (created when missing)
 *   --host-class <class>     class for a hero --host-hero creates: warrior (default), rogue, sorcerer, monk, bard,
 *                            barbarian
 *   --host-continue          with --host-hero: load the hero's saved game instead of starting a new dungeon
 *   --host-assets-dir <dir>  the game's own assets (the build's damnationx_host_assets/ or assets/ folder)
 *   --host-hd-dir <dir>      an explicit HD pack folder
 *   --host-no-hd             disable HD pack discovery, including a supplied --host-hd-dir
 * Without --host-hero the game opens its main menu in the UI layer.
 */
struct HostInitParams {
	uint32_t apiVersion;      // 1 .. kHostApiVersion
	int32_t argc;             // command line passed through to the game's option parser (argv[0] is the program)
	const char *const *argv;
	uint32_t uiWidth;         // size of the classic UI layer the game draws (HostUiLayer), in pixels
	uint32_t uiHeight;
};

enum class HostButton : uint32_t {
	None = 0,
	Primary = 1U << 0,   // left click: walk / attack / pick up
	Secondary = 1U << 1, // right click: cast
	Middle = 1U << 2,
};

/** One frame of input. Keys use the game's own key codes (SDLK-compatible values, see controls/). */
struct HostInputFrame {
	// Pointer over the window, in UI-layer pixels. Used for the classic UI (panels, inventory, menus).
	int32_t pointerX;
	int32_t pointerY;
	uint32_t buttonsDown;     // HostButton bits currently held
	uint32_t buttonsPressed;  // HostButton bits that went down this frame
	uint32_t buttonsReleased; // HostButton bits that went up this frame
	// World picking, resolved by the host's 3D raycast. Ignored while the pointer is over a UI panel.
	int32_t hoverTileX; // -1 when the ray misses the dungeon
	int32_t hoverTileY;
	uint32_t hoverEntityId; // HostEntity::id under the pointer, 0 for none
	// Keyboard: up to 16 key transitions per frame, in order.
	uint32_t keyCount;
	struct Key {
		int32_t keycode;
		uint16_t modifiers; // SDL_Keymod-compatible bits
		uint8_t down;       // 1 pressed, 0 released
		uint8_t repeat;
	} keys[16];
	// UTF-8 text typed this frame (chat, hero name), NUL-terminated.
	char text[64];
	// Gamepad: SDL_GamepadButton-compatible bit set and axes in [-32768, 32767].
	uint32_t padButtons;
	int16_t padAxes[6];
	uint16_t reserved;
	// ---- v4 (read only when HostInitParams::apiVersion >= 4) ----
	int32_t wheelX;      // mouse wheel notches this frame: +x right, +y away from the user (scroll up)
	int32_t wheelY;
	uint32_t inputFlags; // HostInputFlag bits
	uint32_t reserved4;
};

/** Version 4: HostInputFrame::inputFlags. */
enum HostInputFlag : uint32_t {
	/**
	 * The host has no 3D pick this frame (hoverTile/hoverEntity are meaningless): the game picks from pointerX/Y in its
	 * own isometric view. Outside the World3D render mode the game always picks from the pointer.
	 */
	kInputNoWorldPick = 1U << 0,
	kInputToggleRun = 1U << 1,
	/** Toggle the first-person camera mode (Graphics > Camera Mode), like the game's ToggleFirstPerson key action. */
	kInputToggleFirstPerson = 1U << 2,
};

struct HostTickResult {
	uint32_t gameTicks;  // simulation ticks run (0..n; the game runs at its fixed 20 Hz)
	uint8_t quitRequested;
	uint8_t inGame;      // 0 while in DiabloUI menus (main menu, hero select): render the UI layer only
	uint8_t reserved[2]; // v3: reserved[0] = 1 when the player changed the render mode in the settings menu this tick
};

/** Mirrors render3d::WorldVertex (40 bytes). */
struct HostWorldVertex {
	float px, py, pz;
	float u, v;
	float nx, ny, nz;
	uint32_t flags; // render3d::WorldVertexFlag bits | albedo scale << 8
	uint32_t reserved;
};
static_assert(sizeof(HostWorldVertex) == 40);

struct HostLevel {
	uint32_t revision;  // changes whenever geometry or the atlas below changes; rebuild host meshes only then
	uint8_t levelType;  // dungeon_type
	uint8_t currentLevel;
	uint8_t setLevel;
	uint8_t reserved;
	const HostWorldVertex *vertices;
	uint32_t vertexCount;
	const uint32_t *indices;
	uint32_t indexCount;
	// Level texture atlas the vertex UVs point into, RGBA8.
	const uint8_t *atlasRgba;
	uint32_t atlasWidth;
	uint32_t atlasHeight;
};

enum class HostEntityKind : uint8_t {
	Player = 0,
	Monster = 1,
	Towner = 2,
	Object = 3,  // chests, shrines, doors, levers, barrels
	Item = 4,    // items lying on the floor
	Missile = 5, // spells and projectiles
};

/**
 * One drawable game entity. Positions are in world units (1 tile = 1 unit, x east, z south, y up) with the
 * walking offset already applied, so the host does not need the game's tile-step interpolation.
 */
struct HostEntity {
	uint32_t id;    // stable for the entity's lifetime: kind << 24 | (index + 1); never 0
	HostEntityKind kind;
	uint8_t facing; // Direction, 0..7
	uint8_t flags;  // HostEntityFlag bits
	uint8_t reserved;
	// What to draw. typeId is the game's own id for the kind: player class, monster type (_monster_id),
	// towner type, object type (_object_id), item base (ItemCursorGraphic), missile id (MissileID).
	uint32_t typeId;
	uint32_t variant;   // player: armor/weapon class bits; monster: unique id + 1 or 0; item: unique/magic tier
	float x, y, z;
	uint16_t anim;      // player_graphic / MonsterGraphic / object frame set; chests: 0 closed, 2 open
	uint16_t frame;     // current frame within anim
	uint16_t frameCount;
	uint16_t lightRadius; // 0 for none
	float animProgress;   // 0..1 through the current frame, for smooth blending
};

enum HostEntityFlag : uint8_t {
	kEntityHighlighted = 1U << 0, // under the cursor / target
	kEntityDead = 1U << 1,
	kEntityInvisible = 1U << 2,
	kEntityLocalPlayer = 1U << 3,
	kEntityReplacedByMesh = 1U << 4, // v3: a 3D pack placement (HostWorldFrame::placements) draws this object instead
};

struct HostLight {
	float x, y, z;
	float radius; // tiles
	uint8_t r, g, b;
	uint8_t flags; // 1 = attached to an entity (missile, player light)
	uint32_t entityId;
};

/** The classic UI drawn by the game into an RGBA8 layer with alpha; transparent where the world shows through. */
struct HostUiLayer {
	uint32_t revision; // unchanged revision means the pixels did not change
	uint32_t width;
	uint32_t height;
	const uint8_t *rgba;
	// Rectangle that changed since the previous revision, for partial texture uploads.
	uint32_t dirtyX, dirtyY, dirtyW, dirtyH;
};

/** A sound the game asked to play this frame. The host plays it with its own audio engine. */
struct HostSoundEvent {
	uint32_t soundId;     // SfxID, kHostMusicBase + track for music, kHostMonsterSoundBase + ... for monsters
	float x, y;           // world position; NaN for non-positional (UI, music)
	float volume;         // 0..1
	uint8_t stop;         // 1 = stop this soundId instead of playing it
	uint8_t loop;
	uint8_t reserved[2];
};
constexpr uint32_t kHostMusicBase = 0x10000;
/** Monster sounds: kHostMonsterSoundBase + monster type (_monster_id) * 8 + kind * 2 + variant (0..1), kind being
 * MonsterSound: 0 attack, 1 hit, 2 death, 3 special. */
constexpr uint32_t kHostMonsterSoundBase = 0x20000;

struct HostCamera {
	float targetX, targetY, targetZ; // local player position the camera follows
	uint8_t viewMode;                // the game's current camera mode (fpv/), for hosts that follow it
	uint8_t reserved[3];
};

struct HostFrame {
	uint32_t apiVersion;
	uint32_t gameTick;
	HostLevel level;
	HostCamera camera;
	const HostEntity *entities;
	uint32_t entityCount;
	const HostLight *lights;
	uint32_t lightCount;
	HostUiLayer ui;
	const HostSoundEvent *sounds;
	uint32_t soundCount;
};

/**
 * Starts the game without creating a window or a renderer. Loads the MPQs, options and saves as the normal
 * executable does. Returns false when the game data is missing or apiVersion does not match.
 */
bool HostInit(const HostInitParams &params);

/**
 * Imports owned Diablo II: Resurrected sounds without starting the game, SDL or a GPU.
 * The two completed packs are written under modsPath; mapDir contains the two d2_*_map.tsv files.
 * The caller owns staging/publication. Returns 0 only on a successful import; never call while HostInit is active.
 */
int32_t HostImportD2Sounds(const char *installPath, const char *modsPath, const char *mapDir);

/** Hands one frame of input to the game. Call once before each HostTick. */
void HostSubmitInput(const HostInputFrame &input);

/** Advances the game by the wall-clock time that passed, running as many 20 Hz ticks as are due. */
HostTickResult HostTick(uint32_t elapsedMs);

/**
 * Snapshot of the current state. Pointers stay valid until the next HostTick or HostShutdown.
 * Sound events are drained: each event appears in exactly one snapshot.
 */
const HostFrame &HostSnapshot();

/** Saves if needed and releases everything HostInit acquired. */
void HostShutdown();

/**
 * Version 2. The game asset path of a sound id (e.g. "sfx\\misc\\walk1.wav", "music\\dtowne.wav"), for
 * HostReadAsset. nullptr for an unknown id. The pointer stays valid until HostShutdown.
 */
const char *HostSoundPath(uint32_t soundId);

/**
 * Version 2. Reads a game asset (MPQ or loose file, the game's own lookup order) into `buffer`: copies
 * min(size, capacity) bytes and returns the asset's full size, 0 when it does not exist. Call with capacity 0 to
 * get the size. Call between HostTick calls (not concurrently with them).
 * Version 3: paths starting with "hd/" are read from the HD pack folder (HostHdDirectory), like the game's HD loader.
 */
uint32_t HostReadAsset(const char *path, uint8_t *buffer, uint32_t capacity);

// =====================================================================================================================
// Version 3: render modes, pack tiers, the Hd2D draw list and the World3D extras
// =====================================================================================================================

/**
 * How the host shows the game. Every mode keeps HostSnapshot (level, entities, lights, sounds) up to date; they differ
 * in what the game draws into HostFrame::ui and what else it exports.
 */
enum class HostRenderMode : uint32_t {
	/** The host renders the world (HostFrame level/entities/lights + HostWorldSnapshot); the UI layer is transparent
	 * where the world shows. */
	World3D = 0,
	/** The original graphics: the game draws the whole frame exactly as the SDL executable (isometric world, UI,
	 * palette, lighting, fades) into HostFrame::ui, fully opaque. The host presents it scaled. */
	Classic = 1,
	/** The HD pack's 2D look: HostFrame::ui holds the classic frame (opaque, as in Classic) and HostHdSnapshot lists the
	 * HD images to draw over it, in order. Without the pack's 2D art the draw list stays empty (Classic output). */
	Hd2D = 2,
};

/**
 * Switches the render mode (between HostTick calls). The mode is also a setting of the game (Settings > Graphics >
 * Render Mode, listed only when embedded) and is saved with the game's options. Before HostInit it sets the mode
 * the game starts in (otherwise the saved setting decides).
 */
void HostSetRenderMode(HostRenderMode mode);
HostRenderMode HostGetRenderMode();
/** Called (on the game thread, inside HostTick) when the player changes the mode in the settings menu; nullptr removes. */
using HostRenderModeCallback = void (*)(HostRenderMode mode, void *user);
void HostSetRenderModeCallback(HostRenderModeCallback callback, void *user);

/** Installed HD pack tiers (bits of HostInstalledPacks). */
enum HostPackFlag : uint32_t {
	kPackHd2D = 1U << 0,         // hd/sprites.txt, hd/tiles-*.txt or hd/ui.txt: the HD 2D art (Hd2D mode)
	kPackFree3D = 1U << 1,       // hd/3d/free/: the FREE 3D pack (placements)
	kPackHd3D = 1U << 2,         // hd/3d/hd/: the HD 3D pack (placements, wins over FREE)
	kPackCharacters3D = 1U << 3, // hd/3d/<class>/<class>.glb: skinned 3D characters
};
/** Valid after HostInit. */
uint32_t HostInstalledPacks();
/** The HD pack folder with a trailing separator, "" when there is none. Valid after HostInit. */
const char *HostHdDirectory();

// ---- Hd2D ----

enum class HostTextureKind : uint8_t {
	SpriteClip = 0, // a clip grid of hd/sprites.txt (and items.txt / objects.txt)
	TilePage = 1,   // a page of hd/tiles-<tileset>.txt
	UiSheet = 2,    // a sheet of hd/ui.txt
};

/** An image file of the HD pack the draw list refers to. The host decodes it (PNG or WebP). */
struct HostTexture {
	uint32_t id;       // HostHdDraw::texture: kind << 24 | index
	uint32_t revision; // changes when the file behind `id` changes (another tileset reuses tile page ids)
	HostTextureKind kind;
	uint8_t indexed;   // UI sheets: R = palette index, G = coverage, A = 255 where covered; drawn through the TRN and
	                   // HostHdFrame::palette like the classic blit. 0: straight RGBA art
	uint8_t reserved[2];
	uint32_t width;    // texels the source rectangles refer to (the full-size image)
	uint32_t height;
	uint32_t reserved2;
	const char *path;      // absolute file path
	const char *assetPath; // "hd/..." (HostReadAsset reads it)
};
static_assert(sizeof(HostTexture) == 24 + 2 * sizeof(void *));

enum class HostHdDrawKind : uint8_t {
	Tile = 0,    // a level piece; masked by HostHdFrame::tileMask
	Sprite = 1,  // a character, monster, towner, object or item; masked by spriteMask
	UiImage = 2, // a UI element; masked by uiMask
	UiTint = 3,  // a flat colour (`color`) over the dst rectangle; masked by uiMask
};

enum HostHdDrawFlag : uint8_t {
	kHdTranslucent = 1U << 0, // tiles: see-through wall, half opacity
	kHdDropShadow = 1U << 1,  // sprites: first the silhouette sheared onto the ground from pivotY (x -0.42, y -0.26 per
	                          // row of height, opacity 0.7, ~1.25 px blur), as the WebGPU path draws it
	kHdSelfLit = 1U << 2,     // full bright (light is 1)
	kHdOutline = 1U << 3,     // sprites: hover outline in `color` around the silhouette (~1 px per world pixel)
	kHdIndexed = 1U << 4,     // UI: the sheet is indexed (see HostTexture::indexed)
};

/**
 * One HD image to draw over the classic frame. Draw in list order (tiles, then sprites, then UI), each clipped to
 * clip* and masked per pixel by its kind's mask plane. Coordinates are UI-layer pixels (HostFrame::ui).
 * Colour: texel.rgb desaturated towards its luma by 1 - saturation, multiplied by tint and by light; UI images use
 * opacity and, for indexed sheets, index -> trnTables[trn] (when trn >= 0) -> palette.
 */
struct HostHdDraw {
	uint32_t texture; // HostTexture::id, 0 for UiTint
	HostHdDrawKind kind;
	uint8_t flags;    // HostHdDrawFlag bits
	uint16_t reserved;
	uint32_t maskIndex; // this draw's index in its kind's mask plane (see HostHdMask)
	int32_t trn;        // UI: index into HostHdFrame::trnTables, -1 for none
	float srcX, srcY, srcW, srcH; // texels of the texture
	float dstX, dstY, dstW, dstH; // UI-layer pixels
	int32_t clipX, clipY, clipW, clipH;
	float light;        // classic light level of the replaced pixels (0 dark .. 1 full bright) times the palette fade
	float opacity;
	float tint[3];
	float saturation;
	uint32_t color;     // UiTint: 0xAABBGGRR; sprites with kHdOutline: the outline colour
	float pivotY;       // sprites: UI-layer row of the feet (drop shadow origin)
};
static_assert(sizeof(HostHdDraw) == 96);

/**
 * A per-pixel plane over the UI layer that decides which HD texels the classic painter order hides (width x height,
 * row major; only the bounds rectangle is meaningful, outside it nothing hides). For a draw with maskIndex i at p:
 *  - tileMask:   hidden iff data[p] > i (0xFFFF hides every tile: UI or a later classic draw lies on top)
 *  - spriteMask: hidden iff (data[p] & 0x7FFF) > i; when hidden and bit 0x8000 is set, draw at half coverage instead
 *                (a see-through wall in front)
 *  - uiMask:     visible iff data[p] != 0xFFFF && i >= data[p]
 */
struct HostHdMask {
	const uint16_t *data;
	uint32_t width;
	uint32_t height;
	int32_t boundsX, boundsY, boundsW, boundsH;
};
static_assert(sizeof(HostHdMask) == 24 + sizeof(void *));

struct HostHdFrame {
	uint32_t revision;        // bumps with every presented Hd2D frame
	uint32_t textureRevision; // bumps whenever a texture is added or replaced
	const HostTexture *textures; // every texture known so far (ids stay valid for the run)
	uint32_t textureCount;
	uint32_t reserved0;
	const HostHdDraw *draws;
	uint32_t drawCount;       // 0 outside Hd2D or without the pack's 2D art
	uint32_t reserved1;
	HostHdMask tileMask;
	HostHdMask spriteMask;
	HostHdMask uiMask;
	const uint8_t *palette;   // 256 x RGBA: the live system palette (palette fades applied)
	const uint8_t *trnTables; // trnCount x 256 palette index maps
	uint32_t trnCount;
	float worldScale;         // UI-layer pixels per world pixel (2 under zoom)
};

/** The HD draw list of the last presented frame. Pointers stay valid until the next HostTick. */
const HostHdFrame &HostHdSnapshot();

// ---- World3D extras ----

/**
 * A static mesh of the 3D packs (hd/3d/placements/<leveltype>.ini, FREE and HD tiers merged, HD winning). HostLevel's
 * geometry already leaves out the tiles it replaces.
 */
struct HostPlacement {
	uint32_t meshId;         // stable per mesh file for the run (share GPU buffers)
	int32_t tileX;
	int32_t tileY;
	uint32_t objectEntityId; // the HostEntity it replaces (flagged kEntityReplacedByMesh), 0 for none
	float model[16];         // column-major glTF -> world (x east, y up, z south; 1 tile = 1 unit): offset, pivot,
	                         // yaw, per-axis scale and unit scale applied
	float boundsMin[3];      // world-space bounds
	float boundsMax[3];
	const char *assetPath;   // "hd/3d/..." .glb (HostReadAsset reads it; sha256 pins already checked)
	const char *path;        // absolute file path
};
static_assert(sizeof(HostPlacement) == 104 + 2 * sizeof(void *));

/** The [environment] overrides of the level type's placement file; unset fields keep the host's defaults. */
struct HostEnvironment {
	uint32_t setMask; // 1 ambient, 2 exposure, 4 moonDirection, 8 moonStrength
	float ambient;    // 0..4 flat ambient (town default 0.85, dungeons 0.03)
	float exposure;   // 0.1..4 (town 1.15, dungeons 1.35)
	float moonDirection[3]; // toward the moon, normalized (y up)
	float moonStrength;     // 0..4
	uint32_t reserved;
};
static_assert(sizeof(HostEnvironment) == 32);

/**
 * The 3D model the installed packs provide for an entity. Host resolution order: HD 3D pack > FREE 3D pack > D2R
 * import (host side) > HD 2D sprite billboard > classic sprite billboard.
 */
struct HostModelHint {
	uint32_t entityId;
	uint32_t pack;         // the HostPackFlag tier it comes from
	const char *assetPath; // "hd/3d/..." .glb
	const char *path;      // absolute file path
};

struct HostWorldFrame {
	uint32_t placementRevision; // changes whenever the placements change (level change, opened chest, pack reload)
	uint32_t placementCount;
	const HostPlacement *placements;
	HostEnvironment environment;
	const HostModelHint *modelHints; // entities of the last HostSnapshot with a pack model
	uint32_t modelHintCount;
	uint32_t reserved;
};

/** The World3D extras for the current level (in game, any mode). Pointers stay valid until the next HostTick. */
const HostWorldFrame &HostWorldSnapshot();

// =====================================================================================================================
// Version 4: the classic sprite art of the entities
// =====================================================================================================================

/**
 * RGBA8 atlas of the classic sprite frames the entities show (palette and TRN already applied, straight alpha,
 * transparent around each frame). Frames are added as entities need them; `revision` changes whenever pixels change.
 * When the level palette changes or the atlas is full it is cleared and refilled (a full upload then: the dirty
 * rectangle covers everything). HostLevel has no kVertexSpriteAtlas triangles (only the WebGPU billboards used it);
 * a host drawing such vertices would sample this atlas.
 */
struct HostSpriteAtlas {
	uint32_t revision;
	uint32_t width;
	uint32_t height;
	uint32_t dirtyX, dirtyY, dirtyW, dirtyH; // changed since the previous HostEntityArtSnapshot
	uint32_t reserved;
	const uint8_t *rgba;
};
static_assert(sizeof(HostSpriteAtlas) == 32 + sizeof(void *));

/**
 * The frame an entity shows this tick in the classic view (the facing the 2D game draws, with its TRN: unique
 * monsters, stone curse). Billboard convention of the game's own 3D view: 128 sprite pixels per world unit, the frame
 * horizontally centred on the entity and its feet `anchorY` pixels above the bottom row (classic art is drawn with its
 * bottom row 16 pixels below the tile centre).
 */
struct HostEntitySprite {
	uint32_t entityId;
	float srcX, srcY, srcW, srcH; // texels of HostSpriteAtlas
	uint16_t width;               // the frame in classic pixels
	uint16_t height;
	int16_t anchorX;              // pixels from the frame's left edge to the feet (width / 2)
	int16_t anchorY;              // pixels from the frame's top edge to the feet (height - 16)
	uint32_t flags;               // reserved, 0
};
static_assert(sizeof(HostEntitySprite) == 32);

/**
 * How to animate a HostModelHint model: the glTF clip names to try, best first, separated by ';' (the game's own 3D
 * view looks for `<action>_<weapon family>`, then the family without its shield, then `<action>`, with town clips
 * falling back to the dungeon ones and block / hit / cast to stand), and where in the clip the game animation is.
 */
struct HostModelClip {
	uint32_t entityId;
	uint8_t loop;      // 1: stand / walk (loop the clip), 0: play once and hold the last pose
	uint8_t reserved[3];
	float progress;    // 0..1 through the game animation (clip time = progress * clip duration)
	float hitProgress; // attacks: progress at the game's hit frame (align it with the clip's hit, 0.4 s in the
	                   // game's models); -1 otherwise
	uint32_t reserved2;
	uint32_t reserved3;
	const char *clips;
};
static_assert(sizeof(HostModelClip) == 24 + sizeof(void *));

struct HostEntityArt {
	HostSpriteAtlas atlas;
	const HostEntitySprite *sprites; // entities of the last HostSnapshot that show a frame
	uint32_t spriteCount;
	uint32_t reserved0;
	const HostModelClip *clips;      // one per HostWorldFrame::modelHints entry
	uint32_t clipCount;
	uint32_t reserved1;
};

/** Sprite art and model clips for the entities of the last HostSnapshot. Pointers stay valid until the next HostTick. */
const HostEntityArt &HostEntityArtSnapshot();

/**
 * The HD pack's 2D frame of an entity (additive, no version bump: a host that does not call HostEntityHdArtSnapshot
 * sees nothing new): the clip frame and recolour the Hd2D view draws for a towner, monster or player body, chosen the
 * same way but outside a world pass. Listed only when the pack's hd/sprites.txt has art for the entity (and Graphics >
 * HD Graphics is on); every other entity keeps its classic HostEntitySprite.
 *
 * Same billboard convention as HostEntitySprite, in HD pixels: `scale` HD pixels per classic pixel (the pack's 4x),
 * the cell horizontally placed with `anchorX` on the entity and the entity's ground `anchorY` pixels below the
 * cell's top edge (the clip pivot plus the classic frame's offset between its feet and HostEntitySprite::anchorY), so
 * dividing by `scale` gives the size and anchor the classic frame would have.
 */
struct HostEntityHdSprite {
	uint32_t entityId;
	uint32_t clip;                // index of the clip in hd/sprites.txt (stable for the run): one sheet per clip
	float srcX, srcY, srcW, srcH; // the frame's cell in texels of the full-size sheet
	uint32_t sheetWidth;          // the full-size sheet (cols x cell width by rows x cell height)
	uint32_t sheetHeight;
	float anchorX;                // HD pixels from the cell's left edge to the entity
	float anchorY;                // HD pixels from the cell's top edge to the entity's ground
	float scale;                  // HD pixels per classic pixel
	float tint[3];                // per channel multiplier after the desaturation (identity: 1)
	float saturation;             // 1: unchanged, 0: grey (luma)
	uint32_t flags;               // HostHdDrawFlag: kHdSelfLit
	const char *path;             // the sheet file (PNG/WebP in the HD pack folder)
};
static_assert(sizeof(HostEntityHdSprite) == 64 + sizeof(void *));

struct HostEntityHdArt {
	const HostEntityHdSprite *sprites; // entities of the last HostSnapshot drawn with HD 2D art
	uint32_t spriteCount;
	uint32_t reserved;
};

/** HD 2D frames of the entities of the last HostSnapshot. Pointers stay valid until the next HostTick. */
const HostEntityHdArt &HostEntityHdArtSnapshot();

// =====================================================================================================================
// Version 5: UI state, 2D view geometry, audio settings, state checkpoints
// =====================================================================================================================

/** What the game's UI shows (bits of HostUiState::flags). */
enum HostUiFlag : uint32_t {
	kUiMainMenu = 1U << 0,      // DiabloUI screens (main menu, hero selection, dialogs before the game)
	kUiLoading = 1U << 1,       // a level load / progress screen
	kUiStore = 1U << 2,         // a store or towner dialog (HostUiState::activeStore, storeTowner)
	kUiQuestText = 1U << 3,     // scrolling quest / towner speech text
	kUiGameMenu = 1U << 4,      // the in-game menu (Esc) and its sub-menus (options)
	kUiGoldDrop = 1U << 5,      // the gold split dialog (inventory)
	kUiGoldWithdraw = 1U << 6,  // the stash's gold withdraw dialog
	kUiHelp = 1U << 7,
	kUiCharacter = 1U << 8,     // character panel
	kUiInventory = 1U << 9,
	kUiSpellbook = 1U << 10,
	kUiQuestLog = 1U << 11,
	kUiStash = 1U << 12,
	kUiVisualStore = 1U << 13,  // the graphical store (left panel)
	kUiAutomap = 1U << 14,
	kUiChat = 1U << 15,         // typing a chat message
	kUiSpellSelect = 1U << 16,  // the speed-spell selection overlay
	kUiChatLog = 1U << 17,
	kUiPaused = 1U << 18,
	kUiDead = 1U << 19,         // the hero is dead (the "you have died" screen)
	kUiMessage = 1U << 20,      // a centred game message (e.g. "Game saved", "No automap available in town")
};

struct HostUiState {
	uint32_t flags;      // HostUiFlag bits
	uint8_t activeStore; // the game's TalkID (0 = none): 1 Smith, 5 Witch, 12 Boy, 14 Healer, 15 Storyteller, 21 Tavern, ...
	uint8_t storeTowner; // the towner the store belongs to (_talker_id: 0 Smith, 1 Healer, ..., HostEntity typeId of
	                     // towners), 0xFF for none
	uint8_t reserved[2];
	uint32_t revision;   // changes whenever flags or the store change
	uint32_t reserved2;
};
static_assert(sizeof(HostUiState) == 16);

/** Valid after any HostTick (also in menus). */
const HostUiState &HostUiStateSnapshot();

/**
 * The isometric view of the 2D modes (Classic, Hd2D), for aiming without a 3D pick: a world point (x east, z south,
 * the units of HostEntity) maps to UI-layer pixels as
 *   screenX = worldToScreen[0] * x + worldToScreen[1] * z + worldToScreen[2]
 *   screenY = worldToScreen[3] * x + worldToScreen[4] * z + worldToScreen[5]
 * (camera scrolling and zoom included; a tile's centre is at its integer coordinates + 0.5). It describes the frame of
 * the last tick in every render mode (in World3D it is the view the 2D modes would show).
 */
struct HostView2D {
	uint8_t valid;  // 0 outside a level
	uint8_t reserved[3];
	float zoom;     // UI-layer pixels per world-pass pixel (1, or 2 when zoomed in)
	float worldToScreen[6];
	int32_t viewportX, viewportY, viewportW, viewportH; // the world area of the UI layer (the panel lies below)
};
static_assert(sizeof(HostView2D) == 48);

/**
 * World3D: the host camera, for the overlays the game draws over the world in screen space (the ground item labels
 * of Alt / Show Item Labels). `screenFromWorld` is a column-major 4x4 matrix taking a world point (x east, y up,
 * z south, w = 1; the units of HostEntity) to homogeneous UI-layer pixels: screen = (r.x / r.w, r.y / r.w), in front
 * of the camera when r.w > 0. nullptr forgets it (no labels). Call between ticks; later frames use it.
 */
void HostSetWorldView(const float *screenFromWorld);

/**
 * Where an entity's classic sprite lies in the UI layer (2D modes): its frame centred on the feet with the bottom row
 * 16 world-pass pixels below them, as the game draws it. Clicking inside it targets the entity.
 */
struct HostEntityScreenRect {
	uint32_t entityId;
	int32_t x, y, w, h;
	float feetX, feetY; // UI-layer pixel of the feet (the tile centre for objects and items)
};
static_assert(sizeof(HostEntityScreenRect) == 28);

struct HostView2DFrame {
	HostView2D view;
	const HostEntityScreenRect *rects; // entities of the last HostSnapshot that show a sprite (none for kEntityInvisible)
	uint32_t rectCount;
	uint32_t reserved;
};

/** The 2D view of the last tick. Pointers stay valid until the next HostTick. */
const HostView2DFrame &HostView2DSnapshot();

/**
 * The game's audio settings for the host's mixer: linear gains 0..1 from the options (Settings > Audio, and the in-game
 * menu's sliders), already applied to the volume of every HostSoundEvent; music events carry the music gain. A change
 * bumps `revision` (apply it to the music that is playing).
 */
struct HostAudioState {
	float musicGain;
	float soundGain;
	float audioCuesGain;
	uint8_t musicOn;
	uint8_t soundOn;
	uint8_t reserved[2];
	uint32_t revision;
	uint32_t reserved2;
};
static_assert(sizeof(HostAudioState) == 24);

const HostAudioState &HostAudioSnapshot();

/**
 * Checkpoints of the game's own state, for time travel (single player, in a level, between HostTick calls). The state
 * is the game's save format (the hero, the current level with its monsters, items, objects and missiles, every visited
 * level, quests, seeds), so a restore is exactly a "Load Game" of a save made at that tick.
 *
 * HostSaveState writes the state into `buffer` and returns its size (0 when no state can be taken now). With a buffer
 * smaller than the size nothing is copied: call again with a large enough buffer (until the next HostTick the second
 * call reuses the state taken by the first). Cost: one save of the game into a temporary archive in the system's
 * temporary folder (deleted again), about 2 ms (release) and 50-500 KiB (it grows with the visited levels).
 *
 * HostLoadState restores a state taken by HostSaveState in this run (same hero), like the game menu's Load Game but from
 * a temporary copy of the state: the player's save archive is never written by either call (it changes only when the
 * game itself saves: menu save, level change, quit). The current level, the hero, quests, items and seeds come from the
 * state; other visited levels keep their latest state (the game's own level files). Cost: the level and its graphics
 * are loaded again, about 50 ms (release). Returns false when it cannot (multiplayer, not in a level, a malformed
 * state).
 */
uint32_t HostSaveState(uint8_t *buffer, uint32_t capacity);
bool HostLoadState(const uint8_t *data, uint32_t size);

/**
 * The local hero's name (the save's character name), "" outside a game. For host settings kept per hero (the model a
 * hero is drawn with). Valid until the next HostTick.
 */
const char *HostHeroName();
/** Text the game wants on the OS clipboard (a lobby invite link), or ""; taking it clears it. Valid until the
 * next call. */
const char *HostTakeClipboardText();
/** Base item mesh name for a player's weapon (slot 0) or shield (slot 1), e.g. Short_Sword / Buckler.
 * Empty for no item or an invalid entity/slot. Copy before the next call; between ticks only. */
const char *HostPlayerEquipmentName(uint32_t entityId, uint32_t slot);
/** Shortest naturally reachable interactable object of this _object_id, or 0. UINT32_MAX selects closed doors.
 * Uses the game's normal player pathfinder without changing game state. Between ticks only. */
uint32_t HostReachableObject(uint32_t typeId);

/** Pointer-free presentation data. Empty belt slots are all zero; count is 0 or 1 (no stacking). */
struct HostHudBeltSlot {
	int32_t type;
	int32_t miscId;
	int32_t graphicId;
	int32_t iconId;
	uint32_t count;
};

enum HostHudFlag : uint32_t {
	kHudInGame = 1U << 0,
	kHudDead = 1U << 1,
	kHudRunning = 1U << 2,
};

/** Additive v5 export. Life/mana retain core fixed-point units (64 = one displayed point).
 * XP values are cumulative; levelStartXP is the previous level's threshold (0 at level 1). Stamina is clamped to 0..100.
 * Spell/type are SpellID/SpellType, graphic is ItemCursorGraphic,
 * icon is the classic cursor frame (_iCurs + CURSOR_FIRSTITEM). Outside a game every field is zero.
 */
struct HostHudState {
	int32_t hp;
	int32_t maxHP;
	int32_t mana;
	int32_t maxMana;
	uint32_t playerLevel;
	uint32_t currentXP;
	uint32_t nextXP;
	uint32_t gold;
	int32_t selectedSpell;
	uint32_t selectedSpellType;
	uint32_t flags;
	uint32_t levelStartXP;
	uint32_t stamina;
	HostHudBeltSlot belt[8];
};
static_assert(sizeof(HostHudBeltSlot) == 20);
static_assert(sizeof(HostHudState) == 212);

/** Read on the host thread between ticks, like HostSnapshot; owns all its data. */
HostHudState HostHudStateSnapshot();

/** Copy the core's UTF-8 hover description into a NUL-terminated buffer. Returns UiFlags text colour.
 * Separate from HostHudState so the existing snapshot layout remains stable. */
uint32_t HostHoverInfo(uint8_t *text, uint32_t capacity);

/** Presentation only; default visible. A change marks the UI for full redraw on the next tick. */
void HostSetHudVisible(bool visible);

/** Toggle the local run preference between ticks. Returns false outside a game, when disabled, dead, or multiplayer. */
bool HostToggleRun();

/**
 * Whether the hero voices (sfx\warrior\, sfx\rogue\, sfx\sorceror\, sfx\monk\) come from the imported Diablo II
 * sound packs. The host passes true only while the local hero is drawn with a D2R model; the default (false) plays the
 * game's own class voice. Other imported sounds are unaffected. Takes effect for the next asset lookup (HostReadAsset).
 */
void HostSetHeroVoiceD2(bool d2);

} // namespace devilution::host
