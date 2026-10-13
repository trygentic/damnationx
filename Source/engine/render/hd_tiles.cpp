#include "engine/render/hd_tiles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "engine/assets.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_view_transform.hpp"
#include "engine/surface.hpp"
#include "levels/gendung.h"
#include "utils/log.hpp"

namespace devilution {
namespace {

enum class ManifestState : uint8_t { Unloaded, Loading, Loaded, Failed };

struct TilesetSlot {
	const char *name;
	ManifestState state = ManifestState::Unloaded;
	hd_tiles::Manifest manifest;
};

std::array<TilesetSlot, 2> Tilesets { { { "town" }, { "cathedral" } } };

/** The non-tile draws of one cell (characters, missiles, items) stay inside this rectangle around its anchor. */
constexpr int DynamicLeft = 96;
constexpr int DynamicRight = 64 + 96;
constexpr int DynamicAbove = 256;
constexpr int DynamicBelow = 48;
constexpr size_t MaxInstances = hd_tiles::DynamicPlane::HiddenAll - 1;

std::vector<HdTileInstance> Instances;
hd_tiles::DynamicPlane Dynamic;
const hd_tiles::Manifest *FrameManifest = nullptr;
int FrameTileset = -1;
hd_tiles::PixelRect FrameViewport;
bool WorldPassEnabled = false;
hd_tiles::RecolourPass Recolour;

int TilesetForLevel()
{
	switch (leveltype) {
	case DTYPE_TOWN:
		return 0;
	case DTYPE_CATHEDRAL:
		return 1;
	default:
		return -1;
	}
}

/** Loads hd/tiles-<name>.txt from the assets, or polls the browser fetch; true once the manifest is usable. */
bool EnsureManifest(TilesetSlot &slot)
{
	const std::string file = std::string("tiles-") + slot.name + ".txt";
	const auto accept = [&](std::string_view text, std::string_view source) {
		std::expected<hd_tiles::Manifest, std::string> parsed = hd_tiles::ParseManifest(text, file);
		if (!parsed.has_value()) {
			LogError("HD tiles: {} ({}); the {} tiles stay classic", parsed.error(), source, slot.name);
			slot.state = ManifestState::Failed;
			return;
		}
		slot.manifest = std::move(*parsed);
		slot.state = ManifestState::Loaded;
		Log("HD tiles: {} images on {} pages from {} ({})", slot.manifest.images.size(), slot.manifest.pages.size(), file, source);
	};
	switch (slot.state) {
	case ManifestState::Loaded:
		return true;
	case ManifestState::Failed:
		return false;
	case ManifestState::Unloaded: {
		const std::string asset = "hd\\" + file;
		if (FindAsset(asset.c_str()).ok()) {
			auto data = LoadAsset(asset);
			if (data.has_value()) {
				accept(static_cast<std::string_view>(*data), "assets");
				return slot.state == ManifestState::Loaded;
			}
		}
		slot.state = ManifestState::Loading;
	}
		[[fallthrough]];
	case ManifestState::Loading: {
		std::string text;
		bool failed = false;
		const std::string url = "hd/" + file;
		if (HdTilesGpuFetchText(url.c_str(), text, failed)) {
			accept(text, url);
		} else if (failed) {
			Log("HD tiles: {} is unavailable; the {} tiles stay classic", url, slot.name);
			slot.state = ManifestState::Failed;
		}
		return slot.state == ManifestState::Loaded;
	}
	}
	return false;
}

/** Records the parts of one key; all-or-nothing (a part whose page is not resident keeps the whole key classic). */
bool RecordParts(const Surface &out, hd_tiles::Parts parts, Point anchor, int lightTableIndex, bool translucent)
{
	if (parts.empty() || FrameManifest == nullptr)
		return false;
	const hd_tiles::Manifest &manifest = *FrameManifest;
	bool resident = true;
	for (uint32_t i = 0; i < parts.count; ++i)
		resident = HdTilesGpuRequestPage(manifest.images[parts.first + i].page) && resident;
	if (!resident || Instances.size() + parts.count > MaxInstances)
		return false;

	const float scale = static_cast<float>(manifest.scale);
	const float originX = static_cast<float>(out.region.x + anchor.x);
	const float originY = static_cast<float>(out.region.y + anchor.y);
	const float viewLeft = static_cast<float>(out.region.x);
	const float viewTop = static_cast<float>(out.region.y);
	const float viewRight = viewLeft + static_cast<float>(out.w());
	const float viewBottom = viewTop + static_cast<float>(out.h());
	for (uint32_t i = 0; i < parts.count; ++i) {
		const hd_tiles::Image &image = manifest.images[parts.first + i];
		HdTileInstance instance {
			.image = parts.first + i,
			.x = originX + image.dx / scale,
			.y = originY + image.dy / scale,
			.width = image.width / scale,
			.height = image.height / scale,
			.lightTableIndex = lightTableIndex,
			.translucent = translucent,
		};
		if (instance.x >= viewRight || instance.y >= viewBottom || instance.x + instance.width <= viewLeft || instance.y + instance.height <= viewTop)
			continue;
		Instances.push_back(instance);
	}
	return true;
}

} // namespace

void HdTilesBeginWorldPass(const Surface &out, bool allowed)
{
	Recolour.reset(); // a recolour (RedBack) belongs to the world pass it was drawn over
	Instances.clear();
	FrameManifest = nullptr;
	Dynamic.reset(out.surface->w, out.surface->h);
	WorldPassEnabled = false;
	FrameViewport = { out.region.x, out.region.y, out.w(), out.h() };
	FrameTileset = TilesetForLevel();
	if (!allowed || FrameTileset < 0 || !HdTilesGpuAvailable())
		return;
	TilesetSlot &slot = Tilesets[FrameTileset];
	if (!EnsureManifest(slot))
		return;
	HdTilesGpuUseManifest(FrameTileset, slot.manifest);
	FrameManifest = &slot.manifest;
	WorldPassEnabled = true;
}

bool HdTilesActive()
{
	return WorldPassEnabled;
}

bool HdTilesWantedForLevel()
{
	const int tileset = TilesetForLevel();
	return tileset >= 0 && HdTilesGpuAvailable() && Tilesets[tileset].state != ManifestState::Failed;
}

bool HdTilesPreferFloorAtlas()
{
	static const bool Enabled = HdTilesGpuFlag("hdFloor") && HdTilesGpuFlag("hdAtlasFloor");
	return Enabled && HdGraphicsEnabled();
}

bool HdTilesRecordFloor(const Surface &out, Point tilePosition, int pieceId, Point targetBufferPosition, int lightTableIndex)
{
	if (!WorldPassEnabled)
		return false;
	return RecordParts(out, FrameManifest->findPiece(tilePosition.x, tilePosition.y, pieceId, hd_tiles::Layer::Floor),
	    targetBufferPosition, lightTableIndex, false);
}

bool HdTilesRecordCell(const Surface &out, Point tilePosition, int pieceId, Point targetBufferPosition, int lightTableIndex, bool translucent)
{
	if (!WorldPassEnabled)
		return false;
	return RecordParts(out, FrameManifest->findPiece(tilePosition.x, tilePosition.y, pieceId, hd_tiles::Layer::Cell),
	    targetBufferPosition, lightTableIndex, translucent);
}

bool HdTilesRecordSpecial(const Surface &out, Point specialPosition, int frame, Point bottomLeft, int lightTableIndex, bool translucent)
{
	if (!WorldPassEnabled)
		return false;
	HdTilesEndDynamic(out);
	if (RecordParts(out, FrameManifest->findSpecial(specialPosition.x, specialPosition.y, frame), bottomLeft, lightTableIndex, translucent))
		return true;
	HdTilesBeginDynamic(out, bottomLeft);
	return false;
}

void HdTilesBeginDynamic(const Surface &out, Point targetBufferPosition)
{
	if (!WorldPassEnabled || Instances.empty())
		return;
	const SDL_Surface &buffer = *out.surface;
	const int x = out.region.x + targetBufferPosition.x;
	const int y = out.region.y + targetBufferPosition.y;
	// Clip to the viewport: nothing outside it is drawn in HD.
	const int left = std::max(x - DynamicLeft, out.region.x);
	const int top = std::max(y - DynamicAbove, out.region.y);
	const int right = std::min(x + DynamicRight, out.region.x + out.w());
	const int bottom = std::min(y + DynamicBelow, out.region.y + out.h());
	if (left >= right || top >= bottom)
		return;
	Dynamic.beginSection(static_cast<const uint8_t *>(buffer.pixels), buffer.pitch, { left, top, right - left, bottom - top },
	    static_cast<uint16_t>(Instances.size()));
}

void HdTilesEndDynamic(const Surface &out)
{
	if (!Dynamic.sectionOpen())
		return;
	const SDL_Surface &buffer = *out.surface;
	Dynamic.endSection(static_cast<const uint8_t *>(buffer.pixels), buffer.pitch);
}

void HdTilesEndWorldContent(const Surface &out)
{
	if (!WorldPassEnabled)
		return;
	HdTilesEndDynamic(out);
	const SDL_Surface &buffer = *out.surface;
	Dynamic.snapshotFrame(static_cast<const uint8_t *>(buffer.pixels), buffer.pitch, { out.region.x, out.region.y, out.w(), out.h() });
}

void HdTilesApplyZoom(const Surface &zoomOut, int viewportWidth, int offsetX, int zoomQuarters)
{
	if (!WorldPassEnabled)
		return;
	const int height = zoomOut.h();
	Dynamic.zoomInPlace(viewportWidth, offsetX + zoomOut.region.x, zoomOut.region.y + height, zoomQuarters);
	const HdWorldTransform &transform = HdCurrentWorldTransform();
	const auto regionX = static_cast<float>(zoomOut.region.x);
	const auto regionY = static_cast<float>(zoomOut.region.y);
	for (HdTileInstance &instance : Instances) {
		instance.x = regionX + transform.ToScreenX(instance.x - static_cast<float>(FrameViewport.x));
		instance.y = regionY + transform.ToScreenY(instance.y - static_cast<float>(FrameViewport.y));
		instance.width = transform.ToScreenLength(instance.width);
		instance.height = transform.ToScreenLength(instance.height);
	}
	FrameViewport = { zoomOut.region.x + offsetX, zoomOut.region.y, viewportWidth, height };
	const SDL_Surface &buffer = *zoomOut.surface;
	Dynamic.snapshotFrame(static_cast<const uint8_t *>(buffer.pixels), buffer.pitch, FrameViewport);
}

void HdTilesInvalidate()
{
	Recolour.reset();
	Instances.clear();
	FrameManifest = nullptr;
	Dynamic.reset(Dynamic.width(), Dynamic.height());
}

void HdTilesFinishFrame(const uint8_t *pixels, int pitch, int width, int height)
{
	if (Instances.empty())
		return;
	if (pixels == nullptr || width != Dynamic.width() || height != Dynamic.height() || !Dynamic.finishFrame(pixels, pitch))
		HdTilesInvalidate();
}

void HdLayersBeginRecolour(const Surface &out, int height)
{
	const SDL_Surface &buffer = *out.surface;
	Recolour.begin(static_cast<const uint8_t *>(buffer.pixels), buffer.pitch, buffer.w, buffer.h,
	    { out.region.x, out.region.y, out.w(), std::min(height, out.h()) });
}

void HdLayersEndRecolour(const Surface &out)
{
	Recolour.end(static_cast<const uint8_t *>(out.surface->pixels), out.surface->pitch);
}

void HdLayersResetRecolour()
{
	Recolour.reset();
}

const uint8_t *HdLayersTrackedFrame(const uint8_t *pixels, int pitch, int width, int height, int &outPitch)
{
	return Recolour.trackedFrame(pixels, pitch, width, height, outPitch);
}

std::span<const HdTileInstance> HdTilesFrameInstances()
{
	return Instances;
}

const hd_tiles::DynamicPlane &HdTilesDynamicPlane()
{
	return Dynamic;
}

hd_tiles::PixelRect HdTilesFrameViewport()
{
	return FrameViewport;
}

int HdTilesFrameTileset()
{
	return FrameTileset;
}

const hd_tiles::Manifest *HdTilesFrameManifest()
{
	return Instances.empty() ? nullptr : FrameManifest;
}

} // namespace devilution
