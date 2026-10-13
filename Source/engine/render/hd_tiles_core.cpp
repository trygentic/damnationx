#include "engine/render/hd_tiles_core.hpp"
#include "engine/render/hd_view_transform.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace devilution::hd_tiles {

namespace {

std::string_view Trim(std::string_view text)
{
	constexpr std::string_view Whitespace = " \t\r\n\v\f";
	const size_t begin = text.find_first_not_of(Whitespace);
	if (begin == std::string_view::npos)
		return {};
	const size_t end = text.find_last_not_of(Whitespace);
	return text.substr(begin, end - begin + 1);
}

std::vector<std::string_view> SplitFields(std::string_view line)
{
	constexpr std::string_view Whitespace = " \t\r\v\f";
	std::vector<std::string_view> fields;
	size_t position = 0;
	while (true) {
		position = line.find_first_not_of(Whitespace, position);
		if (position == std::string_view::npos)
			break;
		const size_t end = std::min(line.find_first_of(Whitespace, position), line.size());
		fields.push_back(line.substr(position, end - position));
		position = end;
	}
	return fields;
}

std::optional<int> ParseInt(std::string_view field)
{
	int value = 0;
	const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
	if (ec != std::errc() || ptr != field.data() + field.size())
		return std::nullopt;
	return value;
}

std::string ResolveAtlasPath(std::string_view file)
{
	std::string path(file);
	std::replace(path.begin(), path.end(), '/', '\\');
	if (!path.starts_with("hd\\"))
		path.insert(0, "hd\\");
	return path;
}

enum class KeyKind : uint8_t {
	Generic,
	Positional,
	Special,
	PositionalSpecial,
};

/** Sort key of a manifest entry: kind, then the lookup key. */
using EntryKey = std::pair<KeyKind, uint64_t>;

struct PendingImage {
	size_t lineNumber;
	Image image;
};

} // namespace

void Manifest::buildGrid()
{
	grid.clear();
	gridEntries.clear();
	gridWidth = 0;
	gridHeight = 0;
	int maxX = -1;
	int maxY = -1;
	for (const auto *map : { &positional, &positionalSpecials }) {
		for (const auto &[key, parts] : *map) {
			maxX = std::max(maxX, static_cast<int>((key >> 48) & 0xFFFF));
			maxY = std::max(maxY, static_cast<int>((key >> 32) & 0xFFFF));
		}
	}
	if (maxX < 0 || maxX >= MaxGridSize || maxY >= MaxGridSize)
		return; // nothing positional, or a position too far out for a dense grid: the hash maps answer
	gridWidth = maxX + 1;
	gridHeight = maxY + 1;
	grid.assign(static_cast<size_t>(gridWidth) * gridHeight * GridSlots, 0);
	gridEntries.reserve(positional.size() + positionalSpecials.size());
	const auto add = [&](uint64_t key, int slot, const Parts &parts) {
		const auto x = static_cast<size_t>((key >> 48) & 0xFFFF);
		const auto y = static_cast<size_t>((key >> 32) & 0xFFFF);
		uint32_t &head = grid[(y * gridWidth + x) * GridSlots + slot];
		gridEntries.push_back(GridEntry { .value = static_cast<uint32_t>((key >> 8) & 0xFFFF), .next = head, .parts = parts });
		head = static_cast<uint32_t>(gridEntries.size());
	};
	for (const auto &[key, parts] : positional)
		add(key, static_cast<int>(key & 0xFF) == static_cast<int>(Layer::Floor) ? 0 : 1, parts);
	for (const auto &[key, parts] : positionalSpecials)
		add(key, SpecialSlot, parts);
}

