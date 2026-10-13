#include "engine/render/hd_sprites_core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace devilution::hd_sprites {

namespace {

std::string_view Trim(std::string_view text)
{
	const size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos)
		return {};
	const size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

std::vector<std::string_view> SplitFields(std::string_view line)
{
	std::vector<std::string_view> fields;
	size_t pos = 0;
	while (pos < line.size()) {
		pos = line.find_first_not_of(" \t", pos);
		if (pos == std::string_view::npos)
			break;
		const size_t end = std::min(line.find_first_of(" \t", pos), line.size());
		fields.push_back(line.substr(pos, end - pos));
		pos = end;
	}
	return fields;
}

bool ParseInt(std::string_view field, int &out)
{
	const char *end = field.data() + field.size();
	const auto result = std::from_chars(field.data(), end, out);
	return result.ec == std::errc() && result.ptr == end;
}

int Wrap(int frame, int clipFrames, bool loop)
{
	if (clipFrames <= 0)
		return 0;
	if (loop) {
		frame %= clipFrames;
		return frame < 0 ? frame + clipFrames : frame;
	}
	return std::clamp(frame, 0, clipFrames - 1);
}

} // namespace

int SpriteIndex::find(std::string_view key) const
{
	const auto it = byKey.find(std::string(key));
	return it != byKey.end() ? it->second : -1;
}

SpriteIndex ParseSpriteIndex(std::string_view text)
{
	SpriteIndex index;
	size_t lineNumber = 0;
	while (!text.empty()) {
		const size_t newline = text.find('\n');
		std::string_view line = text.substr(0, newline);
		text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);
		++lineNumber;
		if (const size_t comment = line.find('#'); comment != std::string_view::npos)
			line = line.substr(0, comment);
		line = Trim(line);
		if (line.empty())
			continue;
		const std::vector<std::string_view> fields = SplitFields(line);
		if (fields[0] != "clip") {
			index.errors.push_back("line " + std::to_string(lineNumber) + ": unknown record '" + std::string(fields[0]) + "'");
			continue;
		}
		ClipInfo clip;
		int loop = 0;
		if (fields.size() != 10
		    || !ParseInt(fields[3], clip.frames) || !ParseInt(fields[4], clip.cols)
		    || !ParseInt(fields[5], clip.cellWidth) || !ParseInt(fields[6], clip.cellHeight)
		    || !ParseInt(fields[7], clip.pivotX) || !ParseInt(fields[8], clip.pivotY)
		    || !ParseInt(fields[9], loop) || (loop != 0 && loop != 1)) {
			index.errors.push_back("line " + std::to_string(lineNumber) + ": malformed clip record");
			continue;
		}
		if (clip.frames <= 0 || clip.cols <= 0 || clip.cellWidth <= 0 || clip.cellHeight <= 0) {
			index.errors.push_back("line " + std::to_string(lineNumber) + ": clip sizes must be positive");
			continue;
		}
		clip.key = std::string(fields[1]);
		clip.png = std::string(fields[2]);
		clip.loop = loop != 0;
		if (index.byKey.contains(clip.key)) {
			index.errors.push_back("line " + std::to_string(lineNumber) + ": duplicate clip '" + clip.key + "'");
			continue;
		}
		index.byKey.emplace(clip.key, static_cast<int>(index.clips.size()));
		index.clips.push_back(std::move(clip));
	}
	return index;
}

int SelectFrame(int clipFrames, bool loop, int currentFrame, int numberOfFrames, int tick, float subtick, int ticksPerFrame)
{
	if (clipFrames <= 0 || numberOfFrames <= 0)
		return 0;
	ticksPerFrame = std::max(ticksPerFrame, 1);
	const double position = std::max(currentFrame, 0) + (std::max(tick, 0) + std::clamp(subtick, 0.0F, 1.0F)) / ticksPerFrame;
	const auto frame = static_cast<int>(std::floor(clipFrames * position / numberOfFrames));
	return Wrap(frame, clipFrames, loop);
}

int SelectFrameFromProgress(int clipFrames, bool loop, float progress)
{
	if (clipFrames <= 0)
		return 0;
	const auto frame = static_cast<int>(std::floor(clipFrames * static_cast<double>(std::max(progress, 0.0F))));
	return Wrap(frame, clipFrames, loop);
}

