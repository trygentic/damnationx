/**
 * @file host/host_hd.cpp
 *
 * The HD pack for an embedding host (contract v3): which tiers are installed, the GPU side of the HD 2D subsystems
 * (hd_sprites, hd_tiles, hd_ui) in builds without WebGPU, and the Hd2D draw list.
 *
 * The HD subsystems already separate what to draw from how: during the world pass they record instances (sprites,
 * tiles) and UI layers, and when the frame is presented they resolve per-pixel visibility planes against the final
 * 8-bit frame (hd_sprites::OcclusionTracker, hd_tiles::DynamicPlane, hd_ui::LayerTracker). Their WebGPU halves
 * (hd_*_webgpu.cpp) only load textures and draw those records. Without WebGPU, the stubs of those files ask this file:
 * in the Hd2D render mode every texture counts as resident (the host loads the files itself), and HostHdSnapshot
 * turns the records into HostHdDraw entries, the planes into HostHdMask, plus a texture table naming the pack files.
 */
#include "host/host_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <span>
#include <string_view>
#include <unordered_map>

#include "engine/assets.hpp"
#include "engine/dx.h"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/gbuffer.hpp"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/render/hd_tiles.hpp"
#include "engine/render/hd_ui.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution::host {

namespace {

std::string HdDir;
uint32_t Packs = 0;
bool UiIndexPresent = false;

// The tile manifest of the current world pass (HdUseTileManifest).
int TileSet = -1;
const hd_tiles::Manifest *TileManifest = nullptr;

// Texture table: ids stay valid for the run; strings live in a deque so their c_str() pointers stay put.
std::vector<HostTexture> Textures;
std::unordered_map<uint32_t, size_t> TextureIndex;
std::deque<std::string> Strings;
uint32_t TextureRevision = 0;

std::vector<HostHdDraw> Draws;
std::vector<uint8_t> PaletteRgba(256 * 4);
std::vector<uint8_t> TrnTables;
HostHdFrame Frame {};
uint32_t FrameRevision = 0;
/** The last present was an in-game Hd2D frame whose records are resolved (OnHdPresent). */
bool FrameResolved = false;

std::array<float, NumLightingLevels> AmbientCurve {};

constexpr uint32_t TextureId(HostTextureKind kind, uint32_t index)
{
	return (static_cast<uint32_t>(kind) << 24) | (index & 0xFFFFFF);
}

/** "hd/..." -> the file in the HD folder; anything else unchanged. */
std::string ResolveHdPath(std::string_view url)
{
	const bool inHd = !HdDir.empty() && (url.starts_with("hd/") || url.starts_with("hd\\"));
	std::string path { inHd ? url.substr(3) : url };
#ifndef _WIN32
	std::replace(path.begin(), path.end(), '\\', '/');
#endif
	if (inHd)
		path.insert(0, HdDir);
	return path;
}

std::string AssetPathOf(std::string_view packRelative)
{
	std::string asset { packRelative };
	std::replace(asset.begin(), asset.end(), '\\', '/');
	if (!asset.starts_with("hd/"))
		asset.insert(0, "hd/");
	return asset;
}

const char *Keep(std::string text)
{
	Strings.push_back(std::move(text));
	return Strings.back().c_str();
}

/** Adds or refreshes a texture entry; returns its id. */
uint32_t UseTexture(HostTextureKind kind, uint32_t index, std::string_view packRelative, int width, int height, bool indexed)
{
	const uint32_t id = TextureId(kind, index);
	const std::string asset = AssetPathOf(packRelative);
	auto it = TextureIndex.find(id);
	if (it != TextureIndex.end()) {
		HostTexture &texture = Textures[it->second];
		if (asset == texture.assetPath && texture.width == static_cast<uint32_t>(width) && texture.height == static_cast<uint32_t>(height))
			return id;
		// Same id, another file (a tile page of another tileset).
		texture.assetPath = Keep(asset);
		texture.path = Keep(ResolveHdPath(asset));
		texture.width = static_cast<uint32_t>(width);
		texture.height = static_cast<uint32_t>(height);
		texture.indexed = indexed ? 1 : 0;
		++texture.revision;
		++TextureRevision;
		return id;
	}
	HostTexture texture {};
	texture.id = id;
	texture.revision = 1;
	texture.kind = kind;
	texture.indexed = indexed ? 1 : 0;
	texture.width = static_cast<uint32_t>(width);
	texture.height = static_cast<uint32_t>(height);
	texture.assetPath = Keep(asset);
	texture.path = Keep(ResolveHdPath(asset));
	TextureIndex.emplace(id, Textures.size());
	Textures.push_back(texture);
	++TextureRevision;
	return id;
}

float PaletteFade()
{
	double logical = 0.0;
	double system = 0.0;
	for (size_t i = 0; i < 256; ++i) {
		logical += logical_palette[i].r + logical_palette[i].g + logical_palette[i].b;
		system += system_palette[i].r + system_palette[i].g + system_palette[i].b;
	}
	return logical > 0.0 ? static_cast<float>(std::clamp(system / logical, 0.0, 1.0)) : 1.0F;
}

/** Brightness of a classic light table, as the WebGPU legacy composite lights HD texels (gbuffer ambient curve). */
float LightOf(int lightTableIndex)
{
	if (lightTableIndex <= 0)
		return 1.0F;
	return AmbientCurve[static_cast<size_t>(std::min<int>(lightTableIndex, NumLightingLevels - 1))];
}

uint32_t PaletteColor(int index)
{
	if (index < 0 || index > 255)
		return 0;
	const SDL_Color &c = system_palette[static_cast<size_t>(index)];
	return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16 | 0xFF000000U;
}

HostHdMask MaskOf(const std::vector<uint16_t> &plane, int width, int height, int bx, int by, int bw, int bh)
{
	HostHdMask mask {};
	if (plane.empty() || width <= 0 || height <= 0 || plane.size() < static_cast<size_t>(width) * height)
		return mask;
	mask.data = plane.data();
	mask.width = static_cast<uint32_t>(width);
	mask.height = static_cast<uint32_t>(height);
	mask.boundsX = bx;
	mask.boundsY = by;
	mask.boundsW = bw;
	mask.boundsH = bh;
	return mask;
}

void AddTileDraws(float fade)
{
	const hd_tiles::Manifest *manifest = HdTilesFrameManifest();
	if (manifest == nullptr || manifest->scale <= 0)
		return;
	const int tileset = HdTilesFrameTileset();
	const hd_tiles::PixelRect viewport = HdTilesFrameViewport();
	const std::span<const HdTileInstance> instances = HdTilesFrameInstances();
	for (size_t i = 0; i < instances.size(); ++i) {
		const HdTileInstance &instance = instances[i];
		if (instance.image >= manifest->images.size())
			continue;
		const hd_tiles::Image &image = manifest->images[instance.image];
		if (image.page >= manifest->pages.size())
			continue;
		const hd_tiles::Page &page = manifest->pages[image.page];
		HostHdDraw draw {};
		draw.texture = UseTexture(HostTextureKind::TilePage, (static_cast<uint32_t>(std::max(tileset, 0)) << 16) | image.page, page.path, page.width, page.height, false);
		draw.kind = HostHdDrawKind::Tile;
		draw.flags = instance.translucent ? kHdTranslucent : 0;
		draw.maskIndex = static_cast<uint32_t>(i);
		draw.trn = -1;
		draw.srcX = image.u;
		draw.srcY = image.v;
		draw.srcW = image.width;
		draw.srcH = image.height;
		draw.dstX = instance.x;
		draw.dstY = instance.y;
		draw.dstW = instance.width;
		draw.dstH = instance.height;
		draw.clipX = viewport.x;
		draw.clipY = viewport.y;
		draw.clipW = viewport.width;
		draw.clipH = viewport.height;
		draw.light = LightOf(instance.lightTableIndex) * fade;
		draw.opacity = instance.translucent ? 0.5F : 1.0F;
		draw.tint[0] = draw.tint[1] = draw.tint[2] = 1.0F;
		draw.saturation = 1.0F;
		Draws.push_back(draw);
	}
}

void AddSpriteDraws(float fade)
{
	const hd_sprites::SpriteIndex &index = HdSpritesIndex();
	const hd_sprites::OcclusionTracker &occlusion = HdSpritesOcclusion();
	const std::span<const HdSpriteInstance> instances = HdSpritesFrameInstances();
	for (size_t i = 0; i < instances.size(); ++i) {
		if (i < occlusion.count() && !occlusion.visible(i))
			continue;
		const HdSpriteInstance &instance = instances[i];
		if (instance.clip < 0 || static_cast<size_t>(instance.clip) >= index.clips.size())
			continue;
		const hd_sprites::ClipInfo &clip = index.clips[static_cast<size_t>(instance.clip)];
		if (clip.cols <= 0 || clip.frames <= 0)
			continue;
		const int frame = std::clamp(instance.frame, 0, clip.frames - 1);
		HostHdDraw draw {};
		draw.texture = UseTexture(HostTextureKind::SpriteClip, static_cast<uint32_t>(instance.clip), clip.png, clip.sheetWidth(), clip.sheetHeight(), false);
		draw.kind = HostHdDrawKind::Sprite;
		draw.flags = (instance.dropShadow ? kHdDropShadow : 0) | (instance.selfLit ? kHdSelfLit : 0) | (instance.outlineColor >= 0 ? kHdOutline : 0);
		draw.maskIndex = static_cast<uint32_t>(i);
		draw.trn = -1;
		draw.srcX = static_cast<float>((frame % clip.cols) * clip.cellWidth);
		draw.srcY = static_cast<float>((frame / clip.cols) * clip.cellHeight);
		draw.srcW = static_cast<float>(clip.cellWidth);
		draw.srcH = static_cast<float>(clip.cellHeight);
		draw.dstX = instance.x;
		draw.dstY = instance.y;
		draw.dstW = instance.width;
		draw.dstH = instance.height;
		draw.clipX = instance.clipRect.x;
		draw.clipY = instance.clipRect.y;
		draw.clipW = instance.clipRect.width;
		draw.clipH = instance.clipRect.height;
		draw.light = (instance.selfLit ? 1.0F : LightOf(instance.lightTableIndex)) * fade;
		draw.opacity = 1.0F;
		draw.tint[0] = instance.tint.rgb[0];
		draw.tint[1] = instance.tint.rgb[1];
		draw.tint[2] = instance.tint.rgb[2];
		draw.saturation = instance.tint.saturation;
		draw.color = PaletteColor(instance.outlineColor);
		draw.pivotY = instance.pivotY;
		Draws.push_back(draw);
	}
}

void AddUiDraws(float fade)
{
	if (!HdUiActive())
		return;
	const hd_ui::LayerTracker &tracker = HdUiLayers();
	const hd_ui::UiIndex &index = HdUiIndex();
	const std::vector<hd_ui::LayerTracker::Layer> &layers = tracker.layers();
	const std::vector<uint8_t> &drawable = tracker.drawableFlags();
	for (size_t i = 0; i < layers.size(); ++i) {
		if (i >= drawable.size() || drawable[i] == 0)
			continue;
		const hd_ui::Placed &placed = layers[i].placed;
		HostHdDraw draw {};
		draw.maskIndex = static_cast<uint32_t>(i);
		draw.trn = placed.trn;
		draw.dstX = static_cast<float>(placed.rect.x);
		draw.dstY = static_cast<float>(placed.rect.y);
		draw.dstW = static_cast<float>(placed.rect.w);
		draw.dstH = static_cast<float>(placed.rect.h);
		draw.clipX = placed.clip.x;
		draw.clipY = placed.clip.y;
		draw.clipW = placed.clip.w;
		draw.clipH = placed.clip.h;
		draw.light = 1.0F;
		draw.opacity = placed.opacity;
		draw.tint[0] = draw.tint[1] = draw.tint[2] = 1.0F;
		draw.saturation = 1.0F;
		if (placed.isTint()) {
			draw.kind = HostHdDrawKind::UiTint;
			draw.color = placed.tint;
		} else {
			const hd_ui::FrameInfo *frame = HdUiLayerFrame(placed);
			if (frame == nullptr || frame->sheet < 0 || static_cast<size_t>(frame->sheet) >= index.sheets.size())
				continue;
			const hd_ui::SheetInfo &sheet = index.sheets[static_cast<size_t>(frame->sheet)];
			draw.kind = HostHdDrawKind::UiImage;
			draw.texture = UseTexture(HostTextureKind::UiSheet, static_cast<uint32_t>(frame->sheet), sheet.png, sheet.width, sheet.height, sheet.indexed);
			draw.flags = sheet.indexed ? kHdIndexed : 0;
			draw.srcX = static_cast<float>(frame->x);
			draw.srcY = static_cast<float>(frame->y);
			draw.srcW = static_cast<float>(frame->w);
			draw.srcH = static_cast<float>(frame->h);
			// True-colour art follows the palette fade like the classic frame (indexed art goes through the palette).
			draw.light = sheet.indexed ? 1.0F : fade;
		}
		Draws.push_back(draw);
	}
}

} // namespace