const Parts *Manifest::findInGrid(int x, int y, int slot, int value) const
{
	if (static_cast<unsigned>(x) >= static_cast<unsigned>(gridWidth) || static_cast<unsigned>(y) >= static_cast<unsigned>(gridHeight))
		return nullptr;
	// The maps key the value as 16 bits (PositionKey).
	const auto wanted = static_cast<uint32_t>(static_cast<uint16_t>(value));
	for (uint32_t entry = grid[(static_cast<size_t>(y) * gridWidth + x) * GridSlots + slot]; entry != 0;) {
		const GridEntry &candidate = gridEntries[entry - 1];
		if (candidate.value == wanted)
			return &candidate.parts;
		entry = candidate.next;
	}
	return nullptr;
}

Parts Manifest::findPiece(int x, int y, int piece, Layer layer) const
{
	if (gridWidth > 0) {
		if (const Parts *parts = findInGrid(x, y, layer == Layer::Floor ? 0 : 1, piece); parts != nullptr)
			return *parts;
	} else if (!positional.empty()) {
		const auto it = positional.find(PositionKey(x, y, piece, static_cast<int>(layer)));
		if (it != positional.end())
			return it->second;
	}
	const std::vector<Parts> &generic = layer == Layer::Floor ? genericFloor : genericCell;
	return piece >= 0 && static_cast<size_t>(piece) < generic.size() ? generic[piece] : Parts {};
}

Parts Manifest::findSpecial(int x, int y, int frame) const
{
	if (gridWidth > 0) {
		if (const Parts *parts = findInGrid(x, y, SpecialSlot, frame); parts != nullptr)
			return *parts;
	} else if (!positionalSpecials.empty()) {
		const auto it = positionalSpecials.find(PositionKey(x, y, frame, 0));
		if (it != positionalSpecials.end())
			return it->second;
	}
	return frame >= 0 && static_cast<size_t>(frame) < specials.size() ? specials[frame] : Parts {};
}

int Manifest::maxPageWidth() const
{
	int width = 0;
	for (const Page &page : pages)
		width = std::max(width, page.width);
	return width;
}

int Manifest::maxPageHeight() const
{
	int height = 0;
	for (const Page &page : pages)
		height = std::max(height, page.height);
	return height;
}