int SelectFrameReversed(int clipFrames, bool loop, int currentFrame, int numberOfFrames, int tick, float subtick, int ticksPerFrame)
{
	if (clipFrames <= 0 || numberOfFrames <= 0)
		return 0;
	ticksPerFrame = std::max(ticksPerFrame, 1);
	const double elapsed = std::min((std::max(tick, 0) + static_cast<double>(std::clamp(subtick, 0.0F, 1.0F))) / ticksPerFrame, 1.0);
	const double position = std::clamp(currentFrame, 0, numberOfFrames - 1) + 1.0 - elapsed;
	// position lies in (0, numberOfFrames]: the top end is the first instant of the last frame.
	const auto frame = std::min(static_cast<int>(std::floor(clipFrames * position / numberOfFrames)), clipFrames - 1);
	return Wrap(frame, clipFrames, loop);
}

int SelectTextureDivisor(float canvasPixelsPerWorldPixel)
{
	if (canvasPixelsPerWorldPixel > 2.0F)
		return 1;
	return canvasPixelsPerWorldPixel > 1.0F ? 2 : 4;
}

float OutlineRadius(float worldScale)
{
	return HdOutline::WorldWidth * std::max(worldScale, 1.0F);
}

float OutlineQuadPadding(float worldScale)
{
	return OutlineRadius(worldScale) + HdOutline::SoftEdge;
}

ImageRequest ClipImageRequest(const ClipInfo &clip, int divisor)
{
	ImageRequest request;
	request.expectedWidth = clip.sheetWidth();
	request.expectedHeight = clip.sheetHeight();
	divisor = std::max(divisor, 1);
	if (divisor > 1) {
		request.width = (request.expectedWidth + divisor - 1) / divisor;
		request.height = (request.expectedHeight + divisor - 1) / divisor;
	}
	return request;
}

std::string_view DirectionSuffix(int direction)
{
	constexpr std::string_view Suffixes[] = { "s", "sw", "w", "nw", "n", "ne", "e", "se" };
	if (direction < 0 || direction >= 8)
		return {};
	return Suffixes[direction];
}

void TextureLru::insert(int id, size_t bytes, uint64_t frame)
{
	Entry &entry = entries_[id];
	total_ = total_ - entry.bytes + bytes;
	entry.bytes = bytes;
	entry.lastUse = frame;
}

void TextureLru::touch(int id, uint64_t frame)
{
	const auto it = entries_.find(id);
	if (it != entries_.end())
		it->second.lastUse = frame;
}

void TextureLru::erase(int id)
{
	const auto it = entries_.find(id);
	if (it == entries_.end())
		return;
	total_ -= it->second.bytes;
	entries_.erase(it);
}

void TextureLru::clear()
{
	entries_.clear();
	total_ = 0;
}

void TextureLru::setPinned(int id, bool pinned)
{
	if (pinned)
		pinned_.insert(id);
	else
		pinned_.erase(id);
}

std::vector<int> TextureLru::evict(uint64_t currentFrame)
{
	std::vector<int> evicted;
	while (total_ > budget_) {
		int oldestId = 0;
		uint64_t oldestUse = std::numeric_limits<uint64_t>::max();
		bool found = false;
		for (const auto &[id, entry] : entries_) {
			if (entry.lastUse >= currentFrame || pinned_.contains(id))
				continue;
			// Ties (same frame) resolve to the lower id so eviction order is deterministic.
			if (entry.lastUse < oldestUse || (entry.lastUse == oldestUse && id < oldestId)) {
				oldestUse = entry.lastUse;
				oldestId = id;
				found = true;
			}
		}
		if (!found)
			break;
		erase(oldestId);
		evicted.push_back(oldestId);
	}
	return evicted;
}

namespace {

constexpr std::string_view WalkAction = "walk";
/** Town walk (weapon stowed); packs without it fall back to WalkAction. */
constexpr std::string_view TownWalkAction = "town_walk";

std::string_view IdleAction(bool inTown)
{
	return inTown ? "town_stand" : "stand";
}

bool IsIdleAction(std::string_view action)
{
	return action == "stand" || action == "town_stand";
}

std::string ClipKey(std::string_view prefix, std::string_view action, int direction)
{
	const std::string_view suffix = DirectionSuffix(direction);
	std::string key;
	key.reserve(prefix.size() + action.size() + 1 + suffix.size());
	key.append(prefix).append(action).append("/").append(suffix);
	return key;
}

} // namespace

