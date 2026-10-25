// Adapted from Douglas Opan's diablo-3d town map manifest (Source/engine/render/town_editor_map.cpp); modified for
// DamnationX (see placement.hpp).
#include "render3d/placement.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>

namespace devilution::render3d {
namespace {

constexpr size_t kMaxFileBytes = 1024 * 1024;
constexpr size_t kMaxMeshes = 1024;
constexpr size_t kMaxPlacements = 8192;

std::string_view Trim(std::string_view value)
{
	const size_t begin = value.find_first_not_of(" \t\r");
	if (begin == std::string_view::npos)
		return {};
	return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
}

std::vector<std::string_view> SplitList(std::string_view text, char separator)
{
	std::vector<std::string_view> parts;
	for (;;) {
		const size_t at = text.find(separator);
		parts.push_back(Trim(text.substr(0, at)));
		if (at == std::string_view::npos)
			return parts;
		text.remove_prefix(at + 1);
	}
}

bool ParseInt(std::string_view text, int &out)
{
	text = Trim(text);
	if (text.empty())
		return false;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
	return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool ParseFloat(std::string_view text, float &out)
{
	text = Trim(text);
	if (text.empty())
		return false;
	// std::from_chars for float is missing from some of our toolchains (older libc++); strtof on a bounded copy.
	const std::string copy(text);
	char *end = nullptr;
	out = std::strtof(copy.c_str(), &end);
	return end == copy.c_str() + copy.size() && std::isfinite(out);
}

bool ParseTile(int value) { return value >= 0 && value < kPlacementGridSize; }

bool ValidId(std::string_view id)
{
	if (id.empty() || id.size() > 64)
		return false;
	return std::all_of(id.begin(), id.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
	});
}

bool ValidAssetPath(std::string_view path)
{
	if (path.empty() || path.size() > 256 || path.front() == '/' || path.find("..") != std::string_view::npos
	    || path.find("//") != std::string_view::npos || !(path.ends_with(".glb") || path.ends_with(".gltf")))
		return false;
	return std::all_of(path.begin(), path.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-';
	});
}

bool ValidSha256(std::string_view hex)
{
	return hex.size() == 64 && std::all_of(hex.begin(), hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct Entry {
	std::string_view key;
	std::string_view value;
	int line = 0;
};

struct Section {
	std::string_view name;
	int line = 0;
	std::vector<Entry> entries;
};

class Parser {
public:
	std::string error;

	bool Fail(int line, std::string_view reason)
	{
		error = "line " + std::to_string(line) + ": " + std::string(reason);
		return false;
	}

	bool SplitSections(std::string_view text, std::vector<Section> &sections)
	{
		int lineNumber = 0;
		while (!text.empty()) {
			++lineNumber;
			const size_t end = text.find('\n');
			std::string_view line = Trim(text.substr(0, end));
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (const size_t comment = line.find_first_of(";#"); comment != std::string_view::npos)
				line = Trim(line.substr(0, comment));
			if (line.empty())
				continue;
			if (line.front() == '[') {
				if (line.back() != ']')
					return Fail(lineNumber, "malformed section header");
				sections.push_back({ Trim(line.substr(1, line.size() - 2)), lineNumber, {} });
				continue;
			}
			const size_t equals = line.find('=');
			if (equals == std::string_view::npos)
				return Fail(lineNumber, "expected key = value");
			if (sections.empty())
				return Fail(lineNumber, "key outside a section");
			sections.back().entries.push_back({ Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)), lineNumber });
		}
		return true;
	}

	/** Every key of `section` must be in `allowed`; only keys in `repeatable` may repeat. */
	bool CheckKeys(const Section &section, std::initializer_list<std::string_view> allowed, std::initializer_list<std::string_view> repeatable = {})
	{
		std::set<std::string_view> seen;
		for (const Entry &entry : section.entries) {
			if (std::find(allowed.begin(), allowed.end(), entry.key) == allowed.end())
				return Fail(entry.line, "unknown key '" + std::string(entry.key) + "'");
			if (!seen.insert(entry.key).second && std::find(repeatable.begin(), repeatable.end(), entry.key) == repeatable.end())
				return Fail(entry.line, "duplicate key '" + std::string(entry.key) + "'");
		}
		return true;
	}

	bool ParseCommon(const Entry &entry, PlacementCommon &common, const std::vector<PlacementMesh> &meshes, bool &handled)
	{
		handled = true;
		if (entry.key == "mesh") {
			const auto it = std::find_if(meshes.begin(), meshes.end(), [&](const PlacementMesh &mesh) { return mesh.id == entry.value; });
			if (it == meshes.end())
				return Fail(entry.line, "unknown mesh '" + std::string(entry.value) + "' (declare [mesh.<id>] before using it)");
			common.mesh = static_cast<int>(it - meshes.begin());
		} else if (entry.key == "offset") {
			const auto parts = SplitList(entry.value, ',');
			if (parts.size() != 3 || !ParseFloat(parts[0], common.offset.x) || !ParseFloat(parts[1], common.offset.y) || !ParseFloat(parts[2], common.offset.z)
			    || std::abs(common.offset.x) > 64.0F || std::abs(common.offset.y) > 64.0F || std::abs(common.offset.z) > 64.0F)
				return Fail(entry.line, "offset must be x,y,z within +-64");
		} else if (entry.key == "yaw") {
			if (!ParseFloat(entry.value, common.yawDegrees) || std::abs(common.yawDegrees) > 360.0F)
				return Fail(entry.line, "yaw must be degrees within +-360");
		} else if (entry.key == "scale") {
			const auto parts = SplitList(entry.value, ',');
			float v[3] {};
			bool ok = parts.size() == 1 || parts.size() == 3;
			for (size_t i = 0; ok && i < parts.size(); ++i)
				ok = ParseFloat(parts[i], v[i]) && v[i] > 0.0F && v[i] <= 100.0F;
			if (!ok)
				return Fail(entry.line, "scale must be s or x,y,z, each in (0, 100]");
			common.scale = parts.size() == 1 ? skm::Vec3 { v[0], v[0], v[0] } : skm::Vec3 { v[0], v[1], v[2] };
		} else if (entry.key == "occluder") {
			if (entry.value == "auto") {
				common.occluder = PlacementOccluder::Auto;
			} else if (entry.value == "none") {
				common.occluder = PlacementOccluder::None;
			} else if (ParseFloat(entry.value, common.occluderTop) && common.occluderTop > 0.0F && common.occluderTop <= 60.0F) {
				common.occluder = PlacementOccluder::Fixed;
			} else {
				return Fail(entry.line, "occluder must be auto, none or a height in (0, 60]");
			}
		} else if (entry.key == "levels") {
			const auto parts = SplitList(entry.value, '-');
			int lo = 0;
			int hi = 0;
			if (parts.size() == 1 && ParseInt(parts[0], lo)) {
				hi = lo;
			} else if (parts.size() != 2 || !ParseInt(parts[0], lo) || !ParseInt(parts[1], hi)) {
				return Fail(entry.line, "levels must be N or N-M");
			}
			if (lo < 0 || hi < lo || hi > 255)
				return Fail(entry.line, "levels out of range");
			common.levelMin = lo;
			common.levelMax = hi;
		} else if (entry.key == "setlevel") {
			if (!ParseInt(entry.value, common.setLevel) || common.setLevel < 0 || common.setLevel > 255)
				return Fail(entry.line, "setlevel must be 0 (normal levels) or a set level number");
		} else {
			handled = false;
		}
		return true;
	}

	bool ParseRect(const Entry &entry, PlacementRect &rect)
	{
		const auto parts = SplitList(entry.value, ',');
		if (parts.size() != 4 || !ParseInt(parts[0], rect.x0) || !ParseInt(parts[1], rect.y0) || !ParseInt(parts[2], rect.x1) || !ParseInt(parts[3], rect.y1)
		    || !ParseTile(rect.x0) || !ParseTile(rect.y0) || !ParseTile(rect.x1) || !ParseTile(rect.y1) || rect.x1 < rect.x0 || rect.y1 < rect.y0)
			return Fail(entry.line, "hide must be an inclusive tile rect x0,y0,x1,y1 inside the 112 x 112 grid");
		return true;
	}

	bool Parse(std::string_view text, PlacementFile &file)
	{
		if (text.size() > kMaxFileBytes)
			return Fail(0, "file larger than 1 MiB");
		if (text.find('\0') != std::string_view::npos)
			return Fail(0, "NUL byte");
		if (text.starts_with("\xEF\xBB\xBF"))
			text.remove_prefix(3);
		std::vector<Section> sections;
		if (!SplitSections(text, sections))
			return false;
		if (sections.empty() || sections.front().name != "Placement")
			return Fail(sections.empty() ? 1 : sections.front().line, "the file must start with [Placement]");

		std::set<std::string_view> names;
		for (const Section &section : sections) {
			if (!names.insert(section.name).second)
				return Fail(section.line, "duplicate section [" + std::string(section.name) + "]");
		}

		const Section &header = sections.front();
		if (!CheckKeys(header, { "format", "version" }))
			return false;
		bool formatOk = false;
		bool versionOk = false;
		for (const Entry &entry : header.entries) {
			if (entry.key == "format")
				formatOk = entry.value == "damnationx.placement";
			else
				versionOk = entry.value == "1";
		}
		if (!formatOk || !versionOk)
			return Fail(header.line, "expected format = damnationx.placement and version = 1");

		for (size_t s = 1; s < sections.size(); ++s) {
			const Section &section = sections[s];
			if (section.name == "environment") {
				if (!ParseEnvironment(section, file.environment))
					return false;
				continue;
			}
			const size_t dot = section.name.find('.');
			const std::string_view kind = section.name.substr(0, dot);
			const std::string_view id = dot == std::string_view::npos ? std::string_view {} : section.name.substr(dot + 1);
			if (!ValidId(id))
				return Fail(section.line, "section must be [mesh.<id>], [instance.<id>] or [rule.<id>] with an id of letters, digits, '_', '-', '.'");
			if (file.meshes.size() >= kMaxMeshes || file.instances.size() + file.rules.size() >= kMaxPlacements)
				return Fail(section.line, "too many sections");
			if (kind == "mesh") {
				if (!ParseMesh(section, id, file))
					return false;
			} else if (kind == "instance") {
				if (!ParseInstance(section, id, file))
					return false;
			} else if (kind == "rule") {
				if (!ParseRule(section, id, file))
					return false;
			} else {
				return Fail(section.line, "unknown section kind '" + std::string(kind) + "'");
			}
		}
		return true;
	}

private:
	bool ParseEnvironment(const Section &section, PlacementEnvironment &environment)
	{
		if (!CheckKeys(section, { "ambient", "exposure", "moon_direction", "moon_strength" }))
			return false;
		for (const Entry &entry : section.entries) {
			float value = 0.0F;
			if (entry.key == "moon_direction") {
				const auto parts = SplitList(entry.value, ',');
				skm::Vec3 direction;
				if (parts.size() != 3 || !ParseFloat(parts[0], direction.x) || !ParseFloat(parts[1], direction.y) || !ParseFloat(parts[2], direction.z)
				    || direction.y <= 0.0001F || std::abs(direction.x) > 4.0F || direction.y > 4.0F || std::abs(direction.z) > 4.0F)
					return Fail(entry.line, "moon_direction must be x,height,z within +-4 with height > 0");
				environment.moonDirection = skm::Normalize(direction);
				continue;
			}
			const bool exposure = entry.key == "exposure";
			if (!ParseFloat(entry.value, value) || value < (exposure ? 0.1F : 0.0F) || value > 4.0F)
				return Fail(entry.line, std::string(entry.key) + (exposure ? " must be in [0.1, 4]" : " must be in [0, 4]"));
			if (entry.key == "ambient")
				environment.ambient = value;
			else if (exposure)
				environment.exposure = value;
			else
				environment.moonStrength = value;
		}
		return true;
	}

	bool ParseMesh(const Section &section, std::string_view id, PlacementFile &file)
	{
		if (!CheckKeys(section, { "file", "units", "sha256" }))
			return false;
		PlacementMesh mesh;
		mesh.id = id;
		for (const Entry &entry : section.entries) {
			if (entry.key == "file") {
				if (!ValidAssetPath(entry.value))
					return Fail(entry.line, "file must be a relative .glb / .gltf asset path ([A-Za-z0-9/._-], no '..')");
				mesh.file = entry.value;
			} else if (entry.key == "units") {
				if (entry.value == "tiles")
					mesh.units = PlacementUnits::Tiles;
				else if (entry.value == "meters")
					mesh.units = PlacementUnits::Meters;
				else
					return Fail(entry.line, "units must be tiles or meters");
			} else {
				if (!ValidSha256(entry.value))
					return Fail(entry.line, "sha256 must be 64 lowercase hex digits");
				mesh.sha256 = entry.value;
			}
		}
		if (mesh.file.empty())
			return Fail(section.line, "[mesh.*] needs file");
		file.meshes.push_back(std::move(mesh));
		return true;
	}

	bool ParseInstance(const Section &section, std::string_view id, PlacementFile &file)
	{
		if (!CheckKeys(section, { "mesh", "tile", "offset", "yaw", "scale", "hide", "occluder", "levels", "setlevel" }, { "hide" }))
			return false;
		PlacementInstance instance;
		instance.id = id;
		bool haveTile = false;
		for (const Entry &entry : section.entries) {
			bool handled = false;
			if (!ParseCommon(entry, instance, file.meshes, handled))
				return false;
			if (handled)
				continue;
			if (entry.key == "tile") {
				const auto parts = SplitList(entry.value, ',');
				if (parts.size() != 2 || !ParseInt(parts[0], instance.tileX) || !ParseInt(parts[1], instance.tileY) || !ParseTile(instance.tileX)
				    || !ParseTile(instance.tileY))
					return Fail(entry.line, "tile must be x,y inside the 112 x 112 grid");
				haveTile = true;
			} else { // hide
				PlacementRect rect;
				if (!ParseRect(entry, rect))
					return false;
				instance.hide.push_back(rect);
			}
		}
		if (instance.mesh < 0 || !haveTile)
			return Fail(section.line, "[instance.*] needs mesh and tile");
		file.instances.push_back(std::move(instance));
		return true;
	}

	bool ParseRule(const Section &section, std::string_view id, PlacementFile &file)
	{
		if (!CheckKeys(section, { "mesh", "object", "piece", "arch", "offset", "yaw", "scale", "hide", "occluder", "levels", "setlevel", "facing", "show" }))
			return false;
		PlacementRule rule;
		rule.id = id;
		for (const Entry &entry : section.entries) {
			bool handled = false;
			if (!ParseCommon(entry, rule, file.meshes, handled))
				return false;
			if (handled)
				continue;
			if (entry.key == "object") {
				for (const std::string_view part : SplitList(entry.value, ',')) {
					if (!part.starts_with("OBJ_") || !ValidId(part))
						return Fail(entry.line, "object must be a list of _object_id names such as OBJ_CHEST1, OBJ_TCHEST1");
					rule.objectNames.emplace_back(part);
				}
			} else if (entry.key == "arch") {
				if (entry.value != "any")
					return Fail(entry.line, "arch must be any");
				rule.arch = true;
			} else if (entry.key == "facing") {
				if (entry.value == "none")
					rule.facing = PlacementFacing::None;
				else if (entry.value == "open_side")
					rule.facing = PlacementFacing::OpenSide;
				else if (entry.value == "passage")
					rule.facing = PlacementFacing::Passage;
				else
					return Fail(entry.line, "facing must be none, open_side or passage");
			} else if (entry.key == "show") {
				if (entry.value == "always")
					rule.show = PlacementShow::Always;
				else if (entry.value == "unopened")
					rule.show = PlacementShow::Unopened;
				else if (entry.value == "closed")
					rule.show = PlacementShow::Closed;
				else
					return Fail(entry.line, "show must be always, unopened or closed");
			} else if (entry.key == "piece") {
				for (const std::string_view part : SplitList(entry.value, ',')) {
					int piece = 0;
					if (!ParseInt(part, piece) || piece < 0 || piece > 0xFFFF)
						return Fail(entry.line, "piece must be a list of dPiece numbers");
					rule.pieces.push_back(static_cast<uint16_t>(piece));
				}
			} else { // hide
				if (entry.value != "self")
					return Fail(entry.line, "rules only accept hide = self");
				rule.hideSelf = true;
			}
		}
		if (rule.mesh < 0)
			return Fail(section.line, "[rule.*] needs mesh");
		if ((rule.objectNames.empty() ? 0 : 1) + (rule.pieces.empty() ? 0 : 1) + (rule.arch ? 1 : 0) != 1)
			return Fail(section.line, "[rule.*] needs exactly one of object, piece or arch");
		if (rule.show != PlacementShow::Always && rule.objectNames.empty())
			return Fail(section.line, "show applies to object rules only");
		file.rules.push_back(std::move(rule));
		return true;
	}
};

bool LevelMatches(const PlacementCommon &common, const PlacementLevel &level)
{
	if (common.levelMin >= 0 && (level.currlevel < common.levelMin || level.currlevel > common.levelMax))
		return false;
	return common.setLevel < 0 || common.setLevel == level.setLevel;
}

ResolvedPlacement Resolve(const PlacementCommon &common, const PlacementFile &file, int source, bool fromRule, int tileX, int tileY,
    float extraYawDegrees = 0.0F, skm::Vec3 shift = {})
{
	ResolvedPlacement placement;
	placement.mesh = common.mesh;
	placement.source = source;
	placement.fromRule = fromRule;
	placement.tileX = tileX;
	placement.tileY = tileY;
	placement.model = PlacementModelMatrix(common, file.meshes[static_cast<size_t>(common.mesh)].units, tileX, tileY, extraYawDegrees, shift);
	placement.occluder = common.occluder;
	placement.occluderTop = common.occluderTop;
	return placement;
}

// Checked in this order by facing = open_side and the wall pull-out: +y, +x, -x, -y.
constexpr std::array<std::pair<int, int>, 4> kNeighbours { { { 0, 1 }, { 1, 0 }, { -1, 0 }, { 0, -1 } } };

bool Solid(const PlacementLevel &level, int x, int y)
{
	if (x < 0 || y < 0 || x >= kPlacementGridSize || y >= kPlacementGridSize)
		return true;
	return level.solid && level.solid(x, y);
}

/** Yaw (degrees) turning +Z toward (dx, dy): RotationY(yaw) maps +Z to (sin yaw, cos yaw). */
float YawToward(int dx, int dy)
{
	return std::atan2(static_cast<float>(dx), static_cast<float>(dy)) * (180.0F / 3.14159265358979323846F);
}

/** First walkable neighbour of (x, y), or {0, 0}. */
std::pair<int, int> OpenSide(const PlacementLevel &level, int x, int y)
{
	for (const auto &[dx, dy] : kNeighbours) {
		if (!Solid(level, x + dx, y + dy))
			return { dx, dy };
	}
	return { 0, 0 };
}

/** The side a thing standing at (x, y) faces: a walkable neighbour with a wall behind it, else the first walkable one. */
std::pair<int, int> FrontSide(const PlacementLevel &level, int x, int y)
{
	for (const auto &[dx, dy] : kNeighbours) {
		if (!Solid(level, x + dx, y + dy) && Solid(level, x - dx, y - dy))
			return { dx, dy };
	}
	return OpenSide(level, x, y);
}

bool ShowObject(PlacementShow show, const PlacementLevel::Object &object)
{
	switch (show) {
	case PlacementShow::Unopened:
		return !object.used;
	case PlacementShow::Closed:
		return object.door && !object.doorOpen;
	default:
		return true;
	}
}

template <typename T>
void MergeById(std::vector<T> &base, const std::vector<T> &over, const std::vector<int> &meshMap)
{
	for (T item : over) {
		if (item.mesh >= 0)
			item.mesh = meshMap[static_cast<size_t>(item.mesh)];
		const auto it = std::find_if(base.begin(), base.end(), [&](const T &existing) { return existing.id == item.id; });
		if (it != base.end())
			*it = std::move(item);
		else
			base.push_back(std::move(item));
	}
}

} // namespace

