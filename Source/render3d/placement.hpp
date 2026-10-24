#pragma once
/**
 * @file render3d/placement.hpp
 *
 * Static 3D mesh placement for the 3D world (docs/design/static-mesh-placement.md): which glTF meshes stand where in a
 * level, keyed to dungeon tiles, and which tile-built geometry they replace.
 *
 * Adapted from the town map manifest and import contract of Douglas Opan's diablo-3d (town_editor_map.cpp,
 * town_model_import.hpp, https://github.com/douglasopan/diablo-3d, Sustainable Use License 1.0). Modified for
 * DamnationX: glTF meshes drawn by our WebGPU world renderer instead of D3DMESH1 meshes and his CPU/D3D11 renderer,
 * tile-keyed instances and dungeon rules instead of a fixed binding table, and shadows through our ray-traced
 * occluder grid.
 *
 * This file is the pure part (no game globals, no file I/O): parsing a placement file and resolving it against a
 * level description. render3d/placement_runtime.hpp loads the files for the current level.
 *
 * World space is fpv_contract.hpp's: x = tile x, z = tile y, y up, 1 tile = 1 unit. Placements are visual only:
 * native collision, pathing, picking identities, triggers, saves and the RNG never change.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render3d/skinned_math.hpp"

namespace devilution::render3d {

/** Grid size of the levels placements are resolved against (MAXDUNX / MAXDUNY). */
constexpr int kPlacementGridSize = 112;

/** Scale from glTF meters to world units for `units = meters` (== fpv::kWarriorMetersToWorld). */
constexpr float kPlacementMetersToWorld = 0.36F;

enum class PlacementUnits : uint8_t {
	Tiles,  // 1 glTF unit = 1 tile
	Meters, // 1 glTF unit = kPlacementMetersToWorld tiles
};

struct PlacementMesh {
	std::string id;
	std::string file;
	PlacementUnits units = PlacementUnits::Tiles;
	/** Lowercase hex SHA-256 of the file, empty when not pinned. */
	std::string sha256;
};

/** Inclusive tile rectangle. */
struct PlacementRect {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;