std::string PlayerAppearancePrefix(std::string_view className, uint8_t graphicNumber, std::string_view action, bool inTown)
{
	constexpr std::string_view Armors[] = { "light", "medium", "heavy" };
	constexpr std::string_view Weapons[] = { "unarmed", "unarmed_shield", "sword", "sword_shield", "bow", "axe", "mace", "mace_shield", "staff" };
	const unsigned armor = graphicNumber >> 4;
	unsigned weapon = graphicNumber & 0xF;
	if (className.empty() || armor >= std::size(Armors) || weapon >= std::size(Weapons))
		return {};
	if (action == "death") {
		weapon = 0; // The original game shares unarmed death clips across weapons.
	} else if (inTown && action == "cast") {
		if (weapon == 2 || weapon == 6)
			weapon = 0;
		else if (weapon == 3 || weapon == 7)
			weapon = 1;
	}
	return std::string(className) + "/" + std::string(Armors[armor]) + "/" + std::string(Weapons[weapon]) + "/";
}

std::string ResolvePlayerClipPrefix(const SpriteIndex &index, std::string_view className, uint8_t graphicNumber,
    std::string_view action, bool inTown)
{
	const std::string prefix = PlayerAppearancePrefix(className, graphicNumber, action, inTown);
	if (prefix.empty())
		return {};
	// Probe the actions eligible as same-facing stand-ins, without scanning unrelated actor clips.
	for (const std::string_view candidate : { action, std::string_view("stand"), std::string_view("town_stand"), std::string_view("walk") }) {
		for (int direction = 0; direction < 8; ++direction) {
			if (index.find(ClipKey(prefix, candidate, direction)) >= 0)
				return prefix;
		}
	}
	if (prefix == "warrior/light/sword_shield/") {
		for (int direction = 0; direction < 8; ++direction) {
			if (index.find(ClipKey("warrior/", action, direction)) >= 0)
				return "warrior/";
		}
	}
	return {}; // Missing equipment must retain its correctly equipped classic appearance.
}

std::string_view PlayerActionForLocation(const SpriteIndex &index, std::string_view prefix, std::string_view action, int direction, bool inTown)
{
	if (inTown && action == WalkAction && !DirectionSuffix(direction).empty()
	    && index.find(ClipKey(prefix, TownWalkAction, direction)) >= 0)
		return TownWalkAction;
	return action;
}

std::vector<BodyClipCandidate> BodyClipCandidates(std::string_view action, int direction, bool inTown, bool allowNeighbourDirections)
{
	std::vector<BodyClipCandidate> candidates;
	if (action.empty() || DirectionSuffix(direction).empty())
		return candidates;
	const auto add = [&candidates](std::string_view candidateAction, int candidateDirection, bool animate) {
		for (const BodyClipCandidate &existing : candidates) {
			if (existing.action == candidateAction && existing.direction == candidateDirection)
				return;
		}
		candidates.push_back({ candidateAction, candidateDirection, animate });
	};
	const bool wantsIdle = IsIdleAction(action);
	const bool wantsWalk = action == WalkAction || action == TownWalkAction;

	add(action, direction, true);
	add(IdleAction(inTown), direction, wantsIdle);
	add(IdleAction(!inTown), direction, wantsIdle);
	add(WalkAction, direction, wantsWalk);
	if (!allowNeighbourDirections)
		return candidates;
	for (int distance = 1; distance <= 4; ++distance) {
		for (const int sign : { 1, -1 }) {
			const int neighbour = (direction + sign * distance + 8) % 8;
			add(action, neighbour, true);
			add(WalkAction, neighbour, wantsWalk);
		}
	}
	return candidates;
}

BodyClipChoice ChooseBodyClip(const SpriteIndex &index, std::string_view prefix, std::string_view action, int direction, bool inTown,
    const std::function<ClipStatus(int)> &status, bool allowNeighbourDirections)
{
	const std::vector<BodyClipCandidate> candidates = BodyClipCandidates(action, direction, inTown, allowNeighbourDirections);
	return ChooseClip(index, prefix, candidates, status);
}

