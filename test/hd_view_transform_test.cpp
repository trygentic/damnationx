#include "engine/render/hd_view_transform.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "engine/render/hd_sprites_core.hpp"

namespace devilution {
namespace {

/** scrollrt.cpp Zoom() on a plain buffer (same loop structure: doubles the top-left quarter in place, right to left). */
void ReferenceZoom(std::vector<uint8_t> &buffer, int pitch, int height, int viewportWidth, int viewportOffsetX)
{
	const int srcWidth = (viewportWidth + 1) / 2;
	const int doubleableWidth = viewportWidth / 2;
	const int srcHeight = (height + 1) / 2;
	const int doubleableHeight = height / 2;
	uint8_t *src = buffer.data() + static_cast<size_t>(srcHeight - 1) * pitch + (srcWidth - 1);
	uint8_t *dst = buffer.data() + static_cast<size_t>(height - 1) * pitch + (viewportOffsetX + viewportWidth - 1);
	const bool oddViewportWidth = (viewportWidth % 2) == 1;
	const auto doubleLine = [&]() {
		for (int i = 0; i < doubleableWidth; i++) {
			*dst-- = *src;
			*dst-- = *src;
			--src;
		}
		if (oddViewportWidth) {
			*dst-- = *src;
			--src;
		}
	};
	for (int hgt = 0; hgt < doubleableHeight; hgt++) {
		doubleLine();
		src -= (pitch - srcWidth);
		std::copy_n(dst + 1, viewportWidth, dst - pitch + 1);
		dst -= 2 * pitch - viewportWidth;
	}
	if ((height % 2) == 1)
		doubleLine();
}

std::vector<uint8_t> Pattern(int pitch, int height)
{
	std::vector<uint8_t> buffer(static_cast<size_t>(pitch) * height);
	for (size_t i = 0; i < buffer.size(); ++i)
		buffer[i] = static_cast<uint8_t>((i * 37 + i / pitch * 11) % 251 + 1);
	return buffer;
}

TEST(HdWorldTransform, UnzoomedIsTheIdentity)
{
	const HdWorldTransform transform = ComputeHdWorldTransform(4, 640, 352, 320, 320);
	EXPECT_TRUE(transform.IsIdentity());
	EXPECT_EQ(transform.Scale(), 1);
	EXPECT_EQ(transform.viewport.width, 640);
	EXPECT_EQ(transform.viewport.height, 352);
	EXPECT_EQ(transform.world.width, 640);
	EXPECT_FLOAT_EQ(transform.ToScreenX(12.5F), 12.5F);
	EXPECT_EQ(transform.WorldX(77), 77);
}

TEST(HdWorldTransform, ZoomedEvenViewport)
{
	const HdWorldTransform transform = ComputeHdWorldTransform(8, 640, 352, 640, 0);
	EXPECT_FALSE(transform.IsIdentity());
	EXPECT_EQ(transform.Scale(), 2);
	EXPECT_EQ(transform.offsetX, 0);
	EXPECT_EQ(transform.offsetY, 0);
	EXPECT_EQ(transform.world.width, 320);
	EXPECT_EQ(transform.world.height, 176);
	EXPECT_FLOAT_EQ(transform.ToScreenX(10.25F), 20.5F);
	EXPECT_FLOAT_EQ(transform.ToScreenLength(49.5F), 99.0F);
	EXPECT_EQ(transform.WorldX(21), 10);
	EXPECT_EQ(transform.WorldY(351), 175);
}

TEST(HdWorldTransform, ZoomedOddViewportAndLeftPanel)
{
	// Left panel open: the viewport starts at the panel width; odd sizes give the first world column/row one pixel.
	const HdWorldTransform transform = ComputeHdWorldTransform(8, 800, 351, 481, 319);
	EXPECT_EQ(transform.viewport.x, 319);
	EXPECT_EQ(transform.viewport.width, 481);
	EXPECT_EQ(transform.offsetX, 318);
	EXPECT_EQ(transform.offsetY, -1);
	EXPECT_EQ(transform.world.width, 241);
	EXPECT_EQ(transform.world.height, 176);
	EXPECT_EQ(transform.WorldX(319), 0);
	EXPECT_EQ(transform.WorldX(320), 1);
	EXPECT_EQ(transform.WorldX(321), 1);
	EXPECT_EQ(transform.WorldY(0), 0);
	EXPECT_EQ(transform.WorldY(1), 1);
	EXPECT_EQ(transform.WorldY(2), 1);
}

TEST(HdWorldTransform, RectanglesClipToTheViewport)
{
	const HdWorldTransform transform = ComputeHdWorldTransform(8, 640, 351, 640, 0);
	const HdRect inside = transform.ToScreen({ 10, 20, 5, 6 });
	EXPECT_EQ(inside.x, 20);
	EXPECT_EQ(inside.y, 39);
	EXPECT_EQ(inside.width, 10);
	EXPECT_EQ(inside.height, 12);
	const HdRect corner = transform.ToScreen({ -3, -3, 5, 5 });
	EXPECT_EQ(corner.x, 0);
	EXPECT_EQ(corner.y, 0);
	EXPECT_EQ(corner.width, 4);
	EXPECT_EQ(corner.height, 3);
	EXPECT_TRUE(transform.ToScreen({ 400, 10, 20, 20 }).empty());
}

/** HdZoomGrid must reproduce Zoom() pixel for pixel for every parity and panel offset. */
TEST(HdWorldTransform, ZoomGridMatchesZoom)
{
	constexpr int Pitch = 37;
	constexpr int Height = 23;
	for (const int viewportWidth : { 36, 35, 20, 21 }) {
		for (const int height : { Height, Height - 1 }) {
			for (const int offset : { 0, Pitch - viewportWidth }) {
				std::vector<uint8_t> zoomed = Pattern(Pitch, Height);
				const std::vector<uint8_t> world = zoomed;
				ReferenceZoom(zoomed, Pitch, height, viewportWidth, offset);
				const HdWorldTransform transform = ComputeHdWorldTransform(8, Pitch, height, viewportWidth, offset);
				std::vector<uint8_t> grid(world.size(), 0);
				HdZoomGrid(world.data(), Pitch, grid.data(), Pitch, Pitch, height, transform, uint8_t { 0 }, [](uint8_t v) { return v; });
				for (int y = 0; y < height; ++y) {
					for (int x = offset; x < offset + viewportWidth; ++x) {
						ASSERT_EQ(grid[static_cast<size_t>(y) * Pitch + x], zoomed[static_cast<size_t>(y) * Pitch + x])
						    << "w " << viewportWidth << " h " << height << " offset " << offset << " at " << x << "," << y;
					}
					for (int x = 0; x < offset; ++x)
						ASSERT_EQ(grid[static_cast<size_t>(y) * Pitch + x], 0);
				}
			}
		}
	}
}

/** HdUpscaleInPlace is Zoom() at 2x and follows the transform (HdZoomGrid of the untouched world) at every level. */
TEST(HdWorldTransform, UpscaleInPlaceMatchesTheTransform)
{
	constexpr int Pitch = 37;
	constexpr int Height = 23;
	// Zoom in quarters: whole levels and the fractional ones (1.25x .. 3.75x).
	for (const int quarters : { 8, 12, 16, 5, 6, 7, 9, 10, 15 }) {
		for (const int viewportWidth : { 36, 35, 20, 21, 22 }) {
			for (const int height : { Height, Height - 1, Height - 2 }) {
				for (const int offset : { 0, Pitch - viewportWidth }) {
					std::vector<uint8_t> zoomed = Pattern(Pitch, Height);
					const std::vector<uint8_t> world = zoomed;
					const HdWorldTransform transform = ComputeHdWorldTransform(quarters, Pitch, height, viewportWidth, offset);
					HdUpscaleInPlace(zoomed.data(), Pitch, transform);
					std::vector<uint8_t> grid(world.size(), 0);
					HdZoomGrid(world.data(), Pitch, grid.data(), Pitch, Pitch, height, transform, uint8_t { 0 }, [](uint8_t v) { return v; });
					if (quarters == 8) {
						std::vector<uint8_t> reference = world;
						ReferenceZoom(reference, Pitch, height, viewportWidth, offset);
						ASSERT_EQ(zoomed, reference) << "w " << viewportWidth << " h " << height << " offset " << offset;
					}
					for (int y = 0; y < height; ++y) {
						for (int x = offset; x < offset + viewportWidth; ++x) {
							ASSERT_EQ(zoomed[static_cast<size_t>(y) * Pitch + x], grid[static_cast<size_t>(y) * Pitch + x])
							    << "quarters " << quarters << " w " << viewportWidth << " h " << height << " offset " << offset << " at " << x << "," << y;
						}
					}
					// The world exactly fills the viewport: the last world column / row ends on its far edge, and
					// every world column after the first covers floor(k) or ceil(k) screen pixels.
					EXPECT_EQ(transform.world.width, (viewportWidth * 4 + quarters - 1) / quarters);
					EXPECT_EQ(transform.ToScreenX(transform.world.width), offset + viewportWidth);
					EXPECT_EQ(transform.ToScreenY(transform.world.height), height);
					EXPECT_LE(transform.offsetX, offset);
					for (int p = 1; p < transform.world.width; ++p) {
						const int width = transform.ToScreenX(p + 1) - transform.ToScreenX(p);
						EXPECT_TRUE(width == quarters / 4 || width == (quarters + 3) / 4) << quarters << " column " << p;
						EXPECT_EQ(transform.WorldX(transform.ToScreenX(p)), p);
						EXPECT_EQ(transform.WorldX(transform.ToScreenX(p) - 1), p - 1);
					}
				}
			}
		}
	}
}

/** Positions follow the pixels: the screen edge of a world pixel is where Zoom() put that pixel's first copy. */
TEST(HdWorldTransform, EdgesFollowTheZoomedPixels)
{
	const HdWorldTransform transform = ComputeHdWorldTransform(8, 101, 61, 101, 0);
	for (int worldX = 1; worldX < transform.world.width; ++worldX) {
		const int screenX = transform.ToScreenX(worldX);
		EXPECT_EQ(transform.WorldX(screenX), worldX);
		EXPECT_EQ(transform.WorldX(screenX - 1), worldX - 1);
	}
}

class ZoomOcclusionTest : public ::testing::Test {
protected:
	static constexpr int Pitch = 64;
	static constexpr int Height = 40;
	std::vector<uint8_t> buffer = std::vector<uint8_t>(static_cast<size_t>(Pitch) * Height, 7);
	hd_sprites::OcclusionTracker tracker;

