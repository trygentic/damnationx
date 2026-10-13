#include "engine/render/hd_objects_core.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

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

std::string LineError(size_t lineNumber, std::string_view message)
{
	return "line " + std::to_string(lineNumber) + ": " + std::string(message);
}

int SelectLoopFrame(int clipFrames, const ObjectBinding &binding, const ObjectAnimState &state)
{
	const int count = binding.last - binding.first + 1;
	return SelectFrame(clipFrames, /*loop=*/true, state.frame - binding.first, count, state.animating ? state.tick : 0,
	    state.animating ? state.subtick : 0.0F, state.ticksPerFrame);
}

} // namespace

const std::vector<int> *ObjectIndex::find(std::string_view cel) const
{
	const auto it = byCel.find(std::string(cel));
	return it != byCel.end() ? &it->second : nullptr;
}

ObjectIndex ParseObjectIndex(std::string_view text)
{
	ObjectIndex index;

	// First pass: the clip lines go through the hd/sprites.txt parser; other lines are blanked so line numbers match.
	std::string clipText;
	clipText.reserve(text.size());
	struct PendingBinding {
		ObjectBinding binding;
		size_t lineNumber;
	};
	std::vector<PendingBinding> pending;
	size_t lineNumber = 0;
	std::string_view rest = text;
	while (!rest.empty()) {
		const size_t newline = rest.find('\n');
		const std::string_view rawLine = rest.substr(0, newline);
		rest = newline == std::string_view::npos ? std::string_view {} : rest.substr(newline + 1);
		++lineNumber;
		std::string_view line = rawLine;
		if (const size_t comment = line.find('#'); comment != std::string_view::npos)
			line = line.substr(0, comment);
		line = Trim(line);
		const std::vector<std::string_view> fields = SplitFields(line);
		if (!fields.empty() && fields[0] == "clip") {
			clipText.append(line);
			clipText.push_back('\n');
			continue;
		}
		clipText.push_back('\n');
		if (fields.empty())
			continue;

		ObjectBinding binding;
		bool ok = false;
		if (fields[0] == "static") {
			binding.kind = ObjectBinding::Kind::Static;
			ok = fields.size() == 4 && ParseInt(fields[2], binding.first);
			binding.last = binding.first;
			if (ok)
				binding.clipKey = std::string(fields[3]);
		} else if (fields[0] == "anim" || fields[0] == "trans") {
			binding.kind = fields[0] == "anim" ? ObjectBinding::Kind::Anim : ObjectBinding::Kind::Transition;
			ok = fields.size() == 5 && ParseInt(fields[2], binding.first) && ParseInt(fields[3], binding.last);
			if (ok)
				binding.clipKey = std::string(fields[4]);
		} else {
			index.errors.push_back(LineError(lineNumber, "unknown record '" + std::string(fields[0]) + "'"));
			continue;
		}
		if (!ok) {
			index.errors.push_back(LineError(lineNumber, "malformed " + std::string(fields[0]) + " record"));
			continue;
		}
		if (binding.first < 1 || binding.last < 1 || (binding.kind == ObjectBinding::Kind::Anim && binding.last < binding.first)) {
			index.errors.push_back(LineError(lineNumber, "frames are 1-based and anim ranges must not be empty"));
			continue;
		}
		if (binding.kind == ObjectBinding::Kind::Transition && binding.first == binding.last) {
			index.errors.push_back(LineError(lineNumber, "a transition needs two different frames"));
			continue;
		}
		binding.cel = std::string(fields[1]);
		pending.push_back({ std::move(binding), lineNumber });
	}

	SpriteIndex clips = ParseSpriteIndex(clipText);
	index.errors.insert(index.errors.end(), clips.errors.begin(), clips.errors.end());
	for (PendingBinding &entry : pending) {
		ObjectBinding &binding = entry.binding;
		binding.clip = clips.find(binding.clipKey);
		if (binding.clip < 0) {
			index.errors.push_back(LineError(entry.lineNumber, "unknown clip '" + binding.clipKey + "'"));
			continue;
		}
		index.byCel[binding.cel].push_back(static_cast<int>(index.bindings.size()));
		index.bindings.push_back(std::move(binding));
	}
	index.clips = std::move(clips.clips);
	return index;
}

int SelectOneShotFrame(int clipFrames, float position, int count)
{
	if (clipFrames <= 0)
		return 0;
	const auto frame = static_cast<int>(std::floor(clipFrames * static_cast<double>(std::max(position, 0.0F)) / std::max(count - 1, 1)));
	return std::clamp(frame, 0, clipFrames - 1);
}

ObjectSelection SelectObjectFrame(const ObjectIndex &index, std::string_view cel, const ObjectAnimState &state)
{
	const std::vector<int> *bindings = index.find(cel);
	if (bindings == nullptr)
		return {};

	int staticMatch = -1;
	int animMatch = -1;
	for (const int i : *bindings) {
		const ObjectBinding &binding = index.bindings[i];
		if (binding.kind == ObjectBinding::Kind::Static && binding.first == state.frame && staticMatch < 0)
			staticMatch = i;
		else if (binding.kind == ObjectBinding::Kind::Anim && state.frame >= binding.first && state.frame <= binding.last && animMatch < 0)
			animMatch = i;
	}
	const int chosen = (state.animating && animMatch >= 0) || staticMatch < 0 ? animMatch : staticMatch;
	if (chosen < 0)
		return {};

	const ObjectBinding &binding = index.bindings[chosen];
	const ClipInfo &clip = index.clips[binding.clip];
	if (binding.kind == ObjectBinding::Kind::Static)
		return { chosen, 0 };
	if (clip.loop)
		return { chosen, SelectLoopFrame(clip.frames, binding, state) };
	float position = static_cast<float>(state.frame - binding.first);
	if (state.animating)
		position += (std::max(state.tick, 0) + std::clamp(state.subtick, 0.0F, 1.0F)) / std::max(state.ticksPerFrame, 1);
	return { chosen, SelectOneShotFrame(clip.frames, position, binding.last - binding.first + 1) };
}

int FindObjectTransition(const ObjectIndex &index, std::string_view cel, int from, int to)
{
	const std::vector<int> *bindings = index.find(cel);
	if (bindings == nullptr)
		return -1;
	for (const int i : *bindings) {
		const ObjectBinding &binding = index.bindings[i];
		if (binding.kind == ObjectBinding::Kind::Transition && binding.first == from && binding.last == to)
			return i;
	}
	return -1;
}

int TransitionFrame(int clipFrames, float elapsedTicks)
{
	if (clipFrames <= 0 || elapsedTicks < 0.0F)
		return -1;
	// 60 FPS clips: three HD frames per game tick.
	const auto frame = static_cast<int>(std::floor(elapsedTicks * 3.0F));
	return frame < clipFrames ? frame : -1;
}

} // namespace devilution::hd_sprites
