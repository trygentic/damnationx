/**
 * @file hd_ui.cpp
 *
 * Engine side of the opt-in HD UI (hd_ui.hpp): the draw hooks and the per-frame resolve.
 */
#include "engine/render/hd_ui.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/palette.h"
#include "engine/render/hd_graphics.hpp"
#include "engine/render/hd_ui_hooks.hpp"
#include "engine/render/ui_scale.hpp"
#include "engine/surface.hpp"
#include "utils/display.h"
#include "utils/log.hpp"

namespace devilution {

namespace {

using hd_ui::Mask;
using hd_ui::Placed;
using hd_ui::PlacedMasked;
using hd_ui::Rect;

struct HdUiState {
	bool installed = false;
	bool indexLoaded = false;
	bool indexFailed = false;
	hd_ui::UiIndex index;
	hd_ui::SpriteRegistry registry;
	hd_ui::OffscreenStore offscreen;
	hd_ui::LayerTracker layers;
	hd_ui::TrnPool trns;
	/** keyId -> frames of that key in the index (nullptr: no HD art), rebuilt as keys are interned. */
	std::vector<const std::vector<hd_ui::FrameInfo> *> keyFrames;
};

// ponytail: immortal, like clx_normal_map's TheCache(). OnSpritesFreed runs from the destructors of namespace-scope
// Owned* sprites in other translation units, which can be destroyed after this file's globals at exit; a plain
// `HdUiState State;` was then torn down first and SpriteRegistry::forget read freed memory (crash on quit).
HdUiState &State = *new HdUiState;

/**
 * The surface the layers mirror: the back buffer, or the UI layer under a UI scale (ui_scale.hpp; the layers are in
 * UI pixels then and the presenter scales them with the UI).
 */
SDL_Surface *LayerTarget()
{
	SDL_Surface *uiLayer = GetUiLayerSurface();
	return uiLayer != nullptr ? uiLayer : PalSurface;
}

bool IsBackBuffer(const Surface &out)
{
	return out.surface != nullptr && out.surface == LayerTarget();
}

Rect SurfaceBounds(const Surface &out)
{
	return Rect { 0, 0, out.surface->w, out.surface->h };
}

Rect RegionRect(const Surface &out)
{
	return Rect { out.region.x, out.region.y, out.region.w, out.region.h }.intersect(SurfaceBounds(out));
}

const std::vector<hd_ui::FrameInfo> *FramesOf(int keyId)
{
	if (!State.indexLoaded || keyId < 0)
		return nullptr;
	if (State.keyFrames.size() <= static_cast<size_t>(keyId)) {
		const size_t old = State.keyFrames.size();
		State.keyFrames.resize(hd_ui::KeyCount(), nullptr);
		for (size_t id = old; id < State.keyFrames.size(); ++id) {
			const auto it = State.index.frames.find(hd_ui::KeyName(static_cast<int>(id)));
			State.keyFrames[id] = it != State.index.frames.end() ? &it->second : nullptr;
		}
	}
	return State.keyFrames[keyId];
}

const hd_ui::FrameInfo *FrameOf(int keyId, int frame)
{
	const std::vector<hd_ui::FrameInfo> *frames = FramesOf(keyId);
	if (frames == nullptr || frame < 0 || static_cast<size_t>(frame) >= frames->size())
		return nullptr;
	const hd_ui::FrameInfo &info = (*frames)[frame];
	return info.sheet >= 0 ? &info : nullptr;
}

/**
 * Sends one placed image to the back-buffer layers or the off-screen surface it was drawn into.
 * `replayed`: the image comes out of a composite or off-screen surface together with the images below it; without HD
 * art it is still recorded (never drawn) so that it hides those images where its classic pixels are.
 */
void Emit(const Surface &out, PlacedMasked entry, bool replayed = false)
{
	if (IsBackBuffer(out)) {
		if (!State.indexLoaded || !HdGraphicsEnabled())
			return;
		if (!replayed && !entry.placed.isTint() && FrameOf(entry.placed.keyId, entry.placed.frame) == nullptr)
			return; // no HD art: the classic draw is all there is
		const SDL_Surface *target = LayerTarget();
		State.layers.record(static_cast<const uint8_t *>(target->pixels), target->pitch, std::move(entry));
		return;
	}
	if (out.surface == PalSurface)
		return; // the world text under a UI scale (floating numbers, automap) stays classic
	State.offscreen.add(out.surface, std::move(entry));
}

// ---- hooks ----

void OnSpritesLoaded(const char *path, const uint8_t *data, size_t size, uint16_t numLists)
{
	if (path == nullptr || data == nullptr || size == 0)
		return;
	const std::string key = hd_ui::NormalizeAssetKey(path);
	if (!hd_ui::IsUiAssetKey(key))
		return;
	const auto registerList = [&](ClxSpriteList list, const std::string &listKey) {
		const int keyId = hd_ui::InternKey(listKey);
		for (uint32_t i = 0; i < list.numSprites(); ++i) {
			const ClxSprite sprite = list[i];
			State.registry.registerFrame(sprite.frameData(), hd_ui::FingerprintOf(sprite.frameData(), sprite.frameSize()), keyId,
			    static_cast<int>(i));
		}
	};
	if (numLists == 0) {
		registerList(ClxSpriteList { data }, key);
		return;
	}
	const ClxSpriteSheet sheet { data, numLists };
	for (uint16_t i = 0; i < numLists; ++i)
		registerList(sheet[i], key + "#" + std::to_string(i));
}

void OnSpritesFreed(const void *data, size_t size)
{
	State.registry.forget(data, size);
}

void OnClxDrawn(const Surface &out, int x, int y, const uint8_t *frameData, uint32_t frameSize, const uint8_t *trn, bool blended)
{
	if (out.surface == nullptr)
		return;
	// HD off: back-buffer draws record nothing (skip the fingerprint); off-screen surfaces are still followed, since
	// panel buffers and composites made now are shown after HD comes back on.
	if (!HdGraphicsEnabled() && IsBackBuffer(out))
		return;
	const hd_ui::SpriteFingerprint fingerprint = hd_ui::FingerprintOf(frameData, frameSize);
	const hd_ui::SpriteRegistry::Entry *entry = State.registry.find(frameData, fingerprint);
	if (entry == nullptr)
		return;
	const int width = fingerprint.width;
	const int height = fingerprint.height;
	const Rect rect { out.region.x + x, out.region.y + y - height + 1, width, height };
	const Rect clip = rect.intersect(RegionRect(out));
	if (clip.empty())
		return;
	const uint16_t header = static_cast<uint16_t>(frameData[0] | (frameData[1] << 8));
	Mask mask = hd_ui::ClxOpaqueMask(frameData + header, frameSize - header, width, height, rect.x, rect.y, clip);
	if (entry->keyId >= 0) {
		PlacedMasked placed;
		placed.placed.keyId = entry->keyId;
		placed.placed.frame = entry->frame;
		placed.placed.rect = rect;
		placed.placed.clip = clip;
		placed.placed.trn = trn != nullptr ? State.trns.intern(trn) : -1;
		placed.placed.opacity = blended ? 0.5F : 1.0F;
		placed.mask = std::move(mask);
		Emit(out, std::move(placed));
		return;
	}
	// A composite (SurfaceToClx): replay its HD content at this position, cut to what this draw wrote.
	for (const PlacedMasked &child : entry->children) {
		PlacedMasked placed;
		placed.placed = child.placed.translated(rect.x, rect.y);
		placed.placed.clip = placed.placed.clip.intersect(clip);
		if (placed.placed.clip.empty())
			continue;
		if (blended)
			placed.placed.opacity *= 0.5F;
		placed.mask = child.mask.translated(rect.x, rect.y).clipped(placed.placed.clip).intersected(mask);
		Emit(out, std::move(placed), /*replayed=*/true);
	}
}

void OnBlitted(const Surface &dst, const Surface &src, int srcX, int srcY, int width, int height, int dstX, int dstY, bool skipZero)
{
	if (src.surface == nullptr || dst.surface == nullptr || src.surface == dst.surface || width <= 0 || height <= 0)
		return;
	const Rect srcRect { src.region.x + srcX, src.region.y + srcY, width, height };
	const int dx = dst.region.x + dstX - srcRect.x;
	const int dy = dst.region.y + dstY - srcRect.y;
	const Rect dstRect = srcRect.translated(dx, dy);
	std::vector<PlacedMasked> entries = State.offscreen.collect(src.surface, srcRect, dx, dy);
	if (entries.empty())
		return;
	// The pixels this blit wrote: all of them (the collected masks lie inside dstRect already), or the non-zero source
	// pixels.
	if (skipZero) {
		Mask written = Mask::Full(dstRect);
		const auto *pixels = static_cast<const uint8_t *>(src.surface->pixels);
		for (int y = 0; y < height; ++y) {
			const uint8_t *row = pixels + static_cast<ptrdiff_t>(srcRect.y + y) * src.surface->pitch + srcRect.x;
			uint8_t *bits = &written.bits[static_cast<size_t>(y) * width];
			for (int x = 0; x < width; ++x)
				bits[x] = row[x] != 0 ? 1 : 0;
		}
		for (PlacedMasked &entry : entries)
			entry.mask = std::move(entry.mask).intersected(written);
	}
	for (PlacedMasked &entry : entries)
		Emit(dst, std::move(entry), /*replayed=*/true);
}

void OnSurfaceToClx(const Surface &surface, unsigned numFrames, const uint8_t *data, size_t size)
{
	if (surface.surface == nullptr || numFrames == 0 || !State.offscreen.has(surface.surface) || size == 0)
		return;
	const ClxSpriteList list { data };
	const int frameHeight = surface.h() / static_cast<int>(numFrames);
	for (unsigned f = 0; f < numFrames && f < list.numSprites(); ++f) {
		const Rect strip { surface.region.x, surface.region.y + static_cast<int>(f) * frameHeight, surface.w(), frameHeight };
		std::vector<PlacedMasked> children = State.offscreen.collect(surface.surface, strip, -strip.x, -strip.y);
		if (children.empty())
			continue;
		const ClxSprite sprite = list[f];
		State.registry.registerComposite(sprite.frameData(), hd_ui::FingerprintOf(sprite.frameData(), sprite.frameSize()), std::move(children));
	}
}

void OnSurfaceCreated(const SDL_Surface *surface)
{
	State.offscreen.reset(surface);
}

void OnSurfaceDownscaled(const Surface &src, const Surface &dst)
{
	if (src.surface == nullptr || dst.surface == nullptr || !State.offscreen.has(src.surface))
		return;
	const Rect srcRect = RegionRect(src);
	std::vector<PlacedMasked> entries = State.offscreen.collect(src.surface, srcRect, -srcRect.x, -srcRect.y);
	const auto half = [](const Rect &r, int ox, int oy) {
		const int x0 = r.x / 2;
		const int y0 = r.y / 2;
		return Rect { ox + x0, oy + y0, (r.right() + 1) / 2 - x0, (r.bottom() + 1) / 2 - y0 };
	};
	for (PlacedMasked &entry : entries) {
		PlacedMasked scaled;
		scaled.placed = entry.placed;
		scaled.placed.rect = half(entry.placed.rect, dst.region.x, dst.region.y);
		scaled.placed.clip = half(entry.placed.clip, dst.region.x, dst.region.y);
		// A half-size pixel is written when any of its 2x2 source pixels was.
		scaled.mask.rect = scaled.placed.clip;
		scaled.mask.bits.assign(static_cast<size_t>(scaled.mask.rect.w) * scaled.mask.rect.h, 0);
		for (int y = 0; y < scaled.mask.rect.h; ++y) {
			for (int x = 0; x < scaled.mask.rect.w; ++x) {
				const int sx = (scaled.mask.rect.x - dst.region.x + x) * 2;
				const int sy = (scaled.mask.rect.y - dst.region.y + y) * 2;
				const bool any = entry.mask.at(sx, sy) || entry.mask.at(sx + 1, sy) || entry.mask.at(sx, sy + 1) || entry.mask.at(sx + 1, sy + 1);
				scaled.mask.bits[static_cast<size_t>(y) * scaled.mask.rect.w + x] = any ? 1 : 0;
			}
		}
		State.offscreen.add(dst.surface, std::move(scaled));
	}
}

void OnSlotTinted(const Surface &out, int x, int y, int width, int height, uint8_t ramp)
{
	if (!IsBackBuffer(out) || !State.indexLoaded || !HdGraphicsEnabled())
		return;
	const Rect rect = Rect { out.region.x + x, out.region.y + y, width, height }.intersect(RegionRect(out));
	if (rect.empty())
		return;
	// The classic recolour has already happened: it wrote the pixels that now hold the target ramp.
	Mask mask = Mask::Full(rect);
	const SDL_Surface *target = LayerTarget();
	const auto *pixels = static_cast<const uint8_t *>(target->pixels);
	// The tint is the mean classic result, nearly opaque: the classic slot is almost black with a faint cast and the
	// HD stone must end up as dark, whatever the ramp.
	uint32_t sum[3] = { 0, 0, 0 };
	uint32_t count = 0;
	for (int yy = 0; yy < rect.h; ++yy) {
		const uint8_t *row = pixels + static_cast<ptrdiff_t>(rect.y + yy) * target->pitch + rect.x;
		for (int xx = 0; xx < rect.w; ++xx) {
			const bool tinted = row[xx] >= ramp && row[xx] < ramp + 16;
			mask.bits[static_cast<size_t>(yy) * rect.w + xx] = tinted ? 1 : 0;
			if (!tinted)
				continue;
			const SDL_Color &c = logical_palette[row[xx]];
			sum[0] += c.r;
			sum[1] += c.g;
			sum[2] += c.b;
			++count;
		}
	}
	if (count == 0)
		return;
	const SDL_Color colour { static_cast<uint8_t>(sum[0] / count), static_cast<uint8_t>(sum[1] / count), static_cast<uint8_t>(sum[2] / count), 255 };
	PlacedMasked placed;
	placed.placed.keyId = -1;
	placed.placed.rect = rect;
	placed.placed.clip = rect;
	placed.placed.tint = static_cast<uint32_t>(colour.r) | (static_cast<uint32_t>(colour.g) << 8) | (static_cast<uint32_t>(colour.b) << 16) | (220U << 24);
	placed.mask = std::move(mask);
	Emit(out, std::move(placed));
}

constexpr HdUiHookTable Hooks {
	.spritesLoaded = OnSpritesLoaded,
	.spritesFreed = OnSpritesFreed,
	.clxDrawn = OnClxDrawn,
	.blitted = OnBlitted,
	.surfaceToClx = OnSurfaceToClx,
	.surfaceCreated = OnSurfaceCreated,
	.surfaceDownscaled = OnSurfaceDownscaled,
	.slotTinted = OnSlotTinted,
};

void PollIndex()
{
	if (State.indexLoaded || State.indexFailed)
		return;
	std::string text;
	bool failed = false;
	if (HdUiGpuFetchIndex(text, failed)) {
		State.index = hd_ui::ParseUiIndex(text);
		for (const std::string &error : State.index.errors)
			LogError("hd/ui.txt: {}", error);
		State.indexLoaded = true;
		State.keyFrames.clear();
		LogInfo("HD UI: {} sheets, {} assets", State.index.sheets.size(), State.index.frames.size());
	} else if (failed) {
		State.indexFailed = true;
		LogError("HD UI: hd/ui.txt could not be loaded; the UI stays classic");
	}
}

} // namespace

void HdUiInstall()
{
	InitHdGraphicsOption(); // the launch flags are known now: list Graphics > HD Graphics when any HD subsystem is on
	if (State.installed || !HdUiGpuWanted())
		return;
	State.installed = true;
	HdUiHooks = &Hooks;
	PollIndex();
}

bool HdUiActive()
{
	// The hooks stay installed while Graphics > HD Graphics is off (hd_graphics.hpp): UI art loads once, so they must
	// see every load to draw it after HD comes back on. Nothing is recorded or drawn meanwhile.
	return State.installed && HdGraphicsEnabled();
}

void HdUiInvalidateRect(int x, int y, int width, int height)
{
	if (State.installed)
		State.layers.invalidate(Rect { x, y, width, height });
}

void HdUiInvalidate()
{
	if (State.installed)
		State.layers.clear();
}

void HdUiFinishFrame(const uint8_t *pixels, int pitch, int width, int height)
{
	if (!HdUiActive())
		return;
	PollIndex();
	if (const SDL_Surface *uiLayer = GetUiLayerSurface(); uiLayer != nullptr) {
		// Under a UI scale the layers mirror the UI layer, which still holds this frame's UI.
		pixels = static_cast<const uint8_t *>(uiLayer->pixels);
		pitch = uiLayer->pitch;
		width = uiLayer->w;
		height = uiLayer->h;
	}
	if (State.layers.width() != width || State.layers.height() != height)
		State.layers.reset(width, height);
	State.layers.resolve(pixels, pitch, [](const hd_ui::LayerTracker::Layer &layer) {
		if (layer.placed.isTint())
			return true;
		const hd_ui::FrameInfo *frame = FrameOf(layer.placed.keyId, layer.placed.frame);
		return frame != nullptr && HdUiGpuSheetReady(frame->sheet);
	});
}

const hd_ui::LayerTracker &HdUiLayers()
{
	return State.layers;
}

const hd_ui::UiIndex &HdUiIndex()
{
	return State.index;
}

const hd_ui::TrnPool &HdUiTrns()
{
	return State.trns;
}

const hd_ui::FrameInfo *HdUiLayerFrame(const hd_ui::Placed &placed)
{
	return placed.isTint() ? nullptr : FrameOf(placed.keyId, placed.frame);
}

std::vector<hd_ui::IndexedSheetFrame> HdUiClassicFramesOfSheet(int sheet)
{
	std::vector<hd_ui::IndexedSheetFrame> frames;
	State.registry.forEach([&](const void *frameData, const hd_ui::SpriteRegistry::Entry &entry) {
		const hd_ui::FrameInfo *info = FrameOf(entry.keyId, entry.frame);
		if (info == nullptr || info->sheet != sheet)
			return;
		const auto *data = static_cast<const uint8_t *>(frameData);
		const uint16_t header = static_cast<uint16_t>(data[0] | (data[1] << 8));
		if (header >= entry.fingerprint.size)
			return;
		frames.push_back(hd_ui::IndexedSheetFrame { *info, data + header, entry.fingerprint.size - header, entry.fingerprint.width, entry.fingerprint.height });
	});
	return frames;
}

} // namespace devilution