std::expected<Manifest, std::string> ParseManifest(std::string_view text, std::string_view name)
{
	const auto lineError = [&](size_t lineNumber, std::string_view message) {
		return std::unexpected(std::string(name) + " line " + std::to_string(lineNumber) + ": " + std::string(message));
	};

	Manifest manifest;
	std::vector<std::optional<Page>> pages;
	// Entries grouped by key in first-seen order of their parts (a key may repeat: one part per line).
	std::map<EntryKey, std::vector<PendingImage>> entries;
	std::vector<EntryKey> keyOrder;

	size_t lineNumber = 0;
	while (!text.empty()) {
		++lineNumber;
		const size_t newline = text.find('\n');
		std::string_view line = text.substr(0, newline);
		text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);
		if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF"))
			line.remove_prefix(3);
		line = Trim(line.substr(0, line.find('#')));
		if (line.empty())
			continue;

		const std::vector<std::string_view> fields = SplitFields(line);
		const std::string_view directive = fields[0];
		if (directive == "scale") {
			if (fields.size() != 2)
				return lineError(lineNumber, "expected 'scale <S>'");
			const std::optional<int> scale = ParseInt(fields[1]);
			if (!scale || *scale < 1 || *scale > MaxScale)
				return lineError(lineNumber, "scale must be 1..4");
			if (manifest.scale != 0)
				return lineError(lineNumber, "scale is defined twice");
			manifest.scale = *scale;
			continue;
		}
		if (directive == "atlas") {
			if (fields.size() != 5)
				return lineError(lineNumber, "expected 'atlas <N> <file> <width> <height>'");
			const std::optional<int> index = ParseInt(fields[1]);
			const std::optional<int> width = ParseInt(fields[3]);
			const std::optional<int> height = ParseInt(fields[4]);
			if (!index || !width || !height || *index < 0)
				return lineError(lineNumber, "atlas index, width and height must be integers");
			if (*index >= MaxPages)
				return lineError(lineNumber, "atlas index must be below " + std::to_string(MaxPages));
			if (*width < 1 || *width > MaxPageSize || *height < 1 || *height > MaxPageSize)
				return lineError(lineNumber, "atlas width and height must be 1..4096");
			if (static_cast<size_t>(*index) >= pages.size())
				pages.resize(static_cast<size_t>(*index) + 1);
			if (pages[*index].has_value())
				return lineError(lineNumber, "atlas " + std::to_string(*index) + " is defined twice");
			pages[*index] = Page { ResolveAtlasPath(fields[2]), *width, *height };
			continue;
		}

		KeyKind kind;
		size_t keyFields; // fields before the image rectangle
		if (directive == "piece") {
			kind = KeyKind::Generic;
			keyFields = 3;
		} else if (directive == "at") {
			kind = KeyKind::Positional;
			keyFields = 5;
		} else if (directive == "special") {
			kind = KeyKind::Special;
			keyFields = 2;
		} else if (directive == "at_special") {
			kind = KeyKind::PositionalSpecial;
			keyFields = 4;
		} else {
			return lineError(lineNumber, "unknown directive '" + std::string(directive) + "'");
		}
		if (fields.size() != keyFields + 7)
			return lineError(lineNumber, "expected " + std::to_string(keyFields + 7) + " fields for '" + std::string(directive) + "'");

		int layer = 0;
		std::vector<int> numbers;
		for (size_t i = 1; i < fields.size(); ++i) {
			const bool isLayerField = (kind == KeyKind::Generic && i == 2) || (kind == KeyKind::Positional && i == 4);
			if (isLayerField) {
				if (fields[i] == "floor")
					layer = static_cast<int>(Layer::Floor);
				else if (fields[i] == "cell")
					layer = static_cast<int>(Layer::Cell);
				else
					return lineError(lineNumber, "layer must be 'floor' or 'cell'");
				continue;
			}
			const std::optional<int> value = ParseInt(fields[i]);
			if (!value)
				return lineError(lineNumber, "'" + std::string(fields[i]) + "' is not an integer");
			numbers.push_back(*value);
		}
		// numbers: key values (without the layer), then page u v w h dx dy
		const size_t keyCount = numbers.size() - 7;
		for (size_t i = 0; i < keyCount; ++i) {
			if (numbers[i] < 0 || numbers[i] > 0xFFFF)
				return lineError(lineNumber, "key values must be 0..65535");
		}
		const int *rect = &numbers[keyCount];
		if (rect[0] < 0 || rect[0] >= MaxPages)
			return lineError(lineNumber, "page out of range");
		if (rect[1] < 0 || rect[2] < 0 || rect[3] < 1 || rect[4] < 1 || rect[1] + rect[3] > MaxPageSize || rect[2] + rect[4] > MaxPageSize)
			return lineError(lineNumber, "image rectangle must be non-empty and inside 4096x4096");
		if (rect[5] < -32768 || rect[5] > 32767 || rect[6] < -32768 || rect[6] > 32767)
			return lineError(lineNumber, "offset out of range");

		uint64_t key = 0;
		switch (kind) {
		case KeyKind::Generic:
			key = Manifest::PositionKey(0, 0, numbers[0], layer);
			break;
		case KeyKind::Positional:
			key = Manifest::PositionKey(numbers[0], numbers[1], numbers[2], layer);
			break;
		case KeyKind::Special:
			key = Manifest::PositionKey(0, 0, numbers[0], 0);
			break;
		case KeyKind::PositionalSpecial:
			key = Manifest::PositionKey(numbers[0], numbers[1], numbers[2], 0);
			break;
		}
		const EntryKey entryKey { kind, key };
		auto [it, inserted] = entries.try_emplace(entryKey);
		if (inserted)
			keyOrder.push_back(entryKey);
		it->second.push_back(PendingImage { lineNumber, Image {
		                                                   .page = static_cast<uint8_t>(rect[0]),
		                                                   .u = static_cast<uint16_t>(rect[1]),
		                                                   .v = static_cast<uint16_t>(rect[2]),
		                                                   .width = static_cast<uint16_t>(rect[3]),
		                                                   .height = static_cast<uint16_t>(rect[4]),
		                                                   .dx = static_cast<int16_t>(rect[5]),
		                                                   .dy = static_cast<int16_t>(rect[6]),
		                                               } });
	}

	if (manifest.scale == 0)
		return std::unexpected(std::string(name) + " defines no scale");
	if (pages.empty())
		return std::unexpected(std::string(name) + " defines no atlas pages");
	for (size_t i = 0; i < pages.size(); ++i) {
		if (!pages[i].has_value())
			return std::unexpected(std::string(name) + " is missing atlas page " + std::to_string(i));
		manifest.pages.push_back(*pages[i]);
	}

	for (const EntryKey &entryKey : keyOrder) {
		const std::vector<PendingImage> &parts = entries[entryKey];
		for (const PendingImage &pending : parts) {
			const Image &image = pending.image;
			if (image.page >= manifest.pages.size())
				return lineError(pending.lineNumber, "page " + std::to_string(image.page) + " is not defined");
			const Page &page = manifest.pages[image.page];
			if (image.u + image.width > page.width || image.v + image.height > page.height)
				return lineError(pending.lineNumber, "image rectangle is outside page " + std::to_string(image.page));
		}
		const Parts span { static_cast<uint32_t>(manifest.images.size()), static_cast<uint32_t>(parts.size()) };
		for (const PendingImage &pending : parts)
			manifest.images.push_back(pending.image);

		const uint64_t key = entryKey.second;
		const auto value = static_cast<size_t>((key >> 8) & 0xFFFF);
		switch (entryKey.first) {
		case KeyKind::Generic: {
			std::vector<Parts> &table = (key & 0xFF) == static_cast<uint64_t>(Layer::Floor) ? manifest.genericFloor : manifest.genericCell;
			if (value >= table.size())
				table.resize(value + 1);
			table[value] = span;
		} break;
		case KeyKind::Positional:
			manifest.positional[key] = span;
			break;
		case KeyKind::Special:
			if (value >= manifest.specials.size())
				manifest.specials.resize(value + 1);
			manifest.specials[value] = span;
			break;
		case KeyKind::PositionalSpecial:
			manifest.positionalSpecials[key] = span;
			break;
		}
	}
	manifest.buildGrid();
	return manifest;
}

