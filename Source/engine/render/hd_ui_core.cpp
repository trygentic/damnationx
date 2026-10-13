#include "engine/render/hd_ui_core.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace devilution::hd_ui {

namespace {

std::vector<std::string_view> SplitFields(std::string_view line)
{
	std::vector<std::string_view> fields;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
			++i;
		const size_t start = i;
		while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
			++i;
		if (i > start)
			fields.push_back(line.substr(start, i - start));
	}
	return fields;
}

bool ParseInt(std::string_view text, int &value)
{
	const char *end = text.data() + text.size();
	const auto result = std::from_chars(text.data(), end, value);
	return result.ec == std::errc() && result.ptr == end;
}

struct KeyTable {
	std::vector<std::string> names;
	std::unordered_map<std::string, int> ids;
};

KeyTable &Keys()
{
	static KeyTable table;
	return table;
}

} // namespace

Rect Rect::intersect(const Rect &o) const
{
	const int x0 = std::max(x, o.x);
	const int y0 = std::max(y, o.y);
	const int x1 = std::min(right(), o.right());
	const int y1 = std::min(bottom(), o.bottom());
	if (x1 <= x0 || y1 <= y0)
		return { x0, y0, 0, 0 };
	return { x0, y0, x1 - x0, y1 - y0 };
}

Rect Union(const Rect &a, const Rect &b)
{
	if (a.empty())
		return b;
	if (b.empty())
		return a;
	const int x0 = std::min(a.x, b.x);
	const int y0 = std::min(a.y, b.y);
	return { x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0 };
}

// ---- index ----

const FrameInfo *UiIndex::find(std::string_view key, int frame) const
{
	const auto it = frames.find(std::string(key));
	if (it == frames.end() || frame < 0 || static_cast<size_t>(frame) >= it->second.size())
		return nullptr;
	const FrameInfo &info = it->second[frame];
	return info.sheet >= 0 ? &info : nullptr;
}

bool UiIndex::hasKey(std::string_view key) const
{
	return frames.contains(std::string(key));
}

UiIndex ParseUiIndex(std::string_view text)
{
	UiIndex index;
	if (text.substr(0, 3) == "\xEF\xBB\xBF")
		text.remove_prefix(3);
	int lineNumber = 0;
	while (!text.empty()) {
		const size_t newline = text.find('\n');
		std::string_view line = text.substr(0, newline);
		text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);
		++lineNumber;
		if (const size_t hash = line.find('#'); hash != std::string_view::npos && (hash == 0 || line[hash - 1] == ' ' || line[hash - 1] == '\t'))
			line = line.substr(0, hash);
		const std::vector<std::string_view> f = SplitFields(line);
		if (f.empty())
			continue;
		const auto error = [&](std::string_view what) {
			index.errors.push_back("line " + std::to_string(lineNumber) + ": " + std::string(what));
		};
		if (f[0] == "scale") {
			int scale = 0;
			if (f.size() != 2 || !ParseInt(f[1], scale) || scale < 1 || scale > 8)
				error("bad scale");
			else
				index.scale = scale;
		} else if (f[0] == "sheet") {
			int id = 0;
			int width = 0;
			int height = 0;
			if ((f.size() != 5 && f.size() != 6) || !ParseInt(f[1], id) || !ParseInt(f[3], width) || !ParseInt(f[4], height)
			    || width < 1 || height < 1 || width > 8192 || height > 8192 || (f.size() == 6 && f[5] != "indexed")) {
				error("bad sheet");
				continue;
			}
			if (id != static_cast<int>(index.sheets.size())) {
				error("sheet ids must be contiguous from 0");
				continue;
			}
			index.sheets.push_back(SheetInfo { std::string(f[2]), width, height, f.size() == 6 });
		} else if (f[0] == "frame") {
			int frame = 0;
			FrameInfo info;
			if (f.size() != 8 || !ParseInt(f[2], frame) || !ParseInt(f[3], info.sheet) || !ParseInt(f[4], info.x)
			    || !ParseInt(f[5], info.y) || !ParseInt(f[6], info.w) || !ParseInt(f[7], info.h) || frame < 0 || frame > 65535) {
				error("bad frame");
				continue;
			}
			if (info.sheet < 0 || static_cast<size_t>(info.sheet) >= index.sheets.size()) {
				error("frame refers to an unknown sheet");
				continue;
			}
			const SheetInfo &sheet = index.sheets[info.sheet];
			if (info.w < 1 || info.h < 1 || info.x < 0 || info.y < 0 || info.x + info.w > sheet.width || info.y + info.h > sheet.height) {
				error("frame rectangle outside its sheet");
				continue;
			}
			std::vector<FrameInfo> &frames = index.frames[NormalizeAssetKey(f[1])];
			if (frames.size() <= static_cast<size_t>(frame))
				frames.resize(static_cast<size_t>(frame) + 1);
			frames[frame] = info;
		} else {
			error("unknown directive");
		}
	}
	return index;
}