	[[nodiscard]] bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

enum class PlacementOccluder : uint8_t {
	Auto,  // the mesh bounds (after the transform) write the shadow occluder grid
	None,  // casts no ray-traced shadow
	Fixed, // the covered cells block [0, occluderTop]
};

/** Automatic turn applied before `yaw` (yaw adds on top). */
enum class PlacementFacing : uint8_t {
	None,
	OpenSide, // +Z toward the first walkable neighbour (+y, +x, -x, -y): chests against a wall
	Passage,  // +Z along the passage through a door or arch: left doors / SE arches face +x, the others +y
};

/** When an object rule's mesh is shown. */
enum class PlacementShow : uint8_t {
	Always,
	Unopened, // until the object is used (_oSelFlag == 0: opened chest, broken barrel); then the native sprite returns
	Closed,   // doors: while closed; the built open door is drawn when open
};

/** The keys shared by instances and rules. */
struct PlacementCommon {
	std::string id;
	int mesh = -1; // index into PlacementFile::meshes
	/** World-unit offset of the mesh origin from the anchor tile's corner (default: the tile centre). */
	skm::Vec3 offset { 0.5F, 0.0F, 0.5F };
	float yawDegrees = 0.0F;
	/** Per-axis scale in model space (scale = s or scale = x,y,z). */
	skm::Vec3 scale { 1.0F, 1.0F, 1.0F };
	PlacementOccluder occluder = PlacementOccluder::Auto;
	float occluderTop = 0.0F;
	/** currlevel filter (inclusive), -1 = any. */
	int levelMin = -1;
	int levelMax = -1;
	/** -1: any; 0: only normal levels; N: only set level N (_setlevels). */
	int setLevel = -1;
};

/** A mesh at a fixed tile (town, set levels). */
struct PlacementInstance : PlacementCommon {
	int tileX = 0;
	int tileY = 0;
	std::vector<PlacementRect> hide;
};

/** One mesh on every tile or object that matches (procedural dungeons). */
struct PlacementRule : PlacementCommon {
	/** Object type names (_object_id enumerators such as OBJ_BARREL); empty for piece and arch rules. */
	std::vector<std::string> objectNames;
	/** Resolved object types (set by the runtime from objectNames; unknown names are dropped). */
	std::vector<int> objectTypes;
	/** dPiece values (0-based, as in dPiece[x][y]) for piece rules. */
	std::vector<uint16_t> pieces;
	/** arch = any: every cell with an arch (tile_geometry::ArchFacing != 0). */
	bool arch = false;
	/**
	 * Suppresses what the matched thing draws: a piece rule its tile's geometry, an arch rule the built lintel, an
	 * object rule the object's billboard (doors: the built closed door panel).
	 */
	bool hideSelf = false;
	PlacementFacing facing = PlacementFacing::None;
	PlacementShow show = PlacementShow::Always;
};

/**
 * Optional [environment] section: overrides of the world pass's lighting for this level type, after the shared
 * Tristram light profile of diablo-3d (town_lighting_profile.cpp, d3d-lighting.ini). Unset fields keep the renderer's
 * defaults (world_gpu.cpp).
 */
struct PlacementEnvironment {
	std::optional<float> ambient;       // 0..4, flat ambient light (town default 0.85, dungeons 0.03)
	std::optional<float> exposure;      // 0.1..4 (town default 1.15, dungeons 1.35)
	std::optional<skm::Vec3> moonDirection; // toward the moon / sun, world x, height, z (height > 0); normalized
	std::optional<float> moonStrength;  // 0..4 (town default 0.6, dungeons 0 = no sky light)
};

struct PlacementFile {
	PlacementEnvironment environment;
	std::vector<PlacementMesh> meshes;
	std::vector<PlacementInstance> instances;
	std::vector<PlacementRule> rules;
};

struct PlacementParseResult {
	std::optional<PlacementFile> file;
	/** "line N: reason" when parsing failed. */
	std::string error;
};

/**
 * Parses a placement file (format damnationx.placement, version 1). Strict: an unknown section or key, a duplicate,
 * a malformed value or an out-of-range tile rejects the whole file, so an authoring typo never half-applies.
 */
PlacementParseResult ParsePlacementFile(std::string_view text);

/**
 * Overlays `over` (a higher tier, e.g. the HD pack) on `base` (e.g. the free pack): sections with the same id are
 * replaced ([mesh.X] in `over` replaces mesh X for every placement that uses it), new ones are added, and
 * [environment] keys set in `over` win.
 */
void MergePlacementFiles(PlacementFile &base, const PlacementFile &over);

/** What ResolvePlacements needs to know about the level. */
struct PlacementLevel {
	int currlevel = 0;
	/** 0 on normal levels, the _setlevels value on a set level. */
	int setLevel = 0;
	/** dPiece[x][y]; may be empty when the file has no piece rules. */
	std::function<uint16_t(int x, int y)> piece;
	struct Object {
		int index = -1; // index into Objects
		int type = -1;  // _object_id
		int x = 0;
		int y = 0;
		bool used = false;     // _oSelFlag == 0 (opened chest, broken barrel)
		bool door = false;
		bool doorOpen = false;
		bool leftDoor = false; // the doorway is cut into a wall running along y
	};
	std::vector<Object> objects;
	/** Native solid cells (doors excluded); out of the grid counts as solid. Used by facing and the wall pull-out. */
	std::function<bool(int x, int y)> solid;
	/** tile_geometry::ArchFacing of the cell: 0 none, 1 SW (wall along x), 2 SE (wall along y). */
	std::function<uint8_t(int x, int y)> arch;
};

/** One mesh to draw. */
struct ResolvedPlacement {
	/** Index into PlacementFile::meshes. */
	int mesh = -1;
	/** Index of the instance (instance placements) or rule (rule placements) in the file. */
	int source = -1;
	bool fromRule = false;
	/** Model (glTF space) -> world: T(tile corner + offset) * RotationY(yaw) * S(scale * unit scale). */
	skm::Mat4 model;
	/** Anchor tile. */
	int tileX = 0;
	int tileY = 0;
	/** The matched object (object rules), -1 otherwise. */
	int objectIndex = -1;
	/** Arch rule match: the rule's hide = self removes the lintel of the anchor tile. */
	bool arch = false;
	PlacementOccluder occluder = PlacementOccluder::Auto;
	float occluderTop = 0.0F;
};

/**
 * Builds the model matrix of a placement anchored at (tileX, tileY): T(tile corner + offset + shift) *
 * RotationY(extraYaw + yaw) * S(scale * unit scale). `extraYawDegrees` and `shift` come from facing and the wall pull-out.
 */
skm::Mat4 PlacementModelMatrix(const PlacementCommon &common, PlacementUnits units, int tileX, int tileY, float extraYawDegrees = 0.0F,
    skm::Vec3 shift = {});

/** Every placement of `file` that applies to `level`, instances first, in file order. */
std::vector<ResolvedPlacement> ResolvePlacements(const PlacementFile &file, const PlacementLevel &level);

/** The tile rectangles a resolved placement hides (instance `hide` rects, or the anchor tile of a piece rule with hide = self). */
void AppendHiddenRects(const PlacementFile &file, const ResolvedPlacement &placement, std::vector<PlacementRect> &out);

/** The placement file name for a level type: town.ini, cathedral.ini, ... (empty for an unknown level type). */
std::string_view PlacementFileNameForLevelType(int leveltype);

} // namespace devilution::render3d