void MergePlacementFiles(PlacementFile &base, const PlacementFile &over)
{
	std::vector<int> meshMap;
	for (const PlacementMesh &mesh : over.meshes) {
		const auto it = std::find_if(base.meshes.begin(), base.meshes.end(), [&](const PlacementMesh &existing) { return existing.id == mesh.id; });
		if (it != base.meshes.end()) {
			*it = mesh;
			meshMap.push_back(static_cast<int>(it - base.meshes.begin()));
		} else {
			meshMap.push_back(static_cast<int>(base.meshes.size()));
			base.meshes.push_back(mesh);
		}
	}
	MergeById(base.instances, over.instances, meshMap);
	MergeById(base.rules, over.rules, meshMap);
	PlacementEnvironment &environment = base.environment;
	if (over.environment.ambient)
		environment.ambient = over.environment.ambient;
	if (over.environment.exposure)
		environment.exposure = over.environment.exposure;
	if (over.environment.moonDirection)
		environment.moonDirection = over.environment.moonDirection;
	if (over.environment.moonStrength)
		environment.moonStrength = over.environment.moonStrength;
}

PlacementParseResult ParsePlacementFile(std::string_view text)
{
	PlacementParseResult result;
	PlacementFile file;
	Parser parser;
	if (parser.Parse(text, file))
		result.file = std::move(file);
	else
		result.error = std::move(parser.error);
	return result;
}