std::string NormalizeAssetKey(std::string_view path)
{
	std::string key;
	key.reserve(path.size());
	for (const char c : path) {
		if (c == '\\')
			key.push_back('/');
		else if (c >= 'A' && c <= 'Z')
			key.push_back(static_cast<char>(c - 'A' + 'a'));
		else
			key.push_back(c);
	}
	// Strip a file extension of the last path component (".cel", ".clx", ".pcx"), keeping a "#list" suffix.
	const size_t list = key.find('#');
	std::string suffix;
	if (list != std::string::npos) {
		suffix = key.substr(list);
		key.resize(list);
	}
	const size_t slash = key.rfind('/');
	const size_t dot = key.rfind('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		key.resize(dot);
	return key + suffix;
}

bool IsUiAssetKey(std::string_view key)
{
	// The DiabloUI selector pentagram (ui_art/focus, focus16, focus42) takes its red from the menu screen's palette,
	// not from its own PCX palette, so true-colour HD art baked from the PCX comes out grey and gold. Keep it classic.
	if (key.substr(0, 12) == "ui_art/focus")
		return false;
	constexpr std::string_view Prefixes[] = { "ctrlpan/", "data/", "ui_art/", "fonts/", "items/duricons", "gendata/" };
	for (const std::string_view prefix : Prefixes) {
		if (key.substr(0, prefix.size()) == prefix)
			return true;
	}
	return false;
}

int InternKey(std::string_view key)
{
	KeyTable &table = Keys();
	const auto it = table.ids.find(std::string(key));
	if (it != table.ids.end())
		return it->second;
	const int id = static_cast<int>(table.names.size());
	table.names.emplace_back(key);
	table.ids.emplace(std::string(key), id);
	return id;
}

const std::string &KeyName(int keyId)
{
	static const std::string Empty;
	const KeyTable &table = Keys();
	if (keyId < 0 || static_cast<size_t>(keyId) >= table.names.size())
		return Empty;
	return table.names[keyId];
}

size_t KeyCount()
{
	return Keys().names.size();
}

// ---- masks ----

namespace {

/** True when any of the `count` bytes at `p` is non-zero. */
bool AnyNonZero(const uint8_t *p, int count)
{
	int i = 0;
	for (; i + 8 <= count; i += 8) {
		uint64_t word;
		std::memcpy(&word, p + i, 8);
		if (word != 0)
			return true;
	}
	for (; i < count; ++i) {
		if (p[i] != 0)
			return true;
	}
	return false;
}

/** Clears every pixel of `mask` outside `keep` (mask coordinates). */
void ClearOutside(Mask &mask, const Rect &keep)
{
	const Rect &r = mask.rect;
	const Rect inside = r.intersect(keep);
	for (int y = 0; y < r.h; ++y) {
		uint8_t *row = &mask.bits[static_cast<size_t>(y) * r.w];
		const int sy = r.y + y;
		if (inside.empty() || sy < inside.y || sy >= inside.bottom()) {
			std::fill_n(row, r.w, uint8_t { 0 });
			continue;
		}
		std::fill_n(row, inside.x - r.x, uint8_t { 0 });
		std::fill_n(row + (inside.right() - r.x), r.right() - inside.right(), uint8_t { 0 });
	}
}

} // namespace

size_t Mask::count() const
{
	return static_cast<size_t>(std::count_if(bits.begin(), bits.end(), [](uint8_t b) { return b != 0; }));
}

bool Mask::covers(const Mask &other) const
{
	const Rect &o = other.rect;
	for (int y = 0; y < o.h; ++y) {
		const uint8_t *row = &other.bits[static_cast<size_t>(y) * o.w];
		const int sy = o.y + y;
		if (sy < rect.y || sy >= rect.bottom()) {
			if (AnyNonZero(row, o.w))
				return false;
			continue;
		}
		// Columns of `other` left and right of this mask must be empty; inside, every set pixel must be set here.
		const int x0 = std::clamp(rect.x - o.x, 0, o.w);
		const int x1 = std::clamp(rect.right() - o.x, x0, o.w);
		if (AnyNonZero(row, x0) || AnyNonZero(row + x1, o.w - x1))
			return false;
		if (x1 == x0)
			continue;
		const uint8_t *own = &bits[static_cast<size_t>(sy - rect.y) * rect.w + (o.x + x0 - rect.x)];
		uint8_t missing = 0;
		for (int x = x0; x < x1; ++x)
			missing |= static_cast<uint8_t>((row[x] != 0) & (own[x - x0] == 0));
		if (missing != 0)
			return false;
	}
	return true;
}

Mask Mask::Full(const Rect &rect)
{
	Mask mask;
	mask.rect = rect.empty() ? Rect { rect.x, rect.y, 0, 0 } : rect;
	mask.bits.assign(static_cast<size_t>(std::max(mask.rect.w, 0)) * std::max(mask.rect.h, 0), 1);
	return mask;
}

Mask Mask::clipped(const Rect &clip) const &
{
	Mask out;
	out.rect = rect.intersect(clip);
	if (out.rect == rect) {
		out.bits = bits;
		return out;
	}
	out.bits.resize(static_cast<size_t>(out.rect.w) * out.rect.h);
	for (int y = 0; y < out.rect.h; ++y) {
		std::memcpy(&out.bits[static_cast<size_t>(y) * out.rect.w],
		    &bits[static_cast<size_t>(out.rect.y + y - rect.y) * rect.w + (out.rect.x - rect.x)], static_cast<size_t>(out.rect.w));
	}
	return out;
}

Mask Mask::clipped(const Rect &clip) &&
{
	const Rect cut = rect.intersect(clip);
	if (cut != rect) {
		// Rows move towards the front only (the cut starts at or after the old origin and is no wider): memmove in order.
		for (int y = 0; y < cut.h; ++y) {
			std::memmove(&bits[static_cast<size_t>(y) * cut.w], &bits[static_cast<size_t>(cut.y + y - rect.y) * rect.w + (cut.x - rect.x)],
			    static_cast<size_t>(cut.w));
		}
		bits.resize(static_cast<size_t>(cut.w) * cut.h);
		rect = cut;
	}
	return std::move(*this);
}

Mask Mask::translated(int dx, int dy) const &
{
	Mask out = *this;
	out.rect = rect.translated(dx, dy);
	return out;
}

Mask Mask::translated(int dx, int dy) &&
{
	rect = rect.translated(dx, dy);
	return std::move(*this);
}

Mask Mask::intersected(const Mask &other) const &
{
	Mask out = *this;
	return std::move(out).intersected(other);
}

Mask Mask::intersected(const Mask &other) &&
{
	const Rect overlap = rect.intersect(other.rect);
	ClearOutside(*this, overlap);
	for (int y = 0; y < overlap.h; ++y) {
		uint8_t *row = &bits[static_cast<size_t>(overlap.y + y - rect.y) * rect.w + (overlap.x - rect.x)];
		const uint8_t *with = &other.bits[static_cast<size_t>(overlap.y + y - other.rect.y) * other.rect.w + (overlap.x - other.rect.x)];
		for (int x = 0; x < overlap.w; ++x)
			row[x] = with[x] != 0 ? row[x] : 0;
	}
	return std::move(*this);
}

Mask ClxOpaqueMask(const uint8_t *pixelData, size_t pixelDataSize, int width, int height, int left, int top, const Rect &clip)
{
	Mask mask;
	mask.rect = Rect { left, top, width, height }.intersect(clip);
	mask.bits.assign(static_cast<size_t>(mask.rect.w) * mask.rect.h, 0);
	if (mask.rect.empty() || width <= 0 || height <= 0)
		return mask;
	// Rows are stored bottom-up; runs may continue on the next row.
	int x = 0;
	int row = height - 1; // frame row (top-down) being decoded
	const auto mark = [&](int count) {
		while (count > 0 && row >= 0) {
			const int run = std::min(count, width - x);
			const int sy = top + row;
			if (sy >= mask.rect.y && sy < mask.rect.bottom()) {
				const int x0 = std::max(left + x, mask.rect.x);
				const int x1 = std::min(left + x + run, mask.rect.right());
				if (x1 > x0)
					std::fill_n(&mask.bits[static_cast<size_t>(sy - mask.rect.y) * mask.rect.w + (x0 - mask.rect.x)], x1 - x0, uint8_t { 1 });
			}
			x += run;
			count -= run;
			if (x >= width) {
				x = 0;
				--row;
			}
		}
	};
	const auto skip = [&](int count) {
		x += count;
		while (x >= width && row >= 0) {
			x -= width;
			--row;
		}
	};
	size_t pos = 0;
	while (pos < pixelDataSize && row >= 0) {
		const uint8_t control = pixelData[pos++];
		if (control < 0x80) {
			skip(control);
		} else if (control <= 0xBE) {
			mark(0xBF - control);
			++pos; // fill colour
		} else {
			const int count = 256 - control;
			mark(count);
			pos += static_cast<size_t>(count);
		}
	}
	return mask;
}

// ---- indexed sheets ----

namespace {

/** Palette index of every pixel of a CLX frame (top-down rows, -1 = transparent). */
void DecodeClxIndices(const uint8_t *pixelData, size_t pixelDataSize, int width, int height, std::vector<int16_t> &out)
{
	out.assign(static_cast<size_t>(width) * height, -1);
	int x = 0;
	int row = height - 1;
	const auto advance = [&]() {
		if (++x >= width) {
			x = 0;
			--row;
		}
	};
	size_t pos = 0;
	while (pos < pixelDataSize && row >= 0) {
		const uint8_t control = pixelData[pos++];
		if (control < 0x80) {
			x += control;
			while (x >= width && row >= 0) {
				x -= width;
				--row;
			}
		} else if (control <= 0xBE) {
			if (pos >= pixelDataSize)
				break;
			const uint8_t colour = pixelData[pos++];
			for (int i = 0; i < 0xBF - control && row >= 0; ++i) {
				out[static_cast<size_t>(row) * width + x] = colour;
				advance();
			}
		} else {
			const int count = 256 - control;
			for (int i = 0; i < count && row >= 0 && pos < pixelDataSize; ++i) {
				out[static_cast<size_t>(row) * width + x] = pixelData[pos++];
				advance();
			}
		}
	}
}

} // namespace

IndexedSheetReport CheckIndexedSheet(const uint8_t *texels, size_t pitch, int width, int height, int scale, std::span<const IndexedSheetFrame> frames)
{
	IndexedSheetReport report;
	for (int y = 0; y < height; ++y) {
		const uint8_t *row = texels + static_cast<size_t>(y) * pitch;
		for (int x = 0; x < width; ++x) {
			const uint8_t alpha = row[static_cast<size_t>(x) * 4 + 3];
			report.covered += alpha == 255 ? 1 : 0;
			report.badTexels += alpha != 0 && alpha != 255 ? 1 : 0;
		}
	}
	std::vector<int16_t> indices;
	for (const IndexedSheetFrame &frame : frames) {
		const FrameInfo &hd = frame.hd;
		if (frame.width <= 0 || frame.height <= 0 || hd.w != frame.width * scale || hd.h != frame.height * scale || hd.x < 0 || hd.y < 0
		    || hd.x + hd.w > width || hd.y + hd.h > height)
			continue;
		DecodeClxIndices(frame.pixelData, frame.pixelDataSize, frame.width, frame.height, indices);
		for (int cy = 0; cy < frame.height; ++cy) {
			for (int cx = 0; cx < frame.width; ++cx) {
				const int16_t index = indices[static_cast<size_t>(cy) * frame.width + cx];
				if (index < 0)
					continue;
				const uint8_t *texel = texels + static_cast<size_t>(hd.y + cy * scale + scale / 2) * pitch + static_cast<size_t>(hd.x + cx * scale + scale / 2) * 4;
				++report.compared;
				report.matched += texel[3] == 255 && texel[0] == index ? 1 : 0;
			}
		}
	}
	report.ok = report.badTexels == 0 && report.covered > 0 && report.matched * 10 >= report.compared * 9;
	return report;
}

// ---- TRN pool ----

int TrnPool::intern(const uint8_t *trn)
{
	for (size_t i = 0; i < tables_.size(); ++i) {
		if (std::memcmp(tables_[i].data(), trn, 256) == 0)
			return static_cast<int>(i);
	}
	if (tables_.size() >= Capacity)
		return -1;
	std::array<uint8_t, 256> table;
	std::memcpy(table.data(), trn, 256);
	tables_.push_back(table);
	++generation_;
	return static_cast<int>(tables_.size() - 1);
}

void TrnPool::clear()
{
	tables_.clear();
	++generation_;
}

// ---- registry ----

SpriteFingerprint FingerprintOf(const uint8_t *frameData, uint32_t frameSize)
{
	SpriteFingerprint fp;
	if (frameData == nullptr || frameSize < 6)
		return fp;
	fp.width = static_cast<uint16_t>(frameData[2] | (frameData[3] << 8));
	fp.height = static_cast<uint16_t>(frameData[4] | (frameData[5] << 8));
	fp.size = frameSize;
	const uint16_t header = static_cast<uint16_t>(frameData[0] | (frameData[1] << 8));
	for (uint32_t i = 0; i < 4 && header + i < frameSize; ++i)
		fp.head |= static_cast<uint32_t>(frameData[header + i]) << (8 * i);
	return fp;
}

void SpriteRegistry::registerFrame(const void *frameData, SpriteFingerprint fingerprint, int keyId, int frame)
{
	entries_[frameData] = Entry { fingerprint, keyId, frame, {} };
}

void SpriteRegistry::registerComposite(const void *frameData, SpriteFingerprint fingerprint, std::vector<PlacedMasked> children)
{
	entries_[frameData] = Entry { fingerprint, -1, 0, std::move(children) };
}

const SpriteRegistry::Entry *SpriteRegistry::find(const void *frameData, SpriteFingerprint fingerprint) const
{
	const auto it = entries_.find(frameData);
	if (it == entries_.end() || !(it->second.fingerprint == fingerprint))
		return nullptr;
	return &it->second;
}

void SpriteRegistry::forget(const void *begin, size_t size)
{
	if (entries_.empty())
		return;
	const auto *first = static_cast<const uint8_t *>(begin);
	const uint8_t *last = first + size;
	std::erase_if(entries_, [&](const auto &item) {
		const auto *p = static_cast<const uint8_t *>(item.first);
		return p >= first && p < last;
	});
}

// ---- off-screen ----

void OffscreenStore::reset(const void *surface)
{
	surfaces_.erase(surface);
}

void OffscreenStore::add(const void *surface, PlacedMasked entry)
{
	if (entry.placed.clip.empty())
		return;
	std::vector<PlacedMasked> &entries = surfaces_[surface];
	std::erase_if(entries, [&](const PlacedMasked &old) {
		return entry.placed.clip.contains(old.placed.clip) && entry.mask.covers(old.mask);
	});
	if (entries.size() >= MaxEntriesPerSurface)
		entries.erase(entries.begin());
	entries.push_back(std::move(entry));
}

std::vector<PlacedMasked> OffscreenStore::collect(const void *surface, const Rect &srcRect, int dx, int dy) const
{
	std::vector<PlacedMasked> out;
	const auto it = surfaces_.find(surface);
	if (it == surfaces_.end())
		return out;
	for (const PlacedMasked &entry : it->second) {
		const Rect clip = entry.placed.clip.intersect(srcRect);
		if (clip.empty())
			continue;
		PlacedMasked moved;
		moved.placed = entry.placed;
		moved.placed.clip = clip;
		moved.placed = moved.placed.translated(dx, dy);
		moved.mask = entry.mask.clipped(clip).translated(dx, dy);
		out.push_back(std::move(moved));
	}
	return out;
}

bool OffscreenStore::has(const void *surface) const
{
	const auto it = surfaces_.find(surface);
	return it != surfaces_.end() && !it->second.empty();
}

size_t OffscreenStore::size(const void *surface) const
{
	const auto it = surfaces_.find(surface);
	return it == surfaces_.end() ? 0 : it->second.size();
}

// ---- layers ----

void LayerTracker::reset(int width, int height)
{
	layers_.clear();
	drawable_.clear();
	width_ = width;
	height_ = height;
	const size_t count = static_cast<size_t>(std::max(width, 0)) * std::max(height, 0);
	plane_.assign(count, 0);
	planeRects_.clear();
	bounds_ = {};
}

void LayerTracker::clear()
{
	layers_.clear();
	drawable_.clear();
	bounds_ = {};
}

bool LayerTracker::record(const uint8_t *pixels, int pitch, PlacedMasked entry)
{
	const Rect buffer { 0, 0, width_, height_ };
	Layer layer;
	layer.placed = entry.placed;
	layer.placed.clip = entry.placed.clip.intersect(buffer).intersect(entry.placed.rect);
	if (layer.placed.clip.empty())
		return false;
	layer.mask = std::move(entry.mask).clipped(layer.placed.clip);
	if (layer.mask.rect != layer.placed.clip) {
		// Re-base the mask on the full clip rectangle (pixels outside the original mask were not written).
		Mask full;
		full.rect = layer.placed.clip;
		full.bits.assign(static_cast<size_t>(full.rect.w) * full.rect.h, 0);
		const Rect &part = layer.mask.rect; // inside the clip rectangle
		for (int y = 0; y < part.h; ++y) {
			std::memcpy(&full.bits[static_cast<size_t>(part.y + y - full.rect.y) * full.rect.w + (part.x - full.rect.x)],
			    &layer.mask.bits[static_cast<size_t>(y) * part.w], static_cast<size_t>(part.w));
		}
		layer.mask = std::move(full);
	}
	const Rect &clip = layer.placed.clip;
	layer.snapshot.resize(static_cast<size_t>(clip.w) * clip.h);
	for (int y = 0; y < clip.h; ++y)
		std::memcpy(&layer.snapshot[static_cast<size_t>(y) * clip.w], pixels + static_cast<ptrdiff_t>(clip.y + y) * pitch + clip.x, clip.w);

	std::erase_if(layers_, [&](const Layer &old) {
		return clip.contains(old.placed.clip) && layer.mask.covers(old.mask);
	});
	if (layers_.size() >= MaxLayers)
		layers_.erase(layers_.begin());
	layers_.push_back(std::move(layer));
	return true;
}

void LayerTracker::invalidate(const Rect &rect)
{
	std::erase_if(layers_, [&](const Layer &layer) { return layer.placed.clip.intersects(rect); });
}

void LayerTracker::resolve(const uint8_t *pixels, int pitch, const std::function<bool(const Layer &)> &drawable)
{
	const Rect buffer { 0, 0, width_, height_ };
	bounds_ = {};
	for (const Layer &layer : layers_)
		bounds_ = Union(bounds_, layer.placed.clip.intersect(buffer));
	drawable_.assign(layers_.size(), 0);
	if (bounds_.empty()) {
		layers_.clear();
		drawable_.clear();
		return;
	}
	// A pixel's plane value comes from the last layer that wrote it (mask bit) or finds it changed since its snapshot:
	// that layer + 1 when the pixel changed after it or the GPU cannot draw it (a layer the GPU cannot draw hides what
	// lies below it like a classic draw would), else 0 (that layer's HD texel is on top). Pixels no layer touches are 0.
	// Only the layer rectangles of this and the last resolve can hold non-zero values (the layers are often spread over
	// the screen: their union is several times larger than the rectangles).
	const auto zero = [&](const Rect &rect) {
		for (int y = rect.y; y < rect.bottom(); ++y)
			std::fill_n(&plane_[static_cast<size_t>(y) * width_ + rect.x], rect.w, uint16_t { 0 });
	};
	// The plane is 0 outside the last rectangles, so clearing those clears all of it.
	for (const Rect &rect : planeRects_)
		zero(rect);
	planeRects_.clear();
	for (const Layer &layer : layers_) {
		const Rect clip = layer.placed.clip.intersect(buffer);
		if (!clip.empty())
			planeRects_.push_back(clip);
	}
	for (size_t i = 0; i < layers_.size(); ++i) {
		const Layer &layer = layers_[i];
		drawable_[i] = drawable(layer) ? 1 : 0;
		const Rect &clip = layer.placed.clip;
		const auto hidden = static_cast<uint16_t>(std::min<size_t>(i, std::numeric_limits<int16_t>::max()) + 1);
		// Value of a written pixel that did not change since the snapshot.
		const uint16_t writtenValue = drawable_[i] != 0 ? 0 : hidden;
		for (int y = 0; y < clip.h; ++y) {
			const uint8_t *row = pixels + static_cast<ptrdiff_t>(clip.y + y) * pitch + clip.x;
			const uint8_t *snap = &layer.snapshot[static_cast<size_t>(y) * clip.w];
			const uint8_t *bits = &layer.mask.bits[static_cast<size_t>(y) * clip.w];
			uint16_t *plane = &plane_[static_cast<size_t>(clip.y + y) * width_ + clip.x];
			int x = 0;
			// Eight pixels at a time: unchanged and unwritten (the common case outside the mask), or unchanged and all written.
			for (; x + 8 <= clip.w; x += 8) {
				uint64_t rowWord;
				uint64_t snapWord;
				uint64_t bitsWord;
				std::memcpy(&rowWord, row + x, 8);
				std::memcpy(&snapWord, snap + x, 8);
				std::memcpy(&bitsWord, bits + x, 8);
				if (rowWord == snapWord) {
					if (bitsWord == 0)
						continue;
					if (bitsWord == 0x0101010101010101ULL) {
						std::fill_n(plane + x, 8, writtenValue);
						continue;
					}
				}
				for (int k = x; k < x + 8; ++k) {
					if (row[k] != snap[k])
						plane[k] = hidden;
					else if (bits[k] != 0)
						plane[k] = writtenValue;
				}
			}
			for (; x < clip.w; ++x) {
				if (row[x] != snap[x])
					plane[x] = hidden;
				else if (bits[x] != 0)
					plane[x] = writtenValue;
			}
		}
	}
	// Drop layers that are (nearly) gone from the final frame.
	std::vector<uint8_t> keep(layers_.size(), 1);
	for (size_t i = 0; i < layers_.size(); ++i) {
		const Layer &layer = layers_[i];
		const Rect &clip = layer.placed.clip;
		// An undrawable layer marks its own pixels (it shows classic there): it is shown up to and including itself.
		// Plane values stay below MaxLayers + 1, so a limit past that is "every value".
		const auto limit = static_cast<uint16_t>(std::min<size_t>(i + (drawable_[i] == 0 ? 1 : 0), 0xFFFF));
		size_t written = 0;
		size_t visible = 0;
		for (int y = 0; y < clip.h; ++y) {
			const uint8_t *bits = &layer.mask.bits[static_cast<size_t>(y) * clip.w];
			const uint16_t *plane = &plane_[static_cast<size_t>(clip.y + y) * width_ + clip.x];
			// Branch-free so that it vectorises (mask bytes are 0 or 1).
			uint32_t rowWritten = 0;
			uint32_t rowVisible = 0;
			for (int x = 0; x < clip.w; ++x) {
				const uint32_t bit = bits[x];
				rowWritten += bit;
				rowVisible += bit & static_cast<uint32_t>(plane[x] <= limit);
			}
			written += rowWritten;
			visible += rowVisible;
		}
		if (written == 0 || static_cast<float>(visible) < MinVisibleShare * static_cast<float>(written))
			keep[i] = 0;
	}
	// Compact and renumber the plane: value v (first visible old index) -> number of kept layers below v.
	std::vector<uint16_t> remap(layers_.size() + 1, 0);
	uint16_t kept = 0;
	for (size_t i = 0; i < layers_.size(); ++i) {
		remap[i] = kept;
		if (keep[i] != 0)
			++kept;
	}
	remap[layers_.size()] = kept;
	if (kept != layers_.size()) {
		std::vector<Layer> compact;
		std::vector<uint8_t> flags;
		compact.reserve(kept);
		for (size_t i = 0; i < layers_.size(); ++i) {
			if (keep[i] != 0) {
				compact.push_back(std::move(layers_[i]));
				flags.push_back(drawable_[i]);
			}
		}
		layers_ = std::move(compact);
		drawable_ = std::move(flags);
		// Outside the layer rectangles the plane is 0 (remap[0] = 0). The rectangles overlap, so a renumbered pixel is
		// tagged with RemappedBit (plane values stay below MaxLayers + 1) until every rectangle is done.
		constexpr uint16_t RemappedBit = 0x8000;
		static_assert(MaxLayers + 1 < RemappedBit);
		for (const Rect &rect : planeRects_) {
			for (int y = rect.y; y < rect.bottom(); ++y) {
				uint16_t *plane = &plane_[static_cast<size_t>(y) * width_];
				for (int x = rect.x; x < rect.right(); ++x) {
					if ((plane[x] & RemappedBit) == 0)
						plane[x] = remap[std::min<size_t>(plane[x], remap.size() - 1)] | RemappedBit;
				}
			}
		}
		for (const Rect &rect : planeRects_) {
			for (int y = rect.y; y < rect.bottom(); ++y) {
				uint16_t *plane = &plane_[static_cast<size_t>(y) * width_];
				for (int x = rect.x; x < rect.right(); ++x)
					plane[x] &= static_cast<uint16_t>(~RemappedBit);
			}
		}
	}
}

// ---- TRN LUT ----

std::vector<uint8_t> BuildNearestGrid(std::span<const uint8_t, 768> palette)
{
	constexpr int N = TrnLutSize;
	std::vector<uint8_t> nearest(static_cast<size_t>(N) * N * N);
	std::array<int, 768> pal;
	for (size_t i = 0; i < 768; ++i)
		pal[i] = palette[i];
	for (int b = 0; b < N; ++b) {
		const int cb = b * 255 / (N - 1);
		for (int g = 0; g < N; ++g) {
			const int cg = g * 255 / (N - 1);
			for (int r = 0; r < N; ++r) {
				const int cr = r * 255 / (N - 1);
				int best = 0;
				int bestDistance = std::numeric_limits<int>::max();
				for (int i = 0; i < 256; ++i) {
					const int dg = pal[i * 3 + 1] - cg;
					int distance = 4 * dg * dg;
					if (distance >= bestDistance)
						continue;
					const int dr = pal[i * 3] - cr;
					distance += 3 * dr * dr;
					if (distance >= bestDistance)
						continue;
					const int db = pal[i * 3 + 2] - cb;
					distance += 2 * db * db;
					if (distance < bestDistance) {
						bestDistance = distance;
						best = i;
					}
				}
				nearest[(static_cast<size_t>(b) * N + g) * N + r] = static_cast<uint8_t>(best);
			}
		}
	}
	return nearest;
}

std::vector<uint8_t> BuildTrnLut(std::span<const uint8_t, 256> trn, std::span<const uint8_t, 768> palette, std::span<const uint8_t> nearest)
{
	constexpr int N = TrnLutSize;
	std::vector<uint8_t> lut(static_cast<size_t>(N) * N * N * 4);
	for (int b = 0; b < N; ++b) {
		for (int g = 0; g < N; ++g) {
			for (int r = 0; r < N; ++r) {
				const size_t cell = (static_cast<size_t>(b) * N + g) * N + r;
				const int c[3] = { r * 255 / (N - 1), g * 255 / (N - 1), b * 255 / (N - 1) };
				const int from = nearest[cell];
				const int to = trn[from];
				uint8_t *out = &lut[cell * 4];
				for (int k = 0; k < 3; ++k)
					out[k] = static_cast<uint8_t>(std::clamp(c[k] + palette[to * 3 + k] - palette[from * 3 + k], 0, 255));
				out[3] = 255;
			}
		}
	}
	return lut;
}

std::vector<uint8_t> BuildTrnLut(std::span<const uint8_t, 256> trn, std::span<const uint8_t, 768> palette)
{
	const std::vector<uint8_t> nearest = BuildNearestGrid(palette);
	return BuildTrnLut(trn, palette, nearest);
}

int SelectUiTextureDivisor(float canvasPixelsPerUiPixel)
{
	if (canvasPixelsPerUiPixel > 2.0F)
		return 1;
	if (canvasPixelsPerUiPixel > 1.0F)
		return 2;
	return 4;
}

} // namespace devilution::hd_ui