BodyClipChoice ChooseClip(const SpriteIndex &index, std::string_view prefix, std::span<const BodyClipCandidate> candidates,
    const std::function<ClipStatus(int)> &status)
{
	BodyClipChoice choice;
	bool first = true;
	for (const BodyClipCandidate &candidate : candidates) {
		const bool exact = first;
		first = false;
		const int clip = index.find(ClipKey(prefix, candidate.action, candidate.direction));
		if (clip < 0)
			continue;
		switch (status(clip)) {
		case ClipStatus::Resident:
			choice.clip = clip;
			choice.animate = candidate.animate;
			choice.exact = exact;
			choice.hide = false;
			return choice;
		case ClipStatus::Loading:
			choice.hide = true;
			break;
		case ClipStatus::Unavailable:
			break;
		}
	}
	return choice;
}

namespace {

constexpr std::string_view MonsterStand = "stand";
constexpr std::string_view MonsterDeath = "death";
/** PlanMonsterPreload priority: the stand-in of every action first, corpses (death) before the short actions. */
constexpr std::string_view MonsterPreloadOrder[] = { "stand", "walk", "death", "attack", "hit", "special" };

} // namespace

std::vector<BodyClipCandidate> MonsterClipCandidates(std::string_view action, int direction)
{
	std::vector<BodyClipCandidate> candidates;
	if (action.empty() || DirectionSuffix(direction).empty())
		return candidates;
	const auto add = [&candidates](std::string_view candidateAction, int candidateDirection, bool animate) {
		for (const BodyClipCandidate &existing : candidates) {
			if (existing.action == candidateAction && existing.direction == candidateDirection)
				return;
		}
		candidates.push_back({ candidateAction, candidateDirection, animate });
	};
	const bool isDeath = action == MonsterDeath;
	const bool wantsStand = action == MonsterStand;
	const bool wantsWalk = action == WalkAction;
	const auto addFacing = [&](int candidateDirection) {
		add(action, candidateDirection, true);
		if (isDeath)
			return;
		add(MonsterStand, candidateDirection, wantsStand);
		add(WalkAction, candidateDirection, wantsWalk);
	};
	add(action, direction, true);
	if (!isDeath) {
		add(MonsterStand, direction, wantsStand);
		add(WalkAction, direction, wantsWalk);
	}
	for (int distance = 1; distance <= 4; ++distance) {
		for (const int sign : { 1, -1 })
			addFacing((direction + sign * distance + 8) % 8);
	}
	return candidates;
}

MonsterPreload PlanMonsterPreload(const SpriteIndex &index, std::span<const std::string> prefixes, const std::function<size_t(int)> &bytes,
    size_t pinBudget, size_t prefetchBudget)
{
	MonsterPreload preload;
	std::vector<int> group;
	for (const std::string_view action : MonsterPreloadOrder) {
		for (const std::string &prefix : prefixes) {
			group.clear();
			size_t groupBytes = 0;
			for (int direction = 0; direction < 8; ++direction) {
				const int clip = index.find(ClipKey(prefix, action, direction));
				if (clip < 0 || std::find(preload.pinned.begin(), preload.pinned.end(), clip) != preload.pinned.end()
				    || std::find(preload.prefetched.begin(), preload.prefetched.end(), clip) != preload.prefetched.end())
					continue;
				group.push_back(clip);
				groupBytes += bytes(clip);
			}
			if (group.empty())
				continue;
			if (preload.pinnedBytes + groupBytes <= pinBudget) {
				preload.pinned.insert(preload.pinned.end(), group.begin(), group.end());
				preload.pinnedBytes += groupBytes;
			} else if (preload.prefetchedBytes + groupBytes <= prefetchBudget) {
				preload.prefetched.insert(preload.prefetched.end(), group.begin(), group.end());
				preload.prefetchedBytes += groupBytes;
			}
		}
	}
	return preload;
}

