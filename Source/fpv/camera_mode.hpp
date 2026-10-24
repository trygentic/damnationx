#pragma once
/**
 * @file fpv/camera_mode.hpp
 *
 * Camera modes of the game view (Graphics > Camera Mode, saved as "Camera Mode" in diablo.ini). Kept free of other
 * includes so options.h can use it.
 */

#include <cstdint>

namespace devilution {

enum class CameraMode : uint8_t {
	/** The classic isometric view. */
	Isometric = 0,
	/** First person over the WebGPU 3D world, with the weapon viewmodel. */
	FirstPerson = 1,
	/** Third person over the WebGPU 3D world: a chase camera behind the 3D warrior (or the player's sprite). */
	ThirdPerson = 2,
};

enum class ThirdPersonStyle : uint8_t {
	/** Close camera offset to one side, crosshair aiming (swap the shoulder with the middle mouse button). */
	OverShoulder = 0,
	/** Centred orbit camera further back that swings behind the player while it walks to a clicked target. */
	Orbit = 1,
};

enum class ViewMovement : uint8_t {
	/** Continuous camera-relative movement (WASD / analog stick) on top of free-form movement. */
	Smooth = 0,
	/** One tile per step along the nearest of the eight directions, like a classic dungeon crawler. */
	TileSteps = 1,
};

} // namespace devilution
