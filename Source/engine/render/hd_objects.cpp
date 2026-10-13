#include "engine/render/hd_objects.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "engine/animationinfo.h"
#include "engine/assets.hpp"
#include "engine/render/hd_objects_core.hpp"
#include "engine/render/hd_sprites.hpp"
#include "engine/surface.hpp"
#include "levels/gendung.h"
#include "nthread.h"
#include "objects.h"
#include "tables/objdat.h"
#include "utils/log.hpp"
#include "utils/timer.hpp"

namespace devilution {
namespace {

constexpr char ObjectIndexAsset[] = "hd\\objects.txt";
constexpr char ObjectIndexUrl[] = "hd/objects.txt";
/** The classic animation "hold": finished one-shots park on their last frame with this delay (ObjectStopAnim). */
constexpr int HeldAnimDelay = 1000;
/** A frame change seen after a longer gap (the object was off screen) snaps to the new state without its transition. */
constexpr uint32_t TransitionMaxGapMs = 250;
constexpr float MsPerGameTick = 50.0F;

enum class IndexState : uint8_t { Unloaded, Loading, Loaded, Failed };

IndexState CurrentIndexState = IndexState::Unloaded;
hd_sprites::ObjectIndex ObjectIndex;
/** ObjectIndex::clips -> HD sprite index (HdSpritesAppendClip). */
std::vector<int> GlobalClip;

/** Per Objects[] slot: the last frame drawn and a running transition. */
struct ObjectTrack {
	_object_id type = OBJ_NULL;
	Point position;
	int frame = -1;
	uint32_t lastSeenMs = 0;
	int transition = -1; // ObjectIndex::bindings
	uint32_t transitionStartMs = 0;
};
std::array<ObjectTrack, MAXOBJECTS> Tracks;
int TrackedLevel = -1;

void AcceptIndex(std::string_view text, std::string_view source)
{
	ObjectIndex = hd_sprites::ParseObjectIndex(text);
	for (const std::string &error : ObjectIndex.errors)
		LogError("HD objects ({}): {}", source, error);
	GlobalClip.clear();
	for (const hd_sprites::ClipInfo &clip : ObjectIndex.clips)
		GlobalClip.push_back(HdSpritesAppendClip(clip));
	CurrentIndexState = ObjectIndex.bindings.empty() ? IndexState::Failed : IndexState::Loaded;
	Log("HD objects: {} clips, {} bindings from {}", ObjectIndex.clips.size(), ObjectIndex.bindings.size(), source);
}

/** Loads hd/objects.txt (assets, else fetched next to the page) once the sprite index has settled. */
bool EnsureObjectIndex()
{
	switch (CurrentIndexState) {
	case IndexState::Loaded:
		return true;
	case IndexState::Failed:
		return false;
	case IndexState::Unloaded:
	case IndexState::Loading:
		break;
	}
	// Object clips are appended to the sprite index: wait until hd/sprites.txt can no longer replace it.
	if (!HdSpritesIndexSettled())
		return false;
	if (CurrentIndexState == IndexState::Unloaded) {
		if (FindAsset(ObjectIndexAsset).ok()) {
			auto asset = LoadAsset(ObjectIndexAsset);
			if (asset.has_value()) {
				AcceptIndex(static_cast<std::string_view>(*asset), "assets");
				return CurrentIndexState == IndexState::Loaded;
			}
		}
		CurrentIndexState = IndexState::Loading;
	}
	std::string text;
	bool failed = false;
	if (HdSpritesGpuFetchText(ObjectIndexUrl, text, failed)) {
		AcceptIndex(text, ObjectIndexUrl);
	} else if (failed) {
		Log("HD objects: hd/objects.txt is unavailable; objects stay classic");
		CurrentIndexState = IndexState::Failed;
	}
	return CurrentIndexState == IndexState::Loaded;
}

std::string_view CelName(const Object &object)
{
	const auto type = static_cast<size_t>(object._otype);
	if (type >= AllObjects.size())
		return {};
	const size_t file = AllObjects[type].ofindex;
	return file < ObjMasterLoadList.size() ? std::string_view(ObjMasterLoadList[file]) : std::string_view {};
}

int LevelKey()
{
	return (setlevel ? 0x10000 + static_cast<int>(setlvlnum) : static_cast<int>(currlevel)) * 16 + static_cast<int>(leveltype);
}

/**
 * Advances the object's track to its current frame: a jump with a `trans` binding (seen in consecutive frames) starts
 * the transition. Returns the running transition's binding index and its clip frame, or -1.
 */
int UpdateTrack(const Object &object, std::string_view cel, int &transitionFrame)
{
	const auto slot = static_cast<size_t>(&object - Objects);
	if (slot >= Tracks.size())
		return -1;
	if (TrackedLevel != LevelKey()) {
		Tracks.fill({});
		TrackedLevel = LevelKey();
	}
	ObjectTrack &track = Tracks[slot];
	const uint32_t now = GetMillisecondsSinceStartup();
	const int frame = static_cast<int>(object._oAnimFrame);
	if (track.type != object._otype || track.position != object.position) {
		track = { object._otype, object.position, frame, now, -1, 0 };
		return -1;
	}
	if (frame != track.frame) {
		track.transition = now - track.lastSeenMs <= TransitionMaxGapMs ? hd_sprites::FindObjectTransition(ObjectIndex, cel, track.frame, frame) : -1;
		track.transitionStartMs = now;
		track.frame = frame;
	}
	track.lastSeenMs = now;
	if (track.transition < 0)
		return -1;
	const hd_sprites::ObjectBinding &binding = ObjectIndex.bindings[track.transition];
	// The object moved on (closed again, another frame) while the transition ran: drop it.
	if (binding.last != frame) {
		track.transition = -1;
		return -1;
	}
	transitionFrame = hd_sprites::TransitionFrame(ObjectIndex.clips[binding.clip].frames, static_cast<float>(now - track.transitionStartMs) / MsPerGameTick);
	if (transitionFrame < 0)
		track.transition = -1;
	return track.transition;
}

} // namespace

bool HdSpritesDrawObject(const Surface &out, const Object &object, Point spriteBottomLeft, int spriteWidth, int lightTableIndex)
{
	if (!HdSpritesWorldPassAllowed() || !EnsureObjectIndex())
		return false;
	const std::string_view cel = CelName(object);
	if (cel.empty() || ObjectIndex.find(cel) == nullptr)
		return false;

	int frame = 0;
	int binding = UpdateTrack(object, cel, frame);
	if (binding < 0) {
		const bool animating = object._oAnimFlag != 0 && object._oAnimDelay < HeldAnimDelay;
		const hd_sprites::ObjectAnimState state {
			.frame = static_cast<int>(object._oAnimFrame),
			.animating = animating,
			.tick = object._oAnimCnt,
			.ticksPerFrame = object._oAnimDelay,
			.subtick = animating ? static_cast<float>(ProgressToNextGameTick) / AnimationInfo::baseValueFraction : 0.0F,
		};
		const hd_sprites::ObjectSelection selection = hd_sprites::SelectObjectFrame(ObjectIndex, cel, state);
		binding = selection.binding;
		frame = selection.frame;
	}
	if (binding < 0)
		return false;
	const int clip = GlobalClip[ObjectIndex.bindings[binding].clip];
	if (!HdSpritesGpuRequestClip(clip))
		return false;

	// Object clips pivot on canvas pixel (W/2, H-1) of the 4x classic frame: the frame's bottom centre. The classic
	// bottom row is blitted at spriteBottomLeft.y, so the top edge of that 4x row lies 3/4 of a pixel below it.
	const float pivotX = static_cast<float>(spriteBottomLeft.x) + spriteWidth / 2.0F;
	const float pivotY = static_cast<float>(spriteBottomLeft.y) + 0.75F;
	const bool selfLit = !object.applyLighting;
	return HdSpritesRecord(out, clip, frame, pivotX, pivotY, static_cast<float>(spriteBottomLeft.y), selfLit ? 0 : lightTableIndex,
	    /*dropShadow=*/false, selfLit);
}

} // namespace devilution