// ---- pack (game thread, from InitVideo) ----

void InitHdPack()
{
	// A missing override normally permits discovery. Explicit off must clear
	// both the host's pack state and the core's loose-asset mapping instead.
	HdDir.clear();
	Packs = 0;
	UiIndexPresent = false;
	SetHdAssetDirectory({});
	if (Options.hdDisabled) {
		LogInfo("Host: HD packs explicitly disabled");
		return;
	}
	HdDir = Options.hdDir;
	if (HdDir.empty())
		HdDir = FindHdArtDirectory();
	if (HdDir.empty()) {
		LogInfo("Host: no HD pack");
		return;
	}
	if (HdDir.back() != '/' && HdDir.back() != '\\')
		HdDir += DIRECTORY_SEPARATOR_STR;
	SetHdAssetDirectory(HdDir);
	if (HdArtDirectoryHas2dArt(HdDir))
		Packs |= kPackHd2D;
	if (FileExists(StrCat(HdDir, "3d/free/manifest.json")) || DirectoryExists(StrCat(HdDir, "3d/free/placements").c_str()))
		Packs |= kPackFree3D;
	if (FileExists(StrCat(HdDir, "3d/hd/manifest.json")) || DirectoryExists(StrCat(HdDir, "3d/hd/placements").c_str()))
		Packs |= kPackHd3D;
	for (const char *model : { "3d/warrior/warrior.glb", "3d/rogue/rogue.glb" }) {
		if (FileExists(StrCat(HdDir, model)))
			Packs |= kPackCharacters3D;
	}
	UiIndexPresent = FileExists(StrCat(HdDir, "ui.txt"));
	LogInfo("Host: HD pack {} (2D {}, FREE 3D {}, HD 3D {}, 3D characters {})", HdDir, (Packs & kPackHd2D) != 0,
	    (Packs & kPackFree3D) != 0, (Packs & kPackHd3D) != 0, (Packs & kPackCharacters3D) != 0);
}