PlayerPreload PlanPlayerPreload(const SpriteIndex &index, std::string_view prefix, bool inTown, const std::function<size_t(int)> &bytes,
    size_t idlePinBudget)
{
	PlayerPreload preload;
	std::vector<int> idle;
	size_t idleBytes = 0;
	for (int direction = 0; direction < 8; ++direction) {
		const std::string_view walkAction = PlayerActionForLocation(index, prefix, WalkAction, direction, inTown);
		if (const int walk = index.find(ClipKey(prefix, walkAction, direction)); walk >= 0)
			preload.pinned.push_back(walk);
		if (const int stand = index.find(ClipKey(prefix, IdleAction(inTown), direction)); stand >= 0) {
			idle.push_back(stand);
			idleBytes += bytes(stand);
		}
	}
	std::vector<int> &idleTarget = idleBytes <= idlePinBudget ? preload.pinned : preload.prefetched;
	idleTarget.insert(idleTarget.end(), idle.begin(), idle.end());
	return preload;
}

std::array<float, 3> RecolourTint(std::span<const uint32_t, 256> histogram, std::span<const uint8_t, 768> paletteRgb,
    std::span<const uint8_t> wanted, std::span<const uint8_t> reference)
{
	std::array<double, 3> wantedSum {};
	std::array<double, 3> referenceSum {};
	const auto map = [](std::span<const uint8_t> trn, size_t index) -> size_t { return trn.size() == 256 ? trn[index] : index; };
	for (size_t i = 0; i < 256; ++i) {
		if (histogram[i] == 0)
			continue;
		const size_t w = map(wanted, i);
		const size_t r = map(reference, i);
		for (size_t c = 0; c < 3; ++c) {
			wantedSum[c] += static_cast<double>(histogram[i]) * paletteRgb[w * 3 + c];
			referenceSum[c] += static_cast<double>(histogram[i]) * paletteRgb[r * 3 + c];
		}
	}
	std::array<float, 3> tint { 1.0F, 1.0F, 1.0F };
	for (size_t c = 0; c < 3; ++c) {
		if (referenceSum[c] > 0.0)
			tint[c] = static_cast<float>(std::clamp(wantedSum[c] / referenceSum[c], 0.125, 8.0));
	}
	return tint;
}

std::array<uint8_t, 256> InverseTrn(std::span<const uint8_t, 256> trn)
{
	std::array<uint8_t, 256> inverse;
	std::array<bool, 256> assigned {};
	for (size_t i = 0; i < 256; ++i)
		inverse[i] = static_cast<uint8_t>(i);
	for (size_t i = 0; i < 256; ++i) {
		const uint8_t target = trn[i];
		if (target == i || assigned[target])
			continue;
		inverse[target] = static_cast<uint8_t>(i);
		assigned[target] = true;
	}
	return inverse;
}

void OcclusionTracker::reset(int width, int height)
{
	instances_.clear();
	snapshots_.clear();
	width = std::max(width, 0);
	height = std::max(height, 0);
	const size_t size = static_cast<size_t>(width) * height;
	if (width != width_ || height != height_ || plane_.size() != size) {
		plane_.assign(size, 0);
		planeRects_.clear();
	}
	width_ = width;
	height_ = height;
}

int OcclusionTracker::record(const uint8_t *pixels, int pitch, PixelRect rect)
{
	if (pixels == nullptr || instances_.size() >= WriterMask - 1)
		return -1;
	const int left = std::max(rect.x, 0);
	const int top = std::max(rect.y, 0);
	const int right = std::min(rect.x + rect.width, width_);
	const int bottom = std::min(rect.y + rect.height, height_);
	if (right <= left || bottom <= top)
		return -1;
	Instance instance;
	instance.rect = { left, top, right - left, bottom - top };
	instance.snapshotOffset = snapshots_.size();
	snapshots_.resize(snapshots_.size() + static_cast<size_t>(instance.rect.width) * instance.rect.height);
	uint8_t *snapshot = snapshots_.data() + instance.snapshotOffset;
	for (int y = top; y < bottom; ++y) {
		const uint8_t *row = pixels + static_cast<size_t>(y) * pitch + left;
		std::copy_n(row, instance.rect.width, snapshot);
		snapshot += instance.rect.width;
	}
	instances_.push_back(instance);
	return static_cast<int>(instances_.size() - 1);
}