// ---- DynamicPlane ----

void DynamicPlane::reset(int width, int height)
{
	width = std::max(width, 0);
	height = std::max(height, 0);
	if (width == width_ && height == height_ && rowSpans_.size() == static_cast<size_t>(height_)) {
		// Only the row spans can hold non-zero values.
		for (int y = 0; y < height_; ++y) {
			const RowSpan span = rowSpans_[y];
			std::fill(plane_.data() + static_cast<size_t>(y) * width_ + span.first, plane_.data() + static_cast<size_t>(y) * width_ + span.end, uint16_t { 0 });
		}
	} else {
		width_ = width;
		height_ = height;
		plane_.assign(static_cast<size_t>(width_) * height_, 0);
	}
	rowSpans_.assign(static_cast<size_t>(height_), RowSpan {});
	sectionOpen_ = false;
	frameSnapshot_ = false;
	finished_ = false;
}

PixelRect DynamicPlane::clip(PixelRect rect) const
{
	const int left = std::max(rect.x, 0);
	const int top = std::max(rect.y, 0);
	const int right = std::min(rect.x + rect.width, width_);
	const int bottom = std::min(rect.y + rect.height, height_);
	return { left, top, std::max(right - left, 0), std::max(bottom - top, 0) };
}

void DynamicPlane::copyRect(const uint8_t *pixels, int pitch, PixelRect rect, std::vector<uint8_t> &out)
{
	const size_t size = static_cast<size_t>(rect.width) * rect.height;
	if (out.size() < size)
		out.resize(size); // never shrinks: the snapshot buffers are reused every section and frame
	const uint8_t *row = pixels + static_cast<ptrdiff_t>(rect.y) * pitch + rect.x;
	uint8_t *dst = out.data();
	for (int y = 0; y < rect.height; ++y, row += pitch, dst += rect.width)
		std::memcpy(dst, row, static_cast<size_t>(rect.width));
}

