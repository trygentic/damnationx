#include "fpv/camera_rig.hpp"
#include "fpv/hud_bridge.hpp"
#include "fpv/view_provider.hpp"

#include <cmath>
#include <vector>
#include <utility>

#include <gtest/gtest.h>

namespace devilution::fpv {
namespace {

constexpr float Eps = 1e-3F;

/** A wall filling the tile column x == 10 (all heights), and nothing else. */
bool WallAtX10(float x, float /*y*/, float /*z*/)
{
	return std::floor(x) == 10.0F;
}

/** Floor at y = 0 and a ceiling at y = 1, like a dungeon corridor. */
bool Corridor(float /*x*/, float y, float /*z*/)
{
	return y < 0.0F || y > 1.0F;
}

bool Open(float, float, float)
{
	return false;
}

ViewInputs BaseInputs()
{
	ViewInputs in;
	in.feet = { 5.5F, 0.0F, 5.5F };
	in.yaw = 0.0F; // looking toward +z
	in.pitch = 0.0F;
	in.distance = 2.0F;
	in.style = ThirdPersonStyle::Orbit;
	in.dt = 0.016F;
	return in;
}

TEST(FpvCameraTest, DesiredEyeSitsBehindAndToTheRight)
{
	const Vec3f pivot { 0.0F, 0.5F, 0.0F };
	const Vec3f eye = ThirdPersonDesiredEye(pivot, 0.0F, 0.0F, 2.0F, 0.3F);
	EXPECT_NEAR(eye.z, -2.0F, Eps); // behind: forward is +z
	EXPECT_NEAR(eye.y, 0.5F, Eps);
	EXPECT_NEAR(eye.x, -0.3F, Eps); // right of a +z-facing camera is -x
}

TEST(FpvCameraTest, LookingDownRaisesTheEye)
{
	const Vec3f eye = ThirdPersonDesiredEye({ 0.0F, 0.5F, 0.0F }, 0.0F, -0.5F, 2.0F, 0.0F);
	EXPECT_GT(eye.y, 0.5F);
	EXPECT_NEAR(eye.y, 0.5F + 2.0F * std::sin(0.5F), Eps);
}

TEST(FpvCameraTest, SweepStopsBeforeAWall)
{
	const float fraction = SweepCamera({ 5.5F, 0.5F, 5.5F }, { 12.5F, 0.5F, 5.5F }, 0.1F, WallAtX10);
	const float reach = 5.5F + 7.0F * fraction;
	EXPECT_LT(reach, 10.0F - 0.1F + Eps);
	EXPECT_GT(reach, 10.0F - 0.1F - 0.06F);
}

TEST(FpvCameraTest, SweepIsFreeInTheOpen)
{
	EXPECT_FLOAT_EQ(SweepCamera({ 0.0F, 0.5F, 0.0F }, { 3.0F, 1.5F, -2.0F }, 0.1F, Open), 1.0F);
}

TEST(FpvCameraTest, BoomSnapsInAndEasesOut)
{
	EXPECT_FLOAT_EQ(EaseBoom(2.0F, 0.5F, 0.016F), 0.5F);
	const float out = EaseBoom(0.5F, 2.0F, 0.016F);
	EXPECT_GT(out, 0.5F);
	EXPECT_LT(out, 2.0F);
}

TEST(FpvCameraTest, ZoomIsClamped)
{
	EXPECT_FLOAT_EQ(ZoomBoom(1.0F, 100.0F), kThirdPersonMinDistance);
	EXPECT_FLOAT_EQ(ZoomBoom(5.0F, -100.0F), kThirdPersonMaxDistance);
	EXPECT_LT(ZoomBoom(2.0F, 1.0F), 2.0F);
}

TEST(FpvCameraTest, ChaseAimRayStartsAtThePlayer)
{
	const Ray camera { { 0.0F, 0.5F, -2.0F }, { 0.0F, 0.0F, 1.0F } };
	const Ray aim = ChaseAimRay(camera, { 0.0F, 0.5F, 0.0F }, 0.25F);
	EXPECT_NEAR(aim.origin.z, -0.25F, Eps);
	EXPECT_NEAR(aim.dir.z, 1.0F, Eps);
	// Camera in front of the pivot (looking away from it): unchanged.
	const Ray ahead = ChaseAimRay(camera, { 0.0F, 0.5F, -5.0F });
	EXPECT_NEAR(ahead.origin.z, -2.0F, Eps);
}

TEST(FpvCameraTest, FirstPersonViewUsesTheEyeHeightAndBob)
{
	FirstPersonView view;
	ViewInputs in = BaseInputs();
	in.floorY = 0.2F;
	in.headBob = false;
	in.bobPhase = 1.0F;
	ViewOutput out = view.Update(in);
	EXPECT_NEAR(out.camera.y, 0.2F + kEyeHeight, Eps);
	EXPECT_TRUE(out.drawViewmodel);
	EXPECT_FALSE(out.drawLocalPlayer);
	in.headBob = true;
	out = view.Update(in);
	EXPECT_GT(out.camera.y, 0.2F + kEyeHeight);
}

TEST(FpvCameraTest, ThirdPersonOrbitSitsBehindThePlayer)
{
	ThirdPersonView view;
	ViewInputs in = BaseInputs();
	in.isBlocked = Open;
	view.Reset(in);
	const ViewOutput out = view.Update(in);
	EXPECT_NEAR(out.camera.x, 5.5F, Eps);
	EXPECT_NEAR(out.camera.z, 5.5F - 2.0F, Eps);
	EXPECT_NEAR(out.camera.y, kThirdPersonPivotHeight, Eps);
	EXPECT_TRUE(out.drawLocalPlayer);
	EXPECT_FALSE(out.drawViewmodel);
	EXPECT_TRUE(out.chaseAim);
}

TEST(FpvCameraTest, ThirdPersonCameraIsPulledInByAWall)
{
	ThirdPersonView view;
	ViewInputs in = BaseInputs();
	in.feet = { 8.5F, 0.0F, 5.5F };
	in.yaw = -kPi / 2.0F; // looking toward -x: the camera wants to sit at x = 10.5, inside the wall
	in.isBlocked = WallAtX10;
	view.Reset(in);
	const ViewOutput out = view.Update(in);
	EXPECT_LT(out.camera.x, 10.0F - 0.05F);
	EXPECT_LT(out.resolvedDistance, 1.5F);
}

TEST(FpvCameraTest, ThirdPersonStaysUnderTheDungeonCeiling)
{
	ThirdPersonView view;
	ViewInputs in = BaseInputs();
	in.pitch = -1.0F; // looking steeply down: the boom points up into the ceiling
	in.ceilingY = 0.95F;
	in.isBlocked = Corridor;
	view.Reset(in);
	const ViewOutput out = view.Update(in);
	EXPECT_LE(out.camera.y, 1.0F - 0.1F + Eps);
}

TEST(FpvCameraTest, ShoulderSwapEasesAcross)
{
	ThirdPersonView view;
	ViewInputs in = BaseInputs();
	in.style = ThirdPersonStyle::OverShoulder;
	in.isBlocked = Open;
	in.shoulderSide = 1.0F;
	view.Reset(in);
	const float right = view.Update(in).camera.x;
	in.shoulderSide = -1.0F;
	const float mid = view.Update(in).camera.x;
	for (int i = 0; i < 120; ++i)
		view.Update(in);
	const float left = view.Update(in).camera.x;
	EXPECT_LT(right, 5.5F); // right of a +z-facing camera is -x
	EXPECT_GT(left, 5.5F);
	EXPECT_GT(mid, right);
	EXPECT_LT(mid, left);
}

TEST(FpvCameraTest, OrbitChaseSwingsBehindAWalkingPlayer)
{
	ThirdPersonView view;
	ViewInputs in = BaseInputs();
	in.isBlocked = Open;
	view.Reset(in);
	in.autoWalking = true;
	in.idleLookSeconds = 2.0F;
	in.playerYaw = kPi / 2.0F;
	for (int i = 0; i < 300; ++i)
		view.Update(in);
	EXPECT_NEAR(view.yaw(), kPi / 2.0F, 0.01F);
	// Looking around stops the chase: the mouse yaw wins.
	in.idleLookSeconds = 0.0F;
	view.Update(in);
	EXPECT_NEAR(view.yaw(), 0.0F, Eps);
}

TEST(FpvCameraTest, TileStepFollowsTheMovementVector)
{
	// Each direction's own tile step maps back to it (engine/freeform_movement.hpp TileStep: x = tile x, y = tile y).
	const std::pair<Direction, std::pair<float, float>> cases[] = {
		{ Direction::South, { 1.0F, 1.0F } }, { Direction::SouthWest, { 0.0F, 1.0F } }, { Direction::West, { -1.0F, 1.0F } },
		{ Direction::NorthWest, { -1.0F, 0.0F } }, { Direction::North, { -1.0F, -1.0F } }, { Direction::NorthEast, { 0.0F, -1.0F } },
		{ Direction::East, { 1.0F, -1.0F } }, { Direction::SouthEast, { 1.0F, 0.0F } },
	};
	for (const auto &[dir, v] : cases)
		EXPECT_EQ(StepDirectionFor(v.first, v.second), dir) << static_cast<int>(dir);
	// Slightly off-axis still picks the nearest step.
	EXPECT_EQ(StepDirectionFor(0.2F, 1.0F), Direction::SouthWest);
}

/** A render-stage-only provider like the VR head provider: it gets the first-person gameplay stage by default. */
class HeadsetView final : public render3d::ViewProvider {
public:
	[[nodiscard]] const char *Name() const override { return "headset"; }
	bool BeginFrame(const CameraState &camera) override
	{
		seen = camera;
		return true;
	}
	[[nodiscard]] int EyeCount() const override { return 2; }
	CameraState seen;
};

TEST(FpvCameraTest, OneProviderInterfaceForCamerasAndHeadsets)
{
	HeadsetView vr;
	EXPECT_EQ(render3d::GetViewProvider(), nullptr);
	render3d::SetViewProvider(&vr);
	EXPECT_EQ(render3d::GetViewProvider(), &vr);
	ViewInputs in = BaseInputs();
	in.headBob = false;
	const ViewOutput out = render3d::GetViewProvider()->Update(in);
	EXPECT_NEAR(out.camera.y, kEyeHeight, Eps); // default gameplay stage: the first-person eye
	EXPECT_TRUE(vr.BeginFrame(out.camera));
	EXPECT_EQ(vr.EyeCount(), 2);
	render3d::SetViewProvider(nullptr);
	EXPECT_EQ(render3d::GetViewProvider(), nullptr);
	// The built-in cameras keep the flat render camera.
	ThirdPersonView third;
	EXPECT_FALSE(third.BeginFrame(out.camera));
	EXPECT_EQ(third.EyeCount(), 0);
}

} // namespace
} // namespace devilution::fpv