uint32_t InstalledPacks()
{
	return Packs;
}

const std::string &HdDirectory()
{
	return HdDir;
}

std::string ResolvePackPath(std::string_view assetPath)
{
	return ResolveHdPath(assetPath);
}

// ---- hooks of hd_*_webgpu.cpp's stubs and placement_runtime.cpp ----

namespace {
bool Hd2DDrawing()
{
	return IsActive() && CurrentRenderMode() == HostRenderMode::Hd2D && (Packs & kPackHd2D) != 0 && HdGraphicsEnabled();
}
} // namespace

bool HdSpritesAvailable()
{
	return Hd2DDrawing();
}

bool HdTilesAvailable()
{
	return Hd2DDrawing();
}

bool HdUiAvailable()
{
	return Hd2DDrawing() && UiIndexPresent;
}

bool HdUiWanted()
{
	return IsActive() && UiIndexPresent;
}

size_t HdTextureBudget()
{
	return size_t { 1 } << 30; // the host owns the textures; this only steers the preload plans
}

bool HdFetchText(const char *url, std::string &text, bool &failed, bool optional)
{
	failed = true;
	if (url == nullptr)
		return false;
	const std::string path = ResolveHdPath(url);
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr) {
		if (!optional)
			LogError("HD text load failed: {}", path);
		return false;
	}
	text.clear();
	char buffer[16384];
	for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
		text.append(buffer, read);
	std::fclose(file);
	failed = false;
	return true;
}