namespace {

/** First x in [begin, end) where `a` and `b` differ, `end` when they are equal (eight bytes at a time). */
int FirstDifference(const uint8_t *a, const uint8_t *b, int begin, int end)
{
	int x = begin;
	// Most rows are unchanged: 32 bytes per step with a single branch, then words, then bytes.
	for (; x + 32 <= end; x += 32) {
		std::array<uint64_t, 4> wordsA;
		std::array<uint64_t, 4> wordsB;
		std::memcpy(wordsA.data(), a + x, 32);
		std::memcpy(wordsB.data(), b + x, 32);
		if (((wordsA[0] ^ wordsB[0]) | (wordsA[1] ^ wordsB[1]) | (wordsA[2] ^ wordsB[2]) | (wordsA[3] ^ wordsB[3])) != 0)
			break;
	}
	for (; x + 8 <= end; x += 8) {
		uint64_t wordA;
		uint64_t wordB;
		std::memcpy(&wordA, a + x, 8);
		std::memcpy(&wordB, b + x, 8);
		if (wordA != wordB)
			break;
	}
	while (x < end && a[x] == b[x])
		++x;
	return x;
}

/** One past the last x in [begin, end) where `a` and `b` differ, `begin` when they are equal. */
int LastDifferenceEnd(const uint8_t *a, const uint8_t *b, int begin, int end)
{
	int x = end;
	for (; x - 8 >= begin; x -= 8) {
		uint64_t wordA;
		uint64_t wordB;
		std::memcpy(&wordA, a + x - 8, 8);
		std::memcpy(&wordB, b + x - 8, 8);
		if (wordA != wordB)
			break;
	}
	while (x > begin && a[x - 1] == b[x - 1])
		--x;
	return x;
}

} // namespace

void DynamicPlane::markChanged(const uint8_t *pixels, int pitch, PixelRect rect, const std::vector<uint8_t> &snapshot, uint16_t value, bool keepUndo)
{
	for (int y = 0; y < rect.height; ++y) {
		const uint8_t *row = pixels + static_cast<ptrdiff_t>(rect.y + y) * pitch + rect.x;
		const uint8_t *saved = snapshot.data() + static_cast<size_t>(y) * rect.width;
		// Most rows are unchanged, or change in a short span (a character, the cursor): find it with word compares.
		const int first = FirstDifference(row, saved, 0, rect.width);
		if (first == rect.width)
			continue;
		const int end = LastDifferenceEnd(row, saved, first, rect.width);
		const size_t offset = static_cast<size_t>(rect.y + y) * width_ + rect.x;
		uint16_t *dst = plane_.data() + offset;
		RowSpan &span = rowSpans_[rect.y + y];
		span = span.empty() ? RowSpan { rect.x + first, rect.x + end } : RowSpan { std::min(span.first, rect.x + first), std::max(span.end, rect.x + end) };
		if (keepUndo) {
			undoSpans_.push_back({ offset + first, static_cast<uint32_t>(end - first), undoValues_.size() });
			undoValues_.insert(undoValues_.end(), dst + first, dst + end);
		}
		const auto mark = [&](int from, int to) {
			for (int x = from; x < to; ++x) {
				if (row[x] != saved[x] && dst[x] < value)
					dst[x] = value;
			}
		};
		int x = first;
		for (; x + 8 <= end; x += 8) {
			uint64_t rowWord;
			uint64_t savedWord;
			std::memcpy(&rowWord, row + x, 8);
			std::memcpy(&savedWord, saved + x, 8);
			if (rowWord != savedWord)
				mark(x, x + 8);
		}
		mark(x, end);
	}
}