	void SetUp() override { tracker.reset(Pitch, Height); }
	uint8_t &At(int x, int y) { return buffer[static_cast<size_t>(y) * Pitch + x]; }
	uint16_t Plane(int x, int y) const { return tracker.plane()[static_cast<size_t>(y) * Pitch + x]; }
};

TEST_F(ZoomOcclusionTest, RemappedSnapshotsSeeOnlyLaterWrites)
{
	// Floor pattern in the world pass, one instance recorded over it, then a wall pixel drawn after it.
	for (int y = 0; y < Height / 2; ++y)
		for (int x = 0; x < Pitch / 2; ++x)
			At(x, y) = static_cast<uint8_t>(1 + (x + y) % 5);
	ASSERT_EQ(tracker.record(buffer.data(), Pitch, { 4, 3, 10, 8 }), 0);
	At(6, 5) = 200; // drawn after the instance (in front of it)
	const HdWorldTransform transform = ComputeHdWorldTransform(8, Pitch, Height, Pitch, 0);
	ReferenceZoom(buffer, Pitch, Height, Pitch, 0);
	tracker.remap(transform);
	EXPECT_EQ(tracker.rect(0).x, 8);
	EXPECT_EQ(tracker.rect(0).y, 6);
	EXPECT_EQ(tracker.rect(0).width, 20);
	EXPECT_EQ(tracker.rect(0).height, 16);
	At(30, 20) = 99; // UI drawn after the zoom (screen pixels)
	At(25, 18) = 99;
	hd_sprites::PixelRect bounds;
	tracker.resolve(buffer.data(), Pitch, bounds);
	EXPECT_TRUE(tracker.visible(0));
	EXPECT_EQ(bounds.x, 8);
	EXPECT_EQ(bounds.width, 20);
	for (int y = 6; y < 22; ++y) {
		for (int x = 8; x < 28; ++x) {
			const bool wall = x >= 12 && x < 14 && y >= 10 && y < 12;
			const bool ui = x == 25 && y == 18;
			EXPECT_EQ(Plane(x, y), (wall || ui) ? 1 : 0) << x << "," << y;
		}
	}
}

TEST_F(ZoomOcclusionTest, InstancesOutsideTheZoomedViewDisappear)
{
	// Recorded in world pixels that Zoom() never shows (right of the source quarter).
	ASSERT_EQ(tracker.record(buffer.data(), Pitch, { 40, 2, 10, 10 }), 0);
	ASSERT_EQ(tracker.record(buffer.data(), Pitch, { 2, 2, 4, 4 }), 1);
	tracker.remap(ComputeHdWorldTransform(8, Pitch, Height, Pitch, 0));
	EXPECT_TRUE(tracker.rect(0).empty());
	hd_sprites::PixelRect bounds;
	tracker.resolve(buffer.data(), Pitch, bounds);
	EXPECT_FALSE(tracker.visible(0));
	EXPECT_TRUE(tracker.visible(1));
	EXPECT_EQ(bounds.x, 4);
	EXPECT_EQ(bounds.width, 8);
}

TEST_F(ZoomOcclusionTest, IdentityRemapChangesNothing)
{
	ASSERT_EQ(tracker.record(buffer.data(), Pitch, { 3, 4, 5, 6 }), 0);
	tracker.remap(ComputeHdWorldTransform(4, Pitch, Height, Pitch, 0));
	EXPECT_EQ(tracker.rect(0).x, 3);
	EXPECT_EQ(tracker.rect(0).width, 5);
}

TEST(HdTextureDivisor, FollowsCanvasTimesZoom)
{
	EXPECT_EQ(hd_sprites::SelectTextureDivisor(1.0F), 4);
	EXPECT_EQ(hd_sprites::SelectTextureDivisor(1.0F * 2.0F), 2); // 1:1 canvas, zoomed
	EXPECT_EQ(hd_sprites::SelectTextureDivisor(1.5F), 2);
	EXPECT_EQ(hd_sprites::SelectTextureDivisor(1.5F * 2.0F), 1);
	EXPECT_EQ(hd_sprites::SelectTextureDivisor(2.0F * 2.0F), 1);
}

} // namespace
} // namespace devilution