skm::Mat4 PlacementModelMatrix(const PlacementCommon &common, PlacementUnits units, int tileX, int tileY, float extraYawDegrees, skm::Vec3 shift)
{
	constexpr float kDegreesToRadians = 3.14159265358979323846F / 180.0F;
	const float unitScale = units == PlacementUnits::Meters ? kPlacementMetersToWorld : 1.0F;
	const skm::Vec3 origin { static_cast<float>(tileX) + common.offset.x + shift.x, common.offset.y + shift.y,
		static_cast<float>(tileY) + common.offset.z + shift.z };
	const skm::Vec3 scale = common.scale * unitScale;
	return skm::Translation(origin) * skm::RotationY((extraYawDegrees + common.yawDegrees) * kDegreesToRadians)
	    * skm::ComposeTrs({}, {}, scale);
}

std::vector<ResolvedPlacement> ResolvePlacements(const PlacementFile &file, const PlacementLevel &level)
{
	std::vector<ResolvedPlacement> out;
	for (size_t i = 0; i < file.instances.size(); ++i) {
		const PlacementInstance &instance = file.instances[i];
		if (LevelMatches(instance, level))
			out.push_back(Resolve(instance, file, static_cast<int>(i), false, instance.tileX, instance.tileY));
	}
	for (size_t i = 0; i < file.rules.size(); ++i) {
		PlacementRule rule = file.rules[i];
		// Upgrade the two pinned, normalised stock meshes in already-installed
		// packs. New packs carry these dimensions in their rules. Custom meshes
		// and deliberately authored instance transforms remain untouched.
		if (rule.mesh >= 0 && static_cast<size_t>(rule.mesh) < file.meshes.size() && rule.scale.y < 2.0F) {
			const PlacementMesh &mesh = file.meshes[static_cast<size_t>(rule.mesh)];
			if (mesh.id == "arch_stone" && mesh.sha256 == "8a902f99745b65f5b99f8717c3a982eb2d5eb0a3c4f449f9fa24de5c0c0c0c30") {
				rule.scale.x *= 3.4F / 1.26F;
				rule.scale.y *= 3.67F;
			} else if (mesh.id == "door_arch_wooden" && mesh.sha256 == "4c4be068c681a6d9ed9b38d180cdba56e8147043de69d5f2540cc0c55d81828d") {
				rule.scale.x *= 4.0F / 2.3F;
				rule.scale.y *= 3.65F / 0.95F;
			}
		}
		if (!LevelMatches(rule, level))
			continue;
		if (!rule.pieces.empty()) {
			if (!level.piece)
				continue;
			for (int y = 0; y < kPlacementGridSize; ++y) {
				for (int x = 0; x < kPlacementGridSize; ++x) {
					if (std::find(rule.pieces.begin(), rule.pieces.end(), level.piece(x, y)) != rule.pieces.end() && out.size() < kMaxPlacements)
						out.push_back(Resolve(rule, file, static_cast<int>(i), true, x, y));
				}
			}
		} else if (rule.arch) {
			if (!level.arch)
				continue;
			for (int y = 0; y < kPlacementGridSize; ++y) {
				for (int x = 0; x < kPlacementGridSize; ++x) {
					const uint8_t facing = level.arch(x, y);
					if (facing == 0 || out.size() >= kMaxPlacements)
						continue;
					float yaw = 0.0F;
					if (rule.facing == PlacementFacing::Passage)
						yaw = facing == 2 ? 90.0F : 0.0F; // SE arches are cut into a wall along y: the passage runs along x
					else if (rule.facing == PlacementFacing::OpenSide)
						yaw = YawToward(FrontSide(level, x, y).first, FrontSide(level, x, y).second);
					ResolvedPlacement placement = Resolve(rule, file, static_cast<int>(i), true, x, y, yaw);
					placement.arch = true;
					out.push_back(placement);
				}
			}
		} else {
			for (const PlacementLevel::Object &object : level.objects) {
				if (out.size() >= kMaxPlacements || std::find(rule.objectTypes.begin(), rule.objectTypes.end(), object.type) == rule.objectTypes.end()
				    || !ShowObject(rule.show, object))
					continue;
				// Objects standing in a wall cell (torches, wall decorations) are pulled out toward the open neighbour, like
				// their billboards (world_sprites.cpp).
				skm::Vec3 shift {};
				const auto [openX, openY] = OpenSide(level, object.x, object.y);
				if (!object.door && Solid(level, object.x, object.y) && (openX != 0 || openY != 0))
					shift = { 0.56F * static_cast<float>(openX), 0.0F, 0.56F * static_cast<float>(openY) };
				float yaw = 0.0F;
				if (rule.facing == PlacementFacing::OpenSide) {
					const auto [frontX, frontY] = FrontSide(level, object.x, object.y);
					if (frontX != 0 || frontY != 0)
						yaw = YawToward(frontX, frontY);
				}
				else if (rule.facing == PlacementFacing::Passage)
					yaw = object.leftDoor ? 90.0F : 0.0F;
				ResolvedPlacement placement = Resolve(rule, file, static_cast<int>(i), true, object.x, object.y, yaw, shift);
				placement.objectIndex = object.index;
				out.push_back(placement);
			}
		}
	}
	return out;
}

void AppendHiddenRects(const PlacementFile &file, const ResolvedPlacement &placement, std::vector<PlacementRect> &out)
{
	if (!placement.fromRule) {
		const PlacementInstance &instance = file.instances[static_cast<size_t>(placement.source)];
		out.insert(out.end(), instance.hide.begin(), instance.hide.end());
		return;
	}
	const PlacementRule &rule = file.rules[static_cast<size_t>(placement.source)];
	if (rule.hideSelf && !rule.pieces.empty())
		out.push_back({ placement.tileX, placement.tileY, placement.tileX, placement.tileY });
}

std::string_view PlacementFileNameForLevelType(int leveltype)
{
	static constexpr std::array<std::string_view, 7> Names { "town.ini", "cathedral.ini", "catacombs.ini", "caves.ini", "hell.ini", "nest.ini", "crypt.ini" };
	if (leveltype < 0 || static_cast<size_t>(leveltype) >= Names.size())
		return {};
	return Names[static_cast<size_t>(leveltype)];
}

} // namespace devilution::render3d