void DynamicPlane::beginSection(const uint8_t *pixels, int pitch, PixelRect rect, uint16_t instanceCount)
{
	if (sectionOpen_)
		endSection(pixels, pitch);
	sectionRect_ = clip(rect);
	sectionValue_ = instanceCount;
	if (sectionRect_.empty() || pixels == nullptr)
		return;
	copyRect(pixels, pitch, sectionRect_, section_);
	sectionOpen_ = true;
}

void DynamicPlane::endSection(const uint8_t *pixels, int pitch)
{
	if (!sectionOpen_)
		return;
	sectionOpen_ = false;
	// No tile was recorded before the section: nothing to hide.
	if (sectionValue_ == 0 || pixels == nullptr)
		return;
	markChanged(pixels, pitch, sectionRect_, section_, sectionValue_, /*keepUndo=*/false);
}

void DynamicPlane::zoomInPlace(int viewportWidth, int offsetX, int height, int zoomQuarters)
{
	sectionOpen_ = false;
	height = std::min(height, height_);
	if (offsetX < 0 || height <= 0)
		return;
	viewportWidth = std::clamp(viewportWidth, 0, std::max(width_ - offsetX, 0));
	HdUpscaleInPlace(plane_.data(), static_cast<size_t>(width_), ComputeHdWorldTransform(zoomQuarters, offsetX + viewportWidth, height, viewportWidth, offsetX));
	for (int y = 0; y < height; ++y) {
		uint16_t *row = plane_.data() + static_cast<size_t>(y) * width_;
		std::fill(row, row + std::min(offsetX, width_), uint16_t { 0 });
		std::fill(row + std::min(offsetX + viewportWidth, width_), row + width_, uint16_t { 0 });
		rowSpans_[y] = viewportWidth > 0 ? RowSpan { std::min(offsetX, width_), std::min(offsetX + viewportWidth, width_) } : RowSpan {};
	}
}

void DynamicPlane::snapshotFrame(const uint8_t *pixels, int pitch, PixelRect rect)
{
	frameRect_ = clip(rect);
	frameSnapshot_ = !frameRect_.empty() && pixels != nullptr;
	finished_ = false;
	if (frameSnapshot_)
		copyRect(pixels, pitch, frameRect_, frame_);
}

bool DynamicPlane::finishFrame(const uint8_t *pixels, int pitch)
{
	if (!frameSnapshot_ || pixels == nullptr)
		return false;
	// Every present re-derives the UI part from the world part: a re-present (cursor moved) must not keep old marks, so
	// it first restores the plane values the previous finish overwrote.
	if (finished_) {
		for (const UndoSpan &span : undoSpans_)
			std::copy_n(undoValues_.data() + span.valuesOffset, span.length, plane_.data() + span.planeOffset);
	}
	finished_ = true;
	undoSpans_.clear();
	undoValues_.clear();
	markChanged(pixels, pitch, frameRect_, frame_, HiddenAll, /*keepUndo=*/true);
	return true;
}

void RecolourPass::reset()
{
	begun_ = false;
	active_ = false;
}

void RecolourPass::begin(const uint8_t *pixels, int pitch, int width, int height, PixelRect rect)
{
	const int left = std::max(rect.x, 0);
	const int top = std::max(rect.y, 0);
	const int right = std::min(rect.x + rect.width, width);
	const int bottom = std::min(rect.y + rect.height, height);
	const PixelRect clipped { left, top, std::max(right - left, 0), std::max(bottom - top, 0) };
	if (pixels == nullptr || clipped.empty()) {
		begun_ = false;
		return;
	}
	const bool chain = active_ && clipped.x == rect_.x && clipped.y == rect_.y && clipped.width == rect_.width && clipped.height == rect_.height;
	rect_ = clipped;
	const size_t size = static_cast<size_t>(rect_.width) * rect_.height;
	if (!chain)
		before_.resize(size);
	for (int y = 0; y < rect_.height; ++y) {
		const uint8_t *row = pixels + static_cast<ptrdiff_t>(rect_.y + y) * pitch + rect_.x;
		uint8_t *saved = before_.data() + static_cast<size_t>(y) * rect_.width;
		if (!chain) {
			std::copy_n(row, rect_.width, saved);
			continue;
		}
		// A second recolour: what the trackers would see now (the first pass undone where nothing was drawn since).
		const uint8_t *afterFirst = after_.data() + static_cast<size_t>(y) * rect_.width;
		for (int x = 0; x < rect_.width; ++x) {
			if (row[x] != afterFirst[x])
				saved[x] = row[x];
		}
	}
	active_ = false;
	begun_ = true;
}