void OcclusionTracker::remap(const HdWorldTransform &transform)
{
	if (transform.IsIdentity() || instances_.empty())
		return;
	std::vector<uint8_t> remapped;
	for (Instance &instance : instances_) {
		const PixelRect source = instance.rect;
		const HdRect screen = transform.ToScreen({ source.x, source.y, source.width, source.height });
		// Also clip to the plane (the transform's viewport lies inside the back buffer it was made for).
		const int left = std::max(screen.x, 0);
		const int top = std::max(screen.y, 0);
		const int right = std::min(screen.x + screen.width, width_);
		const int bottom = std::min(screen.y + screen.height, height_);
		const size_t offset = remapped.size();
		const uint8_t *snapshot = snapshots_.data() + instance.snapshotOffset;
		instance.snapshotOffset = offset;
		if (source.empty() || right <= left || bottom <= top) {
			instance.rect = { std::clamp(screen.x, 0, width_), std::clamp(screen.y, 0, height_), 0, 0 };
			instance.visible = false;
			continue;
		}
		instance.rect = { left, top, right - left, bottom - top };
		remapped.resize(offset + static_cast<size_t>(instance.rect.width) * instance.rect.height);
		uint8_t *out = remapped.data() + offset;
		for (int y = top; y < bottom; ++y) {
			const int sy = std::clamp(transform.WorldY(y) - source.y, 0, source.height - 1);
			const uint8_t *row = snapshot + static_cast<size_t>(sy) * source.width;
			for (int x = left; x < right; ++x)
				*out++ = row[std::clamp(transform.WorldX(x) - source.x, 0, source.width - 1)];
		}
	}
	snapshots_ = std::move(remapped);
}