void HdUseTileManifest(int tileset, const hd_tiles::Manifest &manifest)
{
	TileSet = tileset;
	TileManifest = &manifest;
}

// ---- Hd2D frames ----

void OnHdPresent(bool inGame)
{
	FrameResolved = false;
	if (CurrentRenderMode() != HostRenderMode::Hd2D || (Packs & kPackHd2D) == 0)
		return;
	SDL_Surface *surface = PalSurface;
	if (surface == nullptr)
		return;
	// As the WebGPU presenter does right before presenting (dx.cpp RenderPresent).
	int trackedPitch = surface->pitch;
	const uint8_t *tracked = HdLayersTrackedFrame(static_cast<const uint8_t *>(surface->pixels), surface->pitch, surface->w, surface->h, trackedPitch);
	if (inGame) {
		HdTilesFinishFrame(tracked, trackedPitch, surface->w, surface->h);
		HdSpritesFinishFrame(tracked, trackedPitch, surface->w, surface->h);
	}
	HdUiFinishFrame(tracked, trackedPitch, surface->w, surface->h);
	FrameResolved = true;
	++FrameRevision;
}

void ResetHdRecords()
{
	HdSpritesInvalidate();
	HdTilesInvalidate();
	HdUiInvalidate();
	FrameResolved = false;
}