void RecolourPass::end(const uint8_t *pixels, int pitch)
{
	if (!begun_ || pixels == nullptr)
		return;
	after_.resize(static_cast<size_t>(rect_.width) * rect_.height);
	for (int y = 0; y < rect_.height; ++y)
		std::copy_n(pixels + static_cast<ptrdiff_t>(rect_.y + y) * pitch + rect_.x, rect_.width, after_.data() + static_cast<size_t>(y) * rect_.width);
	begun_ = false;
	active_ = true;
}

const uint8_t *RecolourPass::trackedFrame(const uint8_t *pixels, int pitch, int width, int height, int &outPitch)
{
	outPitch = pitch;
	if (!active_ || pixels == nullptr || rect_.x + rect_.width > width || rect_.y + rect_.height > height)
		return pixels;
	composed_.resize(static_cast<size_t>(width) * height);
	for (int y = 0; y < height; ++y)
		std::copy_n(pixels + static_cast<ptrdiff_t>(y) * pitch, width, composed_.data() + static_cast<size_t>(y) * width);
	for (int y = 0; y < rect_.height; ++y) {
		uint8_t *row = composed_.data() + static_cast<size_t>(rect_.y + y) * width + rect_.x;
		const uint8_t *before = before_.data() + static_cast<size_t>(y) * rect_.width;
		const uint8_t *after = after_.data() + static_cast<size_t>(y) * rect_.width;
		for (int x = 0; x < rect_.width; ++x) {
			if (row[x] == after[x])
				row[x] = before[x];
		}
	}
	outPitch = width;
	return composed_.data();
}

float LinearLuminance(const std::array<float, 3> &rgb)
{
	return 0.2126F * rgb[0] + 0.7152F * rgb[1] + 0.0722F * rgb[2];
}

std::array<float, 3> ClassicTint(std::span<const std::array<float, 3>> classicLinear)
{
	if (classicLinear.empty())
		return { 1.0F, 1.0F, 1.0F };
	std::array<float, 3> mean {};
	for (const std::array<float, 3> &c : classicLinear) {
		for (size_t i = 0; i < 3; ++i)
			mean[i] += c[i];
	}
	for (float &v : mean)
		v /= static_cast<float>(classicLinear.size());
	const float luminance = LinearLuminance(mean);
	if (luminance < ClassicTintMinLuminance)
		return { 1.0F, 1.0F, 1.0F };
	std::array<float, 3> tint;
	for (size_t i = 0; i < 3; ++i)
		tint[i] = std::clamp(mean[i] / luminance, ClassicTintMin, ClassicTintMax);
	return tint;
}

std::array<float, 3> ApplyClassicTint(const std::array<float, 3> &albedoLinear, const std::array<float, 3> &tint, float strength)
{
	const float luminance = LinearLuminance(albedoLinear);
	std::array<float, 3> result;
	for (size_t i = 0; i < 3; ++i) {
		float recoloured = 0.0F;
		if (luminance > 0.0F) {
			const float own = albedoLinear[i] / luminance - 1.0F;
			recoloured = luminance * std::max(tint[i] + own * ClassicTintHdChromaKeep, 0.0F);
		}
		result[i] = std::clamp(albedoLinear[i] + (recoloured - albedoLinear[i]) * strength, 0.0F, 1.0F);
	}
	return result;
}

float ClassicTintStrength(int tileset)
{
	return tileset == 1 ? 1.0F : 0.0F; // hd_tiles.cpp TilesetForLevel: 0 town, 1 Cathedral
}

} // namespace devilution::hd_tiles