void OcclusionTracker::resolve(const uint8_t *pixels, int pitch, std::span<const PixelRect> translucent, PixelRect &bounds)
{
	bounds = {};
	if (instances_.empty() || pixels == nullptr)
		return;
	int left = width_, top = height_, right = 0, bottom = 0;
	for (const Instance &instance : instances_) {
		if (instance.rect.empty())
			continue; // clipped away by remap(): never visible
		left = std::min(left, instance.rect.x);
		top = std::min(top, instance.rect.y);
		right = std::max(right, instance.rect.x + instance.rect.width);
		bottom = std::max(bottom, instance.rect.y + instance.rect.height);
	}
	if (right <= left || bottom <= top) {
		for (Instance &instance : instances_)
			instance.visible = false;
		return;
	}
	bounds = { left, top, right - left, bottom - top };

	// The instances are often spread over the screen (their bounds several times their area), so everything below works
	// on the instance rectangles: the plane is 0 elsewhere. A pixel the hole closing can change has a non-zero neighbour,
	// so it lies within one pixel of an instance rectangle (`near`).
	const auto intersect = [](const PixelRect &a, const PixelRect &b) {
		const int x0 = std::max(a.x, b.x);
		const int y0 = std::max(a.y, b.y);
		const int x1 = std::min(a.x + a.width, b.x + b.width);
		const int y1 = std::min(a.y + a.height, b.y + b.height);
		return PixelRect { x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0) };
	};
	const auto grown = [&](const PixelRect &rect, int by) {
		return intersect({ rect.x - by, rect.y - by, rect.width + 2 * by, rect.height + 2 * by }, bounds);
	};
	const auto fill = [&](auto &values, const PixelRect &rect, auto value) {
		for (int y = rect.y; y < rect.y + rect.height; ++y)
			std::fill_n(values.data() + static_cast<size_t>(y) * width_ + rect.x, rect.width, value);
	};
	// Clear what the last resolve left (only its rectangles can be non-zero), then this resolve's rectangles.
	for (const PixelRect &rect : planeRects_)
		fill(plane_, rect, uint16_t { 0 });
	planeRects_.clear();
	for (const Instance &instance : instances_) {
		if (instance.rect.empty())
			continue;
		planeRects_.push_back(grown(instance.rect, 1));
		fill(plane_, planeRects_.back(), uint16_t { 0 });
	}

	// See-through wall coverage (1 = inside a translucent rectangle), where it is read: inside planeRects_.
	const size_t size = static_cast<size_t>(width_) * height_;
	if (translucent_.size() != size)
		translucent_.assign(size, 0);
	translucentRects_.clear();
	for (const PixelRect &wall : translucent) {
		for (const PixelRect &near : planeRects_) {
			const PixelRect rect = intersect(wall, near);
			if (rect.empty())
				continue;
			fill(translucent_, rect, uint8_t { 1 });
			translucentRects_.push_back(rect);
		}
	}

	for (size_t i = 0; i < instances_.size(); ++i) {
		Instance &instance = instances_[i];
		const PixelRect &r = instance.rect;
		const uint8_t *snapshot = snapshots_.data() + instance.snapshotOffset;
		const auto writer = static_cast<uint16_t>(i + 1);
		size_t changed = 0;
		for (int y = r.y; y < r.y + r.height; ++y, snapshot += r.width) {
			const uint8_t *row = pixels + static_cast<size_t>(y) * pitch + r.x;
			uint16_t *planeRow = plane_.data() + static_cast<size_t>(y) * width_ + r.x;
			const uint8_t *translucentRow = translucent_.data() + static_cast<size_t>(y) * width_ + r.x;
			const auto mark = [&](int from, int to) {
				for (int x = from; x < to; ++x) {
					if (row[x] != snapshot[x]) {
						planeRow[x] = std::max(planeRow[x], writer);
						if (translucentRow[x] == 0)
							++changed;
					}
				}
			};
			// Eight pixels at a time: most of an instance is still what it drew.
			int x = 0;
			for (; x + 8 <= r.width; x += 8) {
				uint64_t rowWord;
				uint64_t snapshotWord;
				std::memcpy(&rowWord, row + x, 8);
				std::memcpy(&snapshotWord, snapshot + x, 8);
				if (rowWord != snapshotWord)
					mark(x, x + 8);
			}
			mark(x, r.width);
		}
		const size_t area = static_cast<size_t>(r.width) * r.height;
		instance.visible = changed * 50 < area * 49; // >= 98 % changed: hidden, or no longer the world pass
	}

	// Close one-pixel holes left by later writes of the same palette index: a pixel whose two horizontal (or two
	// vertical) neighbours are both hidden for an instance is hidden for it too. min() of the pair keeps the plane
	// monotone (hidden for i implies hidden for every earlier instance). The closing reads the plane before it (scratch_,
	// copied two pixels around each instance: the neighbours of the pixels it can change), and repeating it on a pixel
	// where rectangles overlap changes nothing. Only the bounds' inner pixels are closed.
	if (scratch_.size() != size)
		scratch_.resize(size);
	for (const Instance &instance : instances_) {
		if (instance.rect.empty())
			continue;
		const PixelRect rect = grown(instance.rect, 2);
		for (int y = rect.y; y < rect.y + rect.height; ++y) {
			const size_t offset = static_cast<size_t>(y) * width_ + rect.x;
			std::copy_n(plane_.data() + offset, rect.width, scratch_.data() + offset);
		}
	}
	const PixelRect inner = intersect(bounds, { left + 1, top + 1, bounds.width - 2, bounds.height - 2 });
	for (const PixelRect &near : planeRects_) {
		const PixelRect rect = intersect(near, inner);
		for (int y = rect.y; y < rect.y + rect.height; ++y) {
			const uint16_t *above = scratch_.data() + static_cast<size_t>(y - 1) * width_;
			const uint16_t *row = scratch_.data() + static_cast<size_t>(y) * width_;
			const uint16_t *below = scratch_.data() + static_cast<size_t>(y + 1) * width_;
			uint16_t *planeRow = plane_.data() + static_cast<size_t>(y) * width_;
			for (int x = rect.x; x < rect.x + rect.width; ++x) {
				const uint16_t horizontal = std::min(row[x - 1], row[x + 1]);
				const uint16_t vertical = std::min(above[x], below[x]);
				planeRow[x] = std::max({ planeRow[x], horizontal, vertical });
			}
		}
	}

	for (const PixelRect &rect : translucentRects_) {
		for (int y = rect.y; y < rect.y + rect.height; ++y) {
			uint16_t *planeRow = plane_.data() + static_cast<size_t>(y) * width_;
			for (int x = rect.x; x < rect.x + rect.width; ++x) {
				if (planeRow[x] != 0)
					planeRow[x] |= TranslucentBit;
			}
		}
	}
	// translucent_ stays all 0 between resolves.
	for (const PixelRect &rect : translucentRects_)
		fill(translucent_, rect, uint8_t { 0 });
}

} // namespace devilution::hd_sprites