const HostHdFrame &BuildHdSnapshot(bool inGame)
{
	Draws.clear();
	Frame = {};
	Frame.revision = FrameRevision;
	if (FrameResolved) {
		gbuffer::BuildClassicAmbientCurve(std::span<const SDL_Color, 256>(logical_palette.data(), 256), LightTables, AmbientCurve);
		const float fade = PaletteFade();
		if (inGame) {
			AddTileDraws(fade);
			AddSpriteDraws(fade);
			const hd_tiles::DynamicPlane &tiles = HdTilesDynamicPlane();
			Frame.tileMask = MaskOf(tiles.plane(), tiles.width(), tiles.height(), 0, 0, tiles.width(), tiles.height());
			const hd_sprites::OcclusionTracker &sprites = HdSpritesOcclusion();
			const hd_sprites::PixelRect bounds = HdSpritesOcclusionBounds();
			Frame.spriteMask = MaskOf(sprites.plane(), sprites.width(), sprites.height(), bounds.x, bounds.y, bounds.width, bounds.height);
			Frame.worldScale = HdSpritesFrameScale();
		}
		AddUiDraws(fade);
		if (HdUiActive()) {
			const hd_ui::LayerTracker &ui = HdUiLayers();
			const hd_ui::Rect bounds = ui.bounds();
			Frame.uiMask = MaskOf(ui.plane(), ui.width(), ui.height(), bounds.x, bounds.y, bounds.w, bounds.h);
			const hd_ui::TrnPool &trns = HdUiTrns();
			TrnTables.resize(trns.size() * 256);
			for (size_t i = 0; i < trns.size(); ++i)
				std::copy(trns.table(static_cast<int>(i)).begin(), trns.table(static_cast<int>(i)).end(), TrnTables.begin() + static_cast<ptrdiff_t>(i * 256));
			Frame.trnTables = TrnTables.empty() ? nullptr : TrnTables.data();
			Frame.trnCount = static_cast<uint32_t>(trns.size());
		}
	}
	if (Frame.worldScale == 0.0F)
		Frame.worldScale = 1.0F;
	for (size_t i = 0; i < 256; ++i) {
		PaletteRgba[i * 4 + 0] = system_palette[i].r;
		PaletteRgba[i * 4 + 1] = system_palette[i].g;
		PaletteRgba[i * 4 + 2] = system_palette[i].b;
		PaletteRgba[i * 4 + 3] = 255;
	}
	Frame.palette = PaletteRgba.data();
	Frame.textureRevision = TextureRevision;
	Frame.textures = Textures.empty() ? nullptr : Textures.data();
	Frame.textureCount = static_cast<uint32_t>(Textures.size());
	Frame.draws = Draws.empty() ? nullptr : Draws.data();
	Frame.drawCount = static_cast<uint32_t>(Draws.size());
	return Frame;
}

} // namespace devilution::host
