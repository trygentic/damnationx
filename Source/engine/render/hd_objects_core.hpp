/**
 * @file hd_objects_core.hpp
 *
 * Engine-independent parts of the opt-in HD dungeon object sprites:
 * the hd/objects.txt parser and the choice of HD clip and frame for an object's classic CEL frame and animation state.
 *
 * Nothing here touches game state, SDL or WebGPU so it is covered by native unit tests (test/hd_objects_test.cpp).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/render/hd_sprites_core.hpp"

namespace devilution::hd_sprites {

/**
 * One binding line of hd/objects.txt: which classic CEL frames of an object file (objdat.tsv `file`, e.g. "chest1")
 * an HD clip replaces. Frames are 1-based like Object::_oAnimFrame.
 */
struct ObjectBinding {
	enum class Kind : uint8_t {
		/** `static <cel> <frame> <key>`: shown while the object rests on `first` (== `last`). */
		Static,
		/** `anim <cel> <first> <last> <key>`: the CEL frames first..last as one animation (loop or one-shot by the clip's loop flag). */
		Anim,
		/** `trans <cel> <from> <to> <key>`: played once at 60 FPS when the object's frame jumps from `first` to `last`. */
		Transition,
	};
	Kind kind = Kind::Static;
	std::string cel;
	int first = 0;
	int last = 0;
	std::string clipKey;
	/** Index into ObjectIndex::clips. */
	int clip = -1;
};

struct ObjectIndex {
	/** The `clip` lines of hd/objects.txt (same syntax as hd/sprites.txt). */
	std::vector<ClipInfo> clips;
	std::vector<ObjectBinding> bindings;
	/** CEL file name -> indices into `bindings`, in file order. */
	std::unordered_map<std::string, std::vector<int>> byCel;
	std::vector<std::string> errors;

	[[nodiscard]] const std::vector<int> *find(std::string_view cel) const;
};

/**
 * Parses hd/objects.txt: `clip` lines as in hd/sprites.txt plus `static`, `anim` and `trans` binding lines (see
 * ObjectBinding). Bindings may reference clips defined later in the file; a binding to an unknown clip is rejected.
 */
ObjectIndex ParseObjectIndex(std::string_view text);

/** The animation state of one object, as the classic renderer sees it. */
struct ObjectAnimState {
	/** Object::_oAnimFrame (1-based). */
	int frame = 1;
	/** _oAnimFlag set and the frame advancing (_oAnimDelay below the 1000-tick "hold" the engine parks finished animations with). */
	bool animating = false;
	/** Object::_oAnimCnt and _oAnimDelay. */
	int tick = 0;
	int ticksPerFrame = 1;
	/** Progress to the next game tick in [0, 1]; ignored while not animating. */
	float subtick = 0.0F;
};

struct ObjectSelection {
	/** Index into ObjectIndex::bindings, -1 when the object stays classic. */
	int binding = -1;
	int frame = 0;
};

/**
 * @brief Picks the HD clip frame for an object resting on or animating through `state.frame`.
 *
 * While animating, an `anim` binding containing the frame wins; at rest a `static` binding of exactly that frame wins,
 * then an `anim` binding containing it (frozen). Loop clips map proportionally to the 60 FPS clip (the contract frame
 * formula over first..last, which keeps the random start phase). One-shot clips reach their final frame when the
 * object arrives on `last` and hold it, the engine parking finished animations (opened sarcophagus, broken barrel,
 * used shrine) on their last frame.
 */
ObjectSelection SelectObjectFrame(const ObjectIndex &index, std::string_view cel, const ObjectAnimState &state);

/** Frame of a one-shot animation over `count` CEL frames: `position` = frames into it (+ tick progress). */
int SelectOneShotFrame(int clipFrames, float position, int count);

/** The `trans` binding for a frame change of `cel` from `from` to `to`, or -1. */
int FindObjectTransition(const ObjectIndex &index, std::string_view cel, int from, int to);

/** Frame of a transition clip `elapsedTicks` game ticks (20/s) after it started; -1 once it has finished. */
int TransitionFrame(int clipFrames, float elapsedTicks);

} // namespace devilution::hd_sprites