namespace devilution::fpv {
namespace {

diablo_hud::WindowRecord HudWindow(std::size_t id)
{
	diablo_hud::WindowRecord w;
	w.id = id;
	w.native = { { 0, 0 }, { 100, 100 } };
	w.visual = { { 10, 10 }, { 100, 100 } };
	w.body = { { 0, 10 }, { 100, 90 } };
	return w;
}

TEST(FpvHudBridgeTest, MouselookKeepsTheHudOffThePointer)
{
	const std::vector<diablo_hud::WindowRecord> windows { HudWindow(7) };
	EXPECT_EQ(HudOwnerAt({ 50, 50 }, windows, false).kind, diablo_hud::OwnerKind::WindowBody);
	EXPECT_FALSE(HudOwnerAt({ 50, 50 }, windows, true).owns());
	// A modal still wins over the captured world.
	EXPECT_EQ(HudOwnerAt({ 50, 50 }, windows, true, true).kind, diablo_hud::OwnerKind::Modal);
}

TEST(FpvHudBridgeTest, MouselookEdgesCancelHudButtons)
{
	HudCaptureTracker tracker;
	diablo_hud::InputPairs pairs;
	pairs.own(1);
	EXPECT_FALSE(tracker.update(false, pairs));
	EXPECT_TRUE(pairs.owns(1));
	EXPECT_TRUE(tracker.update(true, pairs));
	EXPECT_FALSE(pairs.owns(1));
	EXPECT_EQ(tracker.generation(), 1U);
	pairs.own(3);
	EXPECT_TRUE(tracker.update(false, pairs));
	EXPECT_EQ(pairs.mask(), 0U);
}

} // namespace
} // namespace devilution::fpv
