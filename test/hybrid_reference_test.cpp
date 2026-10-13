#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "engine/render/hybrid_reference.hpp"

// Executable goldens for Source/engine/render/webgpu_shaders.hpp via its C++ transliteration.
// Fixtures: test/fixtures/hybrid/{volume_synthetic.bin,planes_synthetic.bin}, layout documented in test/fixtures/hybrid/README.md.

namespace devilution {
namespace {

using namespace hybrid_reference;
constexpr float CellPickOffsetForTests = 0.35F; // CELL_PICK_OFFSET

// ---- volume bits ----
constexpr uint32_t VBlock = 1U << 10;
constexpr uint32_t VVisible = 1U << 12;
constexpr uint32_t VExplored = 1U << 13;
constexpr uint32_t VArch = 1U << 14;
constexpr uint32_t VDoorOpen = 1U << 15;
constexpr uint32_t DLightShift = 17;
constexpr uint32_t LintelShift = 21;

// ---- gbuffer kinds, aux byte = kind << 5 | normal ----
constexpr uint32_t KUnlit = 0, KFloor = 1, KWallL = 2, KWallR = 3, KCard = 4, KCardEmissive = 5, KWallBlended = 6, KWallTop = 7;
constexpr uint32_t Aux(uint32_t kind, uint32_t normal) { return (kind << 5) | normal; }

// ---- fixture geometry (README.md): origin (0, -256), classic 2:1 affine, 128x96 planes, 80 viewport rows ----
constexpr int PlanesW = 128, PlanesH = 96, PlanesPitch = 128, ViewportRows = 80;

std::vector<uint8_t> ReadFixture(const char *name)
{
	const std::array<std::string, 3> candidates {
		std::string("test/fixtures/hybrid/") + name,
		std::string("../test/fixtures/hybrid/") + name,
		std::string("../../test/fixtures/hybrid/") + name,
	};
	for (const std::string &path : candidates) {
		std::ifstream in(path, std::ios::binary);
		if (!in) continue;
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
	return {};
}

struct FixtureVolume {
	std::vector<uint32_t> cells;
	FixtureVolume()
	{
		const std::vector<uint8_t> bytes = ReadFixture("volume_synthetic.bin");
		cells.resize(bytes.size() / 4);
		for (size_t i = 0; i < cells.size(); i++)
			cells[i] = static_cast<uint32_t>(bytes[i * 4]) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | (static_cast<uint32_t>(bytes[i * 4 + 2]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 3]) << 24);
	}
	[[nodiscard]] Volume span() const { return Volume(cells.data(), cells.size()); }
	[[nodiscard]] uint32_t &at(int x, int y) { return cells[static_cast<size_t>(y) * 112 + x]; }
};

struct FixturePlanes {
	std::vector<uint8_t> bytes;
	Planes planes;
	uint32_t viewportHeight = 0;
	FixturePlanes()
	{
		bytes = ReadFixture("planes_synthetic.bin");
		if (bytes.size() < 12 || std::memcmp(bytes.data(), "HYBP", 4) != 0) return;
		const auto u16 = [&](size_t off) { return static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8); };
		planes.pitch = u16(4);
		planes.width = u16(6);
		planes.height = u16(8);
		viewportHeight = u16(10);
		const size_t planeBytes = static_cast<size_t>(planes.pitch) * planes.height;
		if (bytes.size() < 12 + 3 * planeBytes) return;
		planes.aux = bytes.data() + 12;
		planes.base = planes.aux + planeBytes;
		planes.index = planes.base + planeBytes;
	}
};

const FixtureVolume &TheVolume()
{
	static const FixtureVolume volume;
	return volume;
}
const FixturePlanes &ThePlanes()
{
	static const FixturePlanes planes;
	return planes;
}

// The classic 2:1 dimetric affine (dx.cpp:491-516): basis +x = (32, 16), +y = (-32, 16) screen px per tile.
Params BaseParams(float originX, float originY)
{
	Params p;
	p.originX = originX;
	p.originY = originY;
	p.xx = 1.0F / 64.0F;
	p.xy = 1.0F / 32.0F;
	p.yx = -1.0F / 64.0F;
	p.yy = 1.0F / 32.0F;
	p.flags = 8; // hybrid, hard shadows
	return p;
}
Params SceneParams() { return BaseParams(0.0F, 0.0F); }
Params FixtureParams()
{
	Params p = BaseParams(0.0F, -256.0F);
	p.viewportHeight = ViewportRows;
	p.maskScale = 2;
	return p;
}

using LutArray = std::array<std::array<float, 4>, 32>;
const LutArray &TestLut()
{
	static const LutArray lut = [] {
		LutArray l {};
		for (auto &e : l) e = { 0.0F, 0.0F, 1.0F, 0.0F };
		l[1] = { 0.0F, 1.0F, 0.0F, 0.0F };          // WallLeft: tile +y
		l[2] = { 1.0F, 0.0F, 0.0F, 0.0F };          // WallRight: tile +x
		l[3] = { 0.6124F, 0.6124F, 0.5F, 0.0F };    // flat card, tilted toward the camera
		l[4] = { 0.7071F, -0.7071F, 0.0F, 0.0F };   // some tilted card normal
		return l;
	}();
	return lut;
}
Lut TestLutSpan() { return Lut(TestLut()); }

Light MakeLight(float x, float y, float z, float radius = 4.0F, float intensity = 1.0F, Rgb color = { 1.0F, 1.0F, 1.0F })
{
	Light l;
	l.position = { x, y, z };
	l.radius = radius;
	l.intensity = intensity;
	l.color = color;
	return l;
}

Point FloorPoint(Vec2 v) { return { static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)) }; }

// Receiver of a foot-table SW-face wall pixel: tile-local column c (0..31), hRows above the foot line.
struct WallReceiver {
	Vec2 ground;
	Point skipA, own;
	Vec3 p;
};
WallReceiver SwFaceReceiver(Point tile, int c, int hRows, const Params &params)
{
	const int leftX = static_cast<int>(params.originX) + 32 * (tile.x - tile.y) - 32; // diamond left vertex
	const int anchorRow = static_cast<int>(params.originY) + 16 * (tile.x + tile.y) + 31; // bottom row of the diamond
	const int foot = (31 - c) / 2;
	const int y = anchorRow - foot - hRows;                       // pixel row of the receiver
	const Vec2 floorScreen { static_cast<float>(leftX + c), static_cast<float>(y + hRows) };
	WallReceiver r;
	r.ground = ScreenToWorld(floorScreen, params);
	const Vec2 nudge { 0.0F, params.receiverNudge };             // n = LUT[1] = (0, 1, 0)
	r.skipA = FloorPoint({ r.ground.x + nudge.x, r.ground.y + nudge.y });
	r.own = FloorPoint({ r.ground.x - nudge.x, r.ground.y - nudge.y });
	r.p = { r.ground.x + nudge.x, r.ground.y + nudge.y, static_cast<float>(hRows) / params.heightPixelsPerWorldUnit };
	return r;
}

float MaxRgb(Rgb c) { return std::max(c.r, std::max(c.g, c.b)); }
bool RgbNear(Rgb a, Rgb b, float eps = 1e-5F) { return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps && std::fabs(a.b - b.b) <= eps; }
constexpr uint32_t AllLights = ~0U; // AccumulateMasked without culling: the culled pass must match it

// ============================================================= fixtures load =============================================================

TEST(HybridReference, FixturesLoad)
{
	ASSERT_EQ(TheVolume().cells.size(), 112U * 112U) << "volume_synthetic.bin must hold 112*112 little-endian u32 cells";
	ASSERT_NE(ThePlanes().planes.aux, nullptr) << "planes_synthetic.bin missing or malformed";
	EXPECT_EQ(ThePlanes().planes.width, static_cast<uint32_t>(PlanesW));
	EXPECT_EQ(ThePlanes().planes.height, static_cast<uint32_t>(PlanesH));
	EXPECT_EQ(ThePlanes().planes.pitch, static_cast<uint32_t>(PlanesPitch));
	EXPECT_EQ(ThePlanes().viewportHeight, static_cast<uint32_t>(ViewportRows));
	// The scene cells the tests rely on.
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	EXPECT_EQ(CellRaw({ 5, 5 }, v, p) & 255U, 64U);
	EXPECT_NE(CellRaw({ 5, 5 }, v, p) & VBlock, 0U);
	EXPECT_EQ(CellRaw({ 5, 9 }, v, p) & 255U, 160U);
	EXPECT_NE(CellRaw({ 20, 20 }, v, p) & VArch, 0U);
	EXPECT_EQ(CellRaw({ 20, 20 }, v, p) & VBlock, 0U) << "the arch cell is walkable (BlockLight clear)";
	EXPECT_EQ((CellRaw({ 20, 20 }, v, p) >> LintelShift) & 127U, 48U) << "lintel 96 px stored / 2";
	EXPECT_EQ(CellRaw({ 40, 40 }, v, p) & VExplored, 0U);
	EXPECT_EQ(CellRaw({ 44, 40 }, v, p) & VExplored, 0U);
	EXPECT_EQ((CellRaw({ 65, 50 }, v, p) >> DLightShift) & 15U, 15U);
	EXPECT_EQ((CellRaw({ 66, 50 }, v, p) >> DLightShift) & 15U, 0U) << "dLight 15 next to a lit cell";
	EXPECT_NE(CellRaw({ 34, 30 }, v, p) & VDoorOpen, 0U);
}

// ============================================================== prelude ==================================================================

TEST(HybridReference, PreludeKinds)
{
	EXPECT_TRUE(IsWallKind(KWallL));
	EXPECT_TRUE(IsWallKind(KWallR));
	EXPECT_TRUE(IsWallKind(KWallBlended));
	EXPECT_FALSE(IsWallKind(KWallTop));
	EXPECT_FALSE(IsWallKind(KCard));
	EXPECT_TRUE(HasHeight(KWallTop));
	EXPECT_TRUE(HasHeight(KCard));
	EXPECT_FALSE(HasHeight(KFloor));
	EXPECT_FALSE(HasHeight(KUnlit));
	EXPECT_TRUE(PassThrough(KUnlit));
	EXPECT_TRUE(PassThrough(KCardEmissive));
	EXPECT_FALSE(PassThrough(KFloor));
}

TEST(HybridReference, HeightRowsWrapsLikeU32)
{
	EXPECT_EQ(HeightRows(KFloor, 200, 10, 0), 0U) << "no height for floors";
	EXPECT_EQ(HeightRows(KWallL, 47, 40, 0), 7U);
	EXPECT_EQ(HeightRows(KWallL, 5, 10, 0), 251U) << "(b - y) & 255 wraps";
	EXPECT_EQ(HeightRows(KCard, 71, 61, 0), 10U);
	EXPECT_EQ(HeightRows(KWallL, 37, 60, 8), 7U) << "under zoom the row is halved before the subtraction";
}

TEST(HybridReference, BaseTexelAtZoomAndRobustAccess)
{
	const Planes &planes = ThePlanes().planes;
	Params p = FixtureParams();
	// Unzoomed: wall (9,8) SW column c = 10 has foot 10, B = 37 for every row of the column.
	EXPECT_EQ(BaseTexelAt(planes, { 10, 30 }, p), 37U);
	EXPECT_EQ(AuxTexelAt(planes, { 10, 30 }), Aux(KWallL, 1));
	// Zoomed with a side-panel offset: output (28, 60) reads the half-size B at ((28 - 8) / 2, 60 / 2) = (10, 30).
	p.zoom = 8;
	p.zoomOffsetX = 8;
	EXPECT_EQ(BaseTexelAt(planes, { 28, 60 }, p), 37U);
	EXPECT_EQ(HeightRows(KWallL, 37, 60, 8), 7U);
	// 3x: output (38, 90) reads ((38 - 8) / 3, 90 / 3) = (10, 30).
	p.zoom = 12;
	EXPECT_EQ(BaseTexelAt(planes, { 38, 90 }, p), 37U);
	EXPECT_EQ(HeightRows(KWallL, 37, 90, 12), 7U);
	// 1.25x with a 1-row remainder (offsetY = -1): output (8 + 13, 37) reads (13 * 4 / 5, (37 + 1) * 4 / 5) = (10, 30).
	p.zoom = 5 | (1 << 8);
	EXPECT_EQ(BaseTexelAt(planes, { 21, 37 }, p), 37U);
	EXPECT_EQ(HeightRows(KWallL, 37, 37, p.zoom), 7U);
	p.zoom = 8;
	// Left of the zoom offset the shifted x is negative: robust access returns 0.
	EXPECT_EQ(BaseTexelAt(planes, { 4, 60 }, p), 0U);
	// Out-of-range plane reads are 0 (WebGPU robust access), also for aux / index.
	p.zoom = 4;
	EXPECT_EQ(BaseTexelAt(planes, { -1, 0 }, p), 0U);
	EXPECT_EQ(BaseTexelAt(planes, { PlanesW, 0 }, p), 0U);
	EXPECT_EQ(AuxTexelAt(planes, { 0, -1 }), 0U);
	EXPECT_EQ(AuxTexelAt(planes, { 0, PlanesH }), 0U);
	EXPECT_EQ(IndexTexelAt(planes, { -5, 5 }), 0U);
	EXPECT_EQ(IndexTexelAt(planes, { 3, 4 }), static_cast<uint32_t>((3 * 7 + 4 * 13) & 255));
}

TEST(HybridReference, ScreenToWorldMatchesClassicAffine)
{
	const Params p = FixtureParams();
	// The top vertex of cell (9,8) is at screen (32, 16) with origin (0, -256): 32 * (9 - 8) = 32, 16 * 17 - 256 = 16.
	const Vec2 w = ScreenToWorld({ 32.0F, 16.0F }, p);
	EXPECT_FLOAT_EQ(w.x, 9.0F);
	EXPECT_FLOAT_EQ(w.y, 8.0F);
	// One tile to the SE is +32, +16 px.
	const Vec2 se = ScreenToWorld({ 64.0F, 32.0F }, p);
	EXPECT_FLOAT_EQ(se.x, 10.0F);
	EXPECT_FLOAT_EQ(se.y, 8.0F);
}

// ============================================================ visibility ================================================================

TEST(HybridReference, UnobstructedIsVisible)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	EXPECT_FLOAT_EQ(Visibility3D({ 2.5F, 2.5F, 0.0F }, { 8.5F, 3.5F, 0.5F }, { 2, 2 }, { 8, 3 }, v, p), 1.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 60.5F, 60.5F, 0.0F }, { 60.5F, 60.5F, 0.5F }, { 60, 60 }, { 60, 60 }, v, p), 1.0F) << "zero-length xy ray";
}

TEST(HybridReference, WallOfHeightTwoBlocksLowLightAndPassesHighLight)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	// Wall of height 2 units at (5,5); receiver south of it, light north of it.
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 6.5F, 0.0F }, { 5.5F, 4.5F, 0.5F }, { 5, 6 }, { 5, 4 }, v, p), 0.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 6.5F, 3.0F }, { 5.5F, 4.5F, 3.0F }, { 5, 6 }, { 5, 4 }, v, p), 1.0F) << "light z 3 clears a 2-unit wall";
	// Wall of height 5 units at (5,9): the same z 3 light is blocked.
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 10.5F, 3.0F }, { 5.5F, 8.5F, 3.0F }, { 5, 10 }, { 5, 8 }, v, p), 0.0F);
	// A ray that starts low and rises over the 2-unit wall: zMin inside (5,5) is below 2 -> blocked.
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 6.5F, 0.0F }, { 5.5F, 4.5F, 3.0F }, { 5, 6 }, { 5, 4 }, v, p), 0.0F);
}

TEST(HybridReference, WallBehindBlocksEverySwColumn)
{
	// Wall of height 2 at (5,5), light z 0.5 in (5,4) -> visibility 0 for every column c in [0,32) and heights 0..40 rows;
	// the same light in (5,6) -> 1 for all of them (no per-column stripes).
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	const Vec3 behind { 5.5F, 4.5F, 0.5F };
	const Vec3 front { 5.5F, 6.5F, 0.5F };
	for (int c = 0; c < 32; c++) {
		for (int h = 0; h <= 40; h++) {
			const WallReceiver r = SwFaceReceiver({ 5, 5 }, c, h, p);
			ASSERT_EQ(r.skipA, (Point { 5, 6 })) << "column " << c << ": the nudged march start is consistently the cell in front of the face";
			ASSERT_EQ(r.own, (Point { 5, 5 })) << "column " << c << ": own is consistently the wall's own cell";
			EXPECT_FLOAT_EQ(Visibility3D(r.p, behind, r.skipA, FloorPoint({ behind.x, behind.y }), v, p), 0.0F) << "c=" << c << " h=" << h;
			EXPECT_FLOAT_EQ(Visibility3D(r.p, front, r.skipA, FloorPoint({ front.x, front.y }), v, p), 1.0F) << "c=" << c << " h=" << h;
		}
	}
	// The ground point itself alternates between v = 1.0 (even columns) and 63/64 (odd columns) - the reason for the nudge rule.
	EXPECT_FLOAT_EQ(SwFaceReceiver({ 5, 5 }, 0, 0, p).ground.y, 6.0F);
	EXPECT_FLOAT_EQ(SwFaceReceiver({ 5, 5 }, 1, 0, p).ground.y, 5.0F + 63.0F / 64.0F);
}

TEST(HybridReference, ArchPassesUnderLintelAndBlocksAboveInWalkableCell)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	ASSERT_EQ(CellRaw({ 20, 20 }, v, p) & VBlock, 0U) << "walkable arch cell";
	const Point skipA { 20, 22 };
	const Point skipB { 20, 18 };
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 0.5F }, { 20.5F, 18.5F, 0.5F }, skipA, skipB, v, p), 1.0F) << "under the lintel (3 units)";
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 2.9F }, { 20.5F, 18.5F, 2.9F }, skipA, skipB, v, p), 1.0F) << "just under the lintel";
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 4.0F }, { 20.5F, 18.5F, 4.0F }, skipA, skipB, v, p), 0.0F) << "above the lintel, below the height (5 units)";
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 6.0F }, { 20.5F, 18.5F, 6.0F }, skipA, skipB, v, p), 1.0F) << "above the arch";
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 0.5F }, { 20.5F, 18.5F, 4.5F }, skipA, skipB, v, p), 1.0F) << "z inside the arch cell is [2.0, 3.0]: zMax == lintel does not block";
	EXPECT_FLOAT_EQ(Visibility3D({ 20.5F, 22.5F, 0.5F }, { 20.5F, 18.5F, 5.5F }, skipA, skipB, v, p), 0.0F) << "z inside the arch cell is [2.375, 3.625]: rises above the lintel while below the height";
}

TEST(HybridReference, DoorToggleFollowsBlockLight)
{
	const Params p = SceneParams();
	FixtureVolume volume = TheVolume();
	EXPECT_FLOAT_EQ(Visibility3D({ 30.5F, 32.5F, 0.5F }, { 30.5F, 28.5F, 0.5F }, { 30, 32 }, { 30, 28 }, volume.span(), p), 0.0F) << "closed door";
	EXPECT_FLOAT_EQ(Visibility3D({ 34.5F, 32.5F, 0.5F }, { 34.5F, 28.5F, 0.5F }, { 34, 32 }, { 34, 28 }, volume.span(), p), 1.0F) << "open door";
	// The volume is rebuilt from the changed dPiece: BlockLight clears, the same ray passes.
	volume.at(30, 30) = (volume.at(30, 30) & ~VBlock) | VDoorOpen;
	EXPECT_FLOAT_EQ(Visibility3D({ 30.5F, 32.5F, 0.5F }, { 30.5F, 28.5F, 0.5F }, { 30, 32 }, { 30, 28 }, volume.span(), p), 1.0F);
	// ... and closing it again blocks (height still 64 px = 2 units).
	volume.at(30, 30) = (volume.at(30, 30) | VBlock) & ~VDoorOpen;
	EXPECT_FLOAT_EQ(Visibility3D({ 30.5F, 32.5F, 0.5F }, { 30.5F, 28.5F, 0.5F }, { 30, 32 }, { 30, 28 }, volume.span(), p), 0.0F);
}

TEST(HybridReference, OutOfGridBlocks)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	EXPECT_EQ(CellRaw({ -1, 5 }, v, p), VBlock | 255U);
	EXPECT_EQ(CellRaw({ 5, 112 }, v, p), VBlock | 255U);
	EXPECT_FLOAT_EQ(Visibility3D({ 2.5F, 5.5F, 0.5F }, { -1.5F, 5.5F, 0.5F }, { 2, 5 }, { -2, 5 }, v, p), 0.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 2.5F, 5.5F, 5.0F }, { -1.5F, 5.5F, 5.0F }, { 2, 5 }, { -2, 5 }, v, p), 0.0F) << "tall: 255 px = 7.97 units";
	EXPECT_FLOAT_EQ(Visibility3D({ 2.5F, 5.5F, 9.0F }, { -1.5F, 5.5F, 9.0F }, { 2, 5 }, { -2, 5 }, v, p), 1.0F) << "the sentinel is 255 px tall, not infinite";
}

TEST(HybridReference, UnexploredCellBlocksByBlockLightOnly)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	// (40,40): unexplored, BlockLight set, height 1 unit, hidden arch bits: blocks regardless of height, never exposes the arch.
	const uint32_t march = CellForMarch({ 40, 40 }, v, p);
	EXPECT_EQ(march & 255U, 255U);
	EXPECT_EQ(march & VArch, 0U);
	EXPECT_NE(march & VBlock, 0U);
	EXPECT_FLOAT_EQ(Visibility3D({ 40.5F, 42.5F, 3.0F }, { 40.5F, 38.5F, 3.0F }, { 40, 42 }, { 40, 38 }, v, p), 0.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 40.5F, 42.5F, 0.5F }, { 40.5F, 38.5F, 4.5F }, { 40, 42 }, { 40, 38 }, v, p), 0.0F) << "would pass under a 1-unit lintel if the arch leaked";
	// (44,40): unexplored, no BlockLight: passes, and contributes no ambient.
	EXPECT_EQ(CellForMarch({ 44, 40 }, v, p), 255U);
	EXPECT_FLOAT_EQ(Visibility3D({ 44.5F, 42.5F, 0.5F }, { 44.5F, 38.5F, 0.5F }, { 44, 42 }, { 44, 38 }, v, p), 1.0F);
	EXPECT_FLOAT_EQ(AmbientOfCell({ 44, 40 }, v, p), 0.0F);
	EXPECT_FLOAT_EQ(AmbientOfCell({ 40, 40 }, v, p), 0.0F);
	// Explored cells keep their real bits.
	EXPECT_EQ(CellForMarch({ 20, 20 }, v, p), CellRaw({ 20, 20 }, v, p));
}

TEST(HybridReference, SkipCellsUseAnySemantics)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	// The wall (5,5) shares row 5 with both the receiver cell (3,5) and the light cell (7,5): any(cell != skip) still blocks.
	EXPECT_FLOAT_EQ(Visibility3D({ 3.5F, 5.5F, 0.5F }, { 7.5F, 5.5F, 0.5F }, { 3, 5 }, { 7, 5 }, v, p), 0.0F);
	// The receiver's own cell (a wall) is skipped ...
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 5.5F, 0.5F }, { 5.5F, 7.5F, 0.5F }, { 5, 5 }, { 5, 7 }, v, p), 1.0F);
	// ... and so is the light's cell.
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 7.5F, 0.5F }, { 5.5F, 5.5F, 0.5F }, { 5, 7 }, { 5, 5 }, v, p), 1.0F);
	// Skipping does not carry over to a neighbour that only shares a column.
	EXPECT_FLOAT_EQ(Visibility3D({ 5.5F, 7.5F, 0.5F }, { 5.5F, 3.5F, 0.5F }, { 5, 7 }, { 5, 3 }, v, p), 0.0F);
}

TEST(HybridReference, SixtyFourStepCapTerminates)
{
	const Params p = SceneParams();
	FixtureVolume volume = TheVolume();
	// A ray across the whole grid along the (blocker-free) anti-diagonal needs ~222 cell steps: the DDA stops after 64 and
	// reports visible (documented cap). The main diagonal is not used because it crosses the (5,5) wall and the (30,30) door.
	const Vec3 from { 0.5F, 111.5F, 0.5F };
	const Vec3 to { 111.5F, 0.5F, 0.5F };
	EXPECT_FLOAT_EQ(Visibility3D(from, to, { 0, 111 }, { 111, 0 }, volume.span(), p), 1.0F);
	// A blocker within the first 64 steps is still seen ...
	volume.at(10, 101) = (VExplored | VVisible | VBlock | 64U);
	EXPECT_FLOAT_EQ(Visibility3D(from, to, { 0, 111 }, { 111, 0 }, volume.span(), p), 0.0F);
	// ... one beyond the cap is not.
	volume.at(10, 101) = (VExplored | VVisible);
	volume.at(80, 31) = (VExplored | VVisible | VBlock | 64U);
	EXPECT_FLOAT_EQ(Visibility3D(from, to, { 0, 111 }, { 111, 0 }, volume.span(), p), 1.0F);
	// The same blocker is seen from nearby (it is the cap, not the cell, that hides it).
	EXPECT_FLOAT_EQ(Visibility3D({ 70.5F, 41.5F, 0.5F }, { 90.5F, 21.5F, 0.5F }, { 70, 41 }, { 90, 21 }, volume.span(), p), 0.0F);
}

TEST(HybridReference, SoftVisibilityAveragesThreeTaps)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	// Wall at (5,5). A ray along x = 6.1: the centre tap and the +x tap (6.32) pass east of the wall, the -x tap (5.88) crosses
	// x = 6 at y = 5.68 and enters (5,5) -> 2/3.
	const float soft = SoftVisibility3D({ 6.1F, 7.5F, 0.5F }, { 6.1F, 3.5F, 0.5F }, { 6, 7 }, { 6, 3 }, v, p);
	EXPECT_NEAR(soft, 2.0F / 3.0F, 1e-6F);
	EXPECT_FLOAT_EQ(Visibility3D({ 6.1F, 7.5F, 0.5F }, { 5.88F, 3.5F, 0.5F }, { 6, 7 }, { 5, 3 }, v, p), 0.0F) << "the -x tap alone";
	// Fully open and fully blocked rays are unchanged.
	EXPECT_FLOAT_EQ(SoftVisibility3D({ 60.5F, 60.5F, 0.5F }, { 64.5F, 60.5F, 0.5F }, { 60, 60 }, { 64, 60 }, v, p), 1.0F);
	EXPECT_FLOAT_EQ(SoftVisibility3D({ 5.5F, 7.5F, 0.5F }, { 5.5F, 3.5F, 0.5F }, { 5, 7 }, { 5, 3 }, v, p), 0.0F);
	// Degenerate xy distance falls back to the hard test.
	EXPECT_FLOAT_EQ(SoftVisibility3D({ 60.5F, 60.5F, 0.0F }, { 60.501F, 60.5F, 1.0F }, { 60, 60 }, { 60, 60 }, v, p), 1.0F);
}

// ========================================================= ambient and own cell ==========================================================

TEST(HybridReference, AmbientUsesTheUploadedCurve)
{
	Params p = SceneParams();
	p.ambientCurve = { 0.9F, 0.8F, 0.7F, 0.5F, 0.3F, 0.2F, 0.1F, 0.05F, 0.02F, 0.01F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F };
	const Volume v = TheVolume().span();
	EXPECT_FLOAT_EQ(AmbientOfCell({ 50, 50 }, v, p), 0.9F) << "dLight 0 reads curve[0], not a hard-coded 1";
	EXPECT_FLOAT_EQ(AmbientOfCell({ 64, 50 }, v, p), 0.0F) << "dLight 14 stays dark when the curve says so, unlike the linear 1/15";
	EXPECT_FLOAT_EQ(AmbientOfCell({ 65, 50 }, v, p), 0.0F);
}

TEST(HybridReference, AmbientFollowsClassicDLight)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	EXPECT_FLOAT_EQ(AmbientOfCell({ 50, 50 }, v, p), 1.0F);
	EXPECT_FLOAT_EQ(AmbientOfCell({ 65, 50 }, v, p), 0.0F);
	EXPECT_NEAR(AmbientOfCell({ 64, 50 }, v, p), 1.0F / 15.0F, 1e-6F);
	EXPECT_FLOAT_EQ(AmbientOfCell({ -1, 50 }, v, p), 0.0F) << "the out-of-grid sentinel V_BLOCK | 255 carries no Explored bit: no ambient";
}

TEST(HybridReference, AmbientSurfaceGatedOnlyByUnexploredOwnCell)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	// Halfway between the centres of (64,50) (dLight 14) and (65,50) (dLight 15): both explored, so surfaces take the
	// bilinear value like cards do; a fully dark explored cell bleeds from its lit neighbour instead of clamping to black.
	EXPECT_NEAR(AmbientBilinear({ 65.0F, 50.5F }, v, p), 0.5F / 15.0F, 1e-6F);
	EXPECT_NEAR(AmbientBilinearClamped({ 65.0F, 50.5F }, { 65, 50 }, v, p), 0.5F / 15.0F, 1e-6F);
	EXPECT_NEAR(AmbientBilinearClamped({ 65.0F, 50.5F }, { 64, 50 }, v, p), 0.5F / 15.0F, 1e-6F);
	for (int i = 0; i <= 160; i++) {
		const Vec2 ground { 50.0F + static_cast<float>(i) * 0.1F, 50.37F };
		const Point own = FloorPoint(ground);
		EXPECT_NEAR(AmbientBilinearClamped(ground, own, v, p), AmbientBilinear(ground, v, p), 1e-6F) << "x=" << ground.x;
	}
	// An unexplored own cell still forces zero (no-reveal).
	EXPECT_FLOAT_EQ(AmbientBilinearClamped({ 44.5F, 40.5F }, { 44, 40 }, v, p), 0.0F);
}

TEST(HybridReference, WallOwnCellRecoversTheOwnCellForEveryBaseRowColumn)
{
	// Pillar (10,7) in the fixture: top vertex (96, 16), base row 47, tile-local columns 0..63 at x = 64..127.
	const Params p = FixtureParams();
	for (int x = 64; x < 128; x++) {
		const Vec2 ground = ScreenToWorld({ static_cast<float>(x), 47.0F }, p);
		EXPECT_NEAR(ground.x + ground.y - 17.0F, 1.9375F, 1e-5F) << "u + v == 1.9375 on the base row";
		EXPECT_EQ(WallOwnCell(ground), (Point { 10, 7 })) << "x=" << x;
	}
	// The plain floor lands in the SW neighbour for the leftmost columns and the SE neighbour for the rightmost ones.
	EXPECT_EQ(FloorPoint(ScreenToWorld({ 64.0F, 47.0F }, p)), (Point { 10, 8 }));
	EXPECT_EQ(FloorPoint(ScreenToWorld({ 127.0F, 47.0F }, p)), (Point { 11, 7 }));
	// The spec's worked example: c = 0 -> own-cell coordinates (0.47, 1.47).
	const Vec2 c0 = ScreenToWorld({ 64.0F, 47.0F }, p);
	EXPECT_NEAR(c0.x - 10.0F, 0.46875F, 1e-5F);
	EXPECT_NEAR(c0.y - 7.0F, 1.46875F, 1e-5F);
}

// ============================================================ light loop ================================================================

TEST(HybridReference, GateZeroOnFullyDarkCellForSurfaces)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	const std::array<Light, 1> lights { MakeLight(65.5F, 51.5F, 0.5F, 4.0F) };
	Params pl = p;
	pl.lightCount = 1;
	const Footprint fp { { 0.0F, 0.0F }, { 112.0F, 112.0F } };
	const Vec3 n { 0.0F, 0.0F, 1.0F };
	// Floor in the dLight-15 cell: gate 0 and clamp 0 -> black.
	const Rgb dark = Accumulate({ 65.5F, 50.5F, 0.0F }, n, { 65.5F, 50.5F }, { 65, 50 }, { 65, 50 }, pl.wrapSurface, true, fp, v, lights, pl);
	EXPECT_FLOAT_EQ(MaxRgb(dark), 0.0F);
	// The dLight-14 neighbour is lit by the same light above its (tiny) ambient.
	const Rgb dim = Accumulate({ 64.5F, 50.5F, 0.0F }, n, { 64.5F, 50.5F }, { 64, 50 }, { 64, 50 }, pl.wrapSurface, true, fp, v, lights, pl);
	EXPECT_GT(MaxRgb(dim), pl.ambientScale / 15.0F + 0.05F);
	// A card at the same dark spot is not gated and not clamped.
	const Rgb card = Accumulate({ 65.5F, 50.5F, 0.0F }, { 0.6124F, 0.6124F, 0.5F }, { 65.5F, 50.5F }, { 65, 50 }, { 65, 50 }, pl.wrapCard, false, fp, v, lights, pl);
	EXPECT_GT(MaxRgb(card), 0.05F);
}

TEST(HybridReference, BloomOnlyAdds)
{
	const Params p = SceneParams();
	const Volume v = TheVolume().span();
	const std::array<Light, 1> lights { MakeLight(60.5F, 60.5F, 0.5F, 4.0F, 1.0F) };
	Params pl = p;
	pl.lightCount = 1;
	const Footprint fp { { 0.0F, 0.0F }, { 112.0F, 112.0F } };
	const Rgb plain = Accumulate({ 60.6F, 60.6F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 60.6F, 60.6F }, { 60, 60 }, { 60, 60 }, pl.wrapSurface, true, fp, v, lights, pl);
	pl.flags |= 2;
	const Rgb bloom = Accumulate({ 60.6F, 60.6F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 60.6F, 60.6F }, { 60, 60 }, { 60, 60 }, pl.wrapSurface, true, fp, v, lights, pl);
	EXPECT_GT(bloom.r, plain.r);
	// Single white light of intensity 1: radiance == peak == plain - ambient; bloom adds radiance * 0.12 * smoothstep(0.45, 1, peak).
	const float peak = plain.r - pl.ambientScale;
	const float t = std::clamp((peak - 0.45F) / 0.55F, 0.0F, 1.0F);
	EXPECT_NEAR(bloom.r - plain.r, peak * 0.12F * (t * t * (3.0F - 2.0F * t)), 1e-5F);
	EXPECT_GT(peak, 0.45F) << "the sample sits inside the bloom ramp";
	// Out of radius / not touching: nothing added.
	const std::array<Light, 1> far { MakeLight(90.5F, 90.5F, 0.5F, 2.0F) };
	const Rgb none = Accumulate({ 60.6F, 60.6F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 60.6F, 60.6F }, { 60, 60 }, { 60, 60 }, pl.wrapSurface, true, fp, v, far, pl);
	EXPECT_FLOAT_EQ(none.r, pl.ambientScale);
}

TEST(HybridReference, OrderedKeyKeepsFloatOrder)
{
	const std::array<float, 11> values { -1e9F, -112.5F, -1.0F, -1e-30F, -0.0F, 0.0F, 1e-30F, 0.25F, 1.0F, 112.5F, 1e9F };
	for (size_t i = 0; i < values.size(); i++) {
		EXPECT_NE(OrderedKey(values[i]), 0U) << "0 is the empty workgroup key";
		const float back = KeyToF32(OrderedKey(values[i]));
		EXPECT_EQ(std::memcmp(&back, &values[i], sizeof(float)), 0) << values[i];
		if (i > 0) EXPECT_LT(OrderedKey(values[i - 1]), OrderedKey(values[i])) << values[i - 1] << " < " << values[i];
	}
}

TEST(HybridReference, WorkgroupLightCullingMatchesTheUnculledPass)
{
	// 32 random lights over (and around) the fixture's world area. For every workgroup and every shaded receiver: no culled light
	// reaches the receiver's nudge square, and shading with the workgroup's mask is bit-identical to shading with every light.
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	for (const uint32_t maskScale : { 2U, 1U }) {
		for (const uint32_t softShadows : { 0U, 16U }) {
			Params p = FixtureParams();
			p.maskScale = maskScale;
			p.flags |= softShadows | 2U;
			Vec2 lo { 1e9F, 1e9F }, hi { -1e9F, -1e9F };
			for (const Vec2 corner : { Vec2 { 0.0F, 0.0F }, Vec2 { PlanesW, 0.0F }, Vec2 { 0.0F, PlanesH + 255.0F }, Vec2 { PlanesW, PlanesH + 255.0F } }) {
				const Vec2 w = ScreenToWorld(corner, p);
				lo = { std::min(lo.x, w.x), std::min(lo.y, w.y) };
				hi = { std::max(hi.x, w.x), std::max(hi.y, w.y) };
			}
			std::mt19937 rng(12345 + maskScale + softShadows);
			std::uniform_real_distribution<float> px(lo.x - 3.0F, hi.x + 3.0F);
			std::uniform_real_distribution<float> py(lo.y - 3.0F, hi.y + 3.0F);
			std::uniform_real_distribution<float> radius(0.5F, 6.0F);
			std::array<Light, 32> lights;
			for (Light &l : lights) l = MakeLight(px(rng), py(rng), 0.5F, radius(rng));
			p.lightCount = static_cast<uint32_t>(lights.size());
			const int maskW = static_cast<int>((planes.width + maskScale - 1) / maskScale);
			const int maskH = static_cast<int>((planes.height + maskScale - 1) / maskScale);
			int touched = 0, culled = 0, shaded = 0;
			for (int wy = 0; wy < (maskH + 7) / 8; wy++) {
				for (int wx = 0; wx < (maskW + 7) / 8; wx++) {
					const uint32_t lightMask = WorkgroupLightMask({ wx, wy }, planes, v, lights, p);
					for (int my = wy * 8; my < wy * 8 + 8; my++) {
						for (int mx = wx * 8; mx < wx * 8 + 8; mx++) {
							const Receiver r = ReceiverOf({ mx, my }, planes, v, p);
							if (!r.shade) continue;
							ASSERT_LT(mx, maskW) << "texels past the mask edge are never shaded";
							shaded++;
							for (size_t i = 0; i < lights.size(); i++) {
								if ((lightMask & (1U << i)) != 0) continue;
								// Nearest point of the nudge square around the ground point to the light.
								const float qx = std::clamp(lights[i].position.x, r.ground.x - p.receiverNudge, r.ground.x + p.receiverNudge) - lights[i].position.x;
								const float qy = std::clamp(lights[i].position.y, r.ground.y - p.receiverNudge, r.ground.y + p.receiverNudge) - lights[i].position.y;
								EXPECT_GE(qx * qx + qy * qy, lights[i].radius * lights[i].radius) << "light " << i << " culled although it reaches (" << mx << "," << my << ")";
							}
							const MaskTexel culledTexel = ShadeReceiver(r, lightMask, v, lights, TestLutSpan(), p);
							const MaskTexel fullTexel = ShadeReceiver(r, AllLights, v, lights, TestLutSpan(), p);
							EXPECT_EQ(std::memcmp(&culledTexel, &fullTexel, sizeof(MaskTexel)), 0) << "texel (" << mx << "," << my << ")";
						}
					}
					const int bits = std::popcount(lightMask);
					touched += bits;
					culled += static_cast<int>(lights.size()) - bits;
				}
			}
			EXPECT_GT(shaded, 0);
			EXPECT_GT(culled, 0) << "the cull must reject far lights, otherwise it is not a cull";
			EXPECT_GT(touched, 0);
		}
	}
	// A block that shades nothing gets no lights.
	Params off = FixtureParams();
	off.flags &= ~8U;
	const std::array<Light, 1> near { MakeLight(10.5F, 8.5F, 0.5F, 50.0F) };
	off.lightCount = 1;
	EXPECT_EQ(WorkgroupLightMask({ 1, 1 }, planes, v, near, off), 0U);
	// Exact boundary: a light whose radius just misses the footprint is culled, one that reaches it is kept.
	const Footprint fp { { 10.0F, 10.0F }, { 12.0F, 12.0F } };
	EXPECT_FALSE(LightTouches(fp, MakeLight(15.0F, 11.0F, 0.5F, 3.0F)));
	EXPECT_TRUE(LightTouches(fp, MakeLight(15.0F, 11.0F, 0.5F, 3.01F)));
	EXPECT_TRUE(LightTouches(fp, MakeLight(11.0F, 11.0F, 0.5F, 0.1F)));
	// The footprint form (HD sprites): the same test per light, bounded by lightCount.
	const std::array<Light, 3> three { MakeLight(15.0F, 11.0F, 0.5F, 3.0F), MakeLight(15.0F, 11.0F, 0.5F, 3.01F), MakeLight(11.0F, 11.0F, 0.5F, 0.1F) };
	Params counted = FixtureParams();
	counted.lightCount = 3;
	EXPECT_EQ(FootprintLightMask(fp, three, counted), 6U);
	counted.lightCount = 2;
	EXPECT_EQ(FootprintLightMask(fp, three, counted), 2U);
}

// ============================================================ light pass ================================================================

TEST(HybridReference, PassThroughTexels)
{
	const Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	const std::array<Light, 0> noLights {};
	const auto expectPass = [&](Point mask, const char *what) {
		const MaskTexel t = LightTexel(mask, planes, v, noLights, TestLutSpan(), p);
		EXPECT_FLOAT_EQ(t.r, 1.0F) << what;
		EXPECT_FLOAT_EQ(t.g, 1.0F) << what;
		EXPECT_FLOAT_EQ(t.b, 1.0F) << what;
		EXPECT_FLOAT_EQ(t.a, 0.0F) << what;
	};
	ASSERT_EQ(AuxTexelAt(planes, { 113, 9 }), Aux(KUnlit, 0));
	expectPass({ 56, 4 }, "Unlit region inside the viewport");
	ASSERT_EQ(AuxTexelAt(planes, { 101, 5 }), Aux(KCardEmissive, 0));
	expectPass({ 50, 2 }, "CardEmissive region");
	expectPass({ 10, 40 }, "row below the viewport (outPixel.y = 81 >= 80)");
	expectPass({ 10, 47 }, "last mask row");
	Params off = p;
	off.flags &= ~8U;
	const MaskTexel t = LightTexel({ 32, 30 }, planes, v, noLights, TestLutSpan(), off);
	EXPECT_FLOAT_EQ(t.a, 0.0F) << "hybrid flag clear: every texel is pass-through";
	// A lit floor texel is not pass-through.
	ASSERT_EQ(AuxTexelAt(planes, { 65, 61 }), Aux(KFloor, 0));
	EXPECT_FLOAT_EQ(LightTexel({ 32, 30 }, planes, v, noLights, TestLutSpan(), p).a, 1.0F);
}

TEST(HybridReference, FloorTexelMatchesAccumulate)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	const std::array<Light, 1> lights { MakeLight(10.5F, 8.5F, 0.5F, 3.0F) };
	p.lightCount = 1;
	const Point mask { 32, 30 }; // outPixel (65, 61): floor of cell (10,8), dLight 0
	const Vec2 ground = ScreenToWorld({ 65.0F, 61.0F }, p);
	ASSERT_EQ(FloorPoint(ground), (Point { 10, 8 }));
	const MaskTexel t = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	const Rgb expected = AccumulateMasked({ ground.x, ground.y, 0.0F }, { 0.0F, 0.0F, 1.0F }, ground, { 10, 8 }, { 10, 8 }, p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ t.r, t.g, t.b }, expected));
	EXPECT_GT(t.r, p.ambientScale) << "the light adds above the classic ambient";
	EXPECT_FLOAT_EQ(t.a, 1.0F);
}

TEST(HybridReference, WallTexelUsesNudgedReceiverAndOwnCell)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// SW face of wall (9,8), column c = 11 (x = 11, foot 10): rows 37 - h. Mask texel (5, 15) -> outPixel (11, 31) -> h = 6.
	const Point mask { 5, 15 };
	ASSERT_EQ(AuxTexelAt(planes, { 11, 31 }), Aux(KWallL, 1));
	ASSERT_EQ(HeightRows(KWallL, BaseTexelAt(planes, { 11, 31 }, p), 31, 4), 6U);
	const Vec2 ground = ScreenToWorld({ 11.0F, 37.0F }, p);
	const Point skipA = FloorPoint({ ground.x, ground.y + p.receiverNudge });
	const Point own = FloorPoint({ ground.x, ground.y - p.receiverNudge });
	EXPECT_EQ(skipA, (Point { 9, 9 }));
	EXPECT_EQ(own, (Point { 9, 8 }));
	// Light in front of the face (south-west, in the dLight-14 room): visible.
	const std::array<Light, 1> front { MakeLight(9.5F, 9.7F, 0.5F, 3.0F) };
	p.lightCount = 1;
	const MaskTexel lit = LightTexel(mask, planes, v, front, TestLutSpan(), p);
	const Rgb expected = AccumulateMasked({ ground.x, ground.y + p.receiverNudge, 6.0F / 32.0F }, { 0.0F, 1.0F, 0.0F }, ground, own, skipA, p.wrapSurface, true, AllLights, v, front, p);
	EXPECT_TRUE(RgbNear({ lit.r, lit.g, lit.b }, expected));
	EXPECT_GT(lit.r, p.ambientScale * 0.2F);
	// Light behind the wall (north): the wall's own cell blocks and only the (clamped) ambient remains.
	const std::array<Light, 1> behind { MakeLight(9.5F, 7.5F, 0.5F, 3.0F) };
	const MaskTexel shadow = LightTexel(mask, planes, v, behind, TestLutSpan(), p);
	EXPECT_NEAR(shadow.r, AmbientBilinearClamped(ground, own, v, p) * p.ambientScale, 1e-6F);
	// The SE face uses the WallRight normal and its own foot table: column c = 40 (x = 40, foot 20): rows 27 - h; mask (20, 12) -> outPixel (41, 25), c = 41, foot 20, h = 2.
	ASSERT_EQ(AuxTexelAt(planes, { 41, 25 }), Aux(KWallR, 2));
	ASSERT_EQ(HeightRows(KWallR, BaseTexelAt(planes, { 41, 25 }, p), 25, 4), 2U);
}

TEST(HybridReference, WallBlendedIsLitLikeTheFaceItContinues)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// Blended wall (10,9), SW column c = 17 (x = 17, foot 7): rows 72 - h. Mask (8, 32) -> outPixel (17, 65) -> h = 7.
	// A see-through pixel is lit exactly like the opaque face it continues (classic shades both with the same
	// bled-up lightmap). Lighting it from the floor under the pixel left every blended piece a different value from its
	// opaque neighbours along the wall.
	const Point mask { 8, 32 };
	ASSERT_EQ(AuxTexelAt(planes, { 17, 65 }), Aux(KWallBlended, 1));
	ASSERT_EQ(HeightRows(KWallBlended, BaseTexelAt(planes, { 17, 65 }, p), 65, 4), 7U);
	std::vector<uint8_t> opaqueBytes(planes.aux, planes.aux + static_cast<size_t>(planes.pitch) * planes.height);
	opaqueBytes[static_cast<size_t>(65) * planes.pitch + 17] = static_cast<uint8_t>(Aux(KWallL, 1));
	Planes opaque = planes;
	opaque.aux = opaqueBytes.data();
	const std::array<Light, 1> lights { MakeLight(10.5F, 10.3F, 0.5F, 3.0F) };
	for (const uint32_t count : { 0U, 1U }) {
		p.lightCount = count;
		const MaskTexel blended = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
		const MaskTexel solid = LightTexel(mask, opaque, v, lights, TestLutSpan(), p);
		EXPECT_FLOAT_EQ(blended.a, 1.0F);
		EXPECT_TRUE(RgbNear({ blended.r, blended.g, blended.b }, { solid.r, solid.g, solid.b }, 0.0F)) << count << " lights";
	}
	p.lightCount = 0;
	const MaskTexel dark = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	EXPECT_LT(MaxRgb({ dark.r, dark.g, dark.b }), 0.1F) << "dark in a dark room (not the 0.5 a pass-through half would give)";
	p.lightCount = 1;
	const MaskTexel lit = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	EXPECT_GT(lit.g, dark.g) << "a light in the room brightens the face";
}

TEST(HybridReference, WallTopTexelUsesWallOwnCell)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// Pillar (10,7): WallTop pixels x 64..127 rows 16..47, B = 47. Mask (40, 23) -> outPixel (81, 47): the base row, h = 0.
	const Point mask { 40, 23 };
	ASSERT_EQ(AuxTexelAt(planes, { 81, 47 }), Aux(KWallTop, 0));
	ASSERT_EQ(HeightRows(KWallTop, BaseTexelAt(planes, { 81, 47 }, p), 47, 0), 0U);
	const Vec2 ground = ScreenToWorld({ 81.0F, 47.0F }, p);
	ASSERT_EQ(FloorPoint(ground), (Point { 10, 8 })) << "floor() lands in the SW neighbour";
	ASSERT_EQ(WallOwnCell(ground), (Point { 10, 7 }));
	const std::array<Light, 1> lights { MakeLight(10.5F, 8.6F, 0.9F, 3.0F) };
	p.lightCount = 1;
	const MaskTexel t = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	const Rgb expected = AccumulateMasked({ ground.x, ground.y, 0.0F }, { 0.0F, 0.0F, 1.0F }, ground, { 10, 7 }, { 10, 7 }, p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ t.r, t.g, t.b }, expected));
	// Higher on the pillar: mask (40, 12) -> outPixel (81, 25) -> h = 22 -> z = 22/32, same ground (B = base row).
	const MaskTexel top = LightTexel({ 40, 12 }, planes, v, lights, TestLutSpan(), p);
	const Rgb expectedTop = AccumulateMasked({ ground.x, ground.y, 22.0F / 32.0F }, { 0.0F, 0.0F, 1.0F }, ground, { 10, 7 }, { 10, 7 }, p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ top.r, top.g, top.b }, expectedTop));
}

TEST(HybridReference, ArchPixelsCarryWallLeftWithTileBase)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// Arch (11,8): x 96..127, rows 48..79, WallLeft normal 1, B = 79 (tile base, foot None). Mask (52, 30) -> outPixel (105, 61) -> h = 18.
	const Point mask { 52, 30 };
	ASSERT_EQ(AuxTexelAt(planes, { 105, 61 }), Aux(KWallL, 1));
	ASSERT_EQ(HeightRows(KWallL, BaseTexelAt(planes, { 105, 61 }, p), 61, 0), 18U);
	const Vec2 ground = ScreenToWorld({ 105.0F, 79.0F }, p);
	const Point skipA = FloorPoint({ ground.x, ground.y + p.receiverNudge });
	const Point own = FloorPoint({ ground.x, ground.y - p.receiverNudge });
	const std::array<Light, 1> lights { MakeLight(12.5F, 9.5F, 0.5F, 3.0F) };
	p.lightCount = 1;
	const MaskTexel t = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	const Rgb expected = AccumulateMasked({ ground.x, ground.y + p.receiverNudge, 18.0F / 32.0F }, { 0.0F, 1.0F, 0.0F }, ground, own, skipA, p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ t.r, t.g, t.b }, expected)) << "arches take the foot-table wall rule (accepted for v1)";
}

TEST(HybridReference, CardStraddlingDarkCellIsNotBlack)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// Card feet row 71, x 42..57: x <= 49 grounds in (10,9) (dLight 14), x >= 50 in (11,9) (dLight 15). Mask (26,30) -> outPixel (53, 61), h = 10.
	const Point mask { 26, 30 };
	ASSERT_EQ(AuxTexelAt(planes, { 53, 61 }), Aux(KCard, 3));
	ASSERT_EQ(HeightRows(KCard, BaseTexelAt(planes, { 53, 61 }, p), 61, 0), 10U);
	const Vec2 ground = ScreenToWorld({ 53.0F, 71.0F }, p);
	ASSERT_EQ(FloorPoint(ground), (Point { 11, 9 }));
	ASSERT_EQ((CellRaw({ 11, 9 }, v, p) >> DLightShift) & 15U, 15U);
	ASSERT_EQ(FloorPoint(ScreenToWorld({ 45.0F, 71.0F }, p)), (Point { 10, 9 })) << "the card straddles the cell edge";
	const std::array<Light, 1> lights { MakeLight(10.5F, 9.5F, 0.5F, 3.0F) };
	p.lightCount = 1;
	const MaskTexel card = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	EXPECT_GT(MaxRgb({ card.r, card.g, card.b }), 0.05F) << "no gate, no clamp for cards";
	const Rgb expected = AccumulateMasked({ ground.x, ground.y, 10.0F / 32.0F }, { 0.6124F, 0.6124F, 0.5F }, ground, { 11, 9 }, { 11, 9 }, p.wrapCard, false, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ card.r, card.g, card.b }, expected));
	// The same receiver through the surface rule is gated along the interpolated level: inside the dLight-15 cell but
	// next to its dLight-14 neighbour it keeps a fading share of the direct light on top of the bilinear ambient, so it
	// is brighter than ambient alone yet dimmer than the ungated card (the gate falls to 0 at level 15).
	const Rgb surface = AccumulateMasked({ ground.x, ground.y, 10.0F / 32.0F }, { 0.6124F, 0.6124F, 0.5F }, ground, { 11, 9 }, { 11, 9 }, p.wrapSurface, true, AllLights, v, lights, p);
	const float ambientOnly = AmbientBilinear(ground, v, p) * p.ambientScale;
	const float gate = LightGate(ground, v, p);
	EXPECT_GT(gate, 0.0F) << "the interpolated level next to the dLight-14 neighbour is below 15";
	EXPECT_GT(MaxRgb(surface), ambientOnly);
	EXPECT_LT(MaxRgb(surface), MaxRgb({ card.r, card.g, card.b }));
	// The other half of the card (over the dLight-14 cell) is lit too: no per-column black stripes across the card.
	const MaskTexel left = LightTexel({ 22, 30 }, planes, v, lights, TestLutSpan(), p); // outPixel (45, 61)
	EXPECT_GT(MaxRgb({ left.r, left.g, left.b }), 0.05F);
}

TEST(HybridReference, PlaneBWrapGuardTurnsTallWallTexelIntoFloor)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	// Texels (2..3, 74..75) are WallLeft with B = (y + 250) & 255 -> h = 250 > 224. Mask (1, 37) -> outPixel (3, 75).
	const Point mask { 1, 37 };
	ASSERT_EQ(AuxTexelAt(planes, { 3, 75 }), Aux(KWallL, 1));
	ASSERT_EQ(HeightRows(KWallL, BaseTexelAt(planes, { 3, 75 }, p), 75, 0), 250U);
	const std::array<Light, 1> lights { MakeLight(10.5F, 10.5F, 0.5F, 3.0F) };
	p.lightCount = 1;
	const MaskTexel t = LightTexel(mask, planes, v, lights, TestLutSpan(), p);
	const Vec2 ground = ScreenToWorld({ 3.0F, 75.0F }, p); // h = 0: the ground is the texel itself
	const Point c = FloorPoint(ground);
	// Floor rule (z = 0, own = skipA = floor(ground)); the normal index stays what the aux byte says (LUT[1]).
	const Rgb asFloor = AccumulateMasked({ ground.x, ground.y, 0.0F }, { 0.0F, 1.0F, 0.0F }, ground, c, c, p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_TRUE(RgbNear({ t.r, t.g, t.b }, asFloor));
	// What the wall rule would have produced (250 rows up, ground 250 rows further down the screen) is different.
	const Vec2 wallGround = ScreenToWorld({ 3.0F, 75.0F + 250.0F }, p);
	const Rgb asWall = AccumulateMasked({ wallGround.x, wallGround.y + p.receiverNudge, 250.0F / 32.0F }, { 0.0F, 1.0F, 0.0F }, wallGround, FloorPoint({ wallGround.x, wallGround.y - p.receiverNudge }), FloorPoint({ wallGround.x, wallGround.y + p.receiverNudge }), p.wrapSurface, true, AllLights, v, lights, p);
	EXPECT_FALSE(RgbNear({ t.r, t.g, t.b }, asWall));
}

TEST(HybridReference, DebugVolumeViewMagentaOnUnexplored)
{
	Params p = FixtureParams();
	p.flags |= 5U << 8; // debugView = Volume
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	const std::array<Light, 0> noLights {};
	// Mask (10, 2) -> outPixel (21, 5): a floor pixel whose ground is in the unexplored cell (8,7).
	ASSERT_EQ(AuxTexelAt(planes, { 21, 5 }), Aux(KFloor, 0));
	ASSERT_EQ(FloorPoint(ScreenToWorld({ 21.0F, 5.0F }, p)), (Point { 8, 7 }));
	ASSERT_EQ(CellRaw({ 8, 7 }, v, p) & VExplored, 0U);
	const MaskTexel leak = LightTexel({ 10, 2 }, planes, v, noLights, TestLutSpan(), p);
	EXPECT_FLOAT_EQ(leak.r, 1.0F);
	EXPECT_FLOAT_EQ(leak.g, 0.0F);
	EXPECT_FLOAT_EQ(leak.b, 1.0F);
	EXPECT_FLOAT_EQ(leak.a, 1.0F);
	// A wall cell shows the red occluder tint scaled by height; the arch its blue tint; a lit floor a grey.
	const Rgb wall = DebugVolumeColor({ 9, 8 }, v, p);
	EXPECT_NEAR(wall.r, 0.3F + 64.0F / 255.0F, 1e-6F);
	EXPECT_NEAR(wall.g, 0.35F * (0.3F + 64.0F / 255.0F), 1e-6F);
	const Rgb arch = DebugVolumeColor({ 11, 8 }, v, p);
	EXPECT_NEAR(arch.b, 0.3F + 160.0F / 255.0F, 1e-6F);
	const Rgb door = DebugVolumeColor({ 34, 30 }, v, p);
	EXPECT_NEAR(door.g, 0.3F + 64.0F / 255.0F, 1e-6F);
	const Rgb floor = DebugVolumeColor({ 50, 50 }, v, p);
	EXPECT_NEAR(floor.r, 0.65F, 1e-6F);
	EXPECT_NEAR(DebugVolumeColor({ 65, 50 }, v, p).r, 0.15F, 1e-6F);
	// A wall texel (mask (20, 2) -> outPixel (41, 5), SE face of (9,8), h = 22) projects to its own cell: the red occluder tint.
	const MaskTexel wallTexel = LightTexel({ 20, 2 }, planes, v, noLights, TestLutSpan(), p);
	EXPECT_NEAR(wallTexel.r, 0.3F + 64.0F / 255.0F, 1e-6F);
	EXPECT_NEAR(wallTexel.g, 0.35F * (0.3F + 64.0F / 255.0F), 1e-6F);
	// The view is rendered into the mask even for pass-through texels (the wall's own pixels stay visible in the overlay).
	const MaskTexel unlit = LightTexel({ 56, 4 }, planes, v, noLights, TestLutSpan(), p);
	EXPECT_FLOAT_EQ(unlit.a, 1.0F);
	// Rows below the viewport are still pass-through.
	EXPECT_FLOAT_EQ(LightTexel({ 10, 44 }, planes, v, noLights, TestLutSpan(), p).a, 0.0F);
}

TEST(HybridReference, RunLightPassCoversTheFullMask)
{
	Params p = FixtureParams();
	const Planes &planes = ThePlanes().planes;
	const Volume v = TheVolume().span();
	const std::array<Light, 2> lights { MakeLight(10.5F, 8.5F, 0.5F, 3.0F), MakeLight(9.5F, 9.7F, 0.5F, 2.0F, 0.8F, { 1.0F, 0.5F, 0.2F }) };
	p.lightCount = 2;
	std::vector<MaskTexel> texels(64 * 48, MaskTexel { -1.0F, -1.0F, -1.0F, -1.0F });
	Mask mask { texels.data(), 64, 48 };
	RunLightPass(mask, planes, v, lights, TestLutSpan(), p);
	int lit = 0, passThrough = 0;
	for (uint32_t my = 0; my < mask.height; my++) {
		for (uint32_t mx = 0; mx < mask.width; mx++) {
			const MaskTexel &t = texels[my * mask.width + mx];
			ASSERT_TRUE(std::isfinite(t.r) && std::isfinite(t.g) && std::isfinite(t.b)) << mx << "," << my;
			ASSERT_TRUE(t.a == 0.0F || t.a == 1.0F) << mx << "," << my;
			if (my * 2 + 1 >= static_cast<uint32_t>(ViewportRows)) EXPECT_FLOAT_EQ(t.a, 0.0F) << "below the viewport";
			if (t.a == 1.0F) {
				EXPECT_GE(std::min(t.r, std::min(t.g, t.b)), 0.0F);
				lit++;
			} else {
				passThrough++;
			}
			const MaskTexel single = LightTexel({ static_cast<int>(mx), static_cast<int>(my) }, planes, v, lights, TestLutSpan(), p);
			EXPECT_TRUE(RgbNear({ t.r, t.g, t.b }, { single.r, single.g, single.b }, 0.0F));
		}
	}
	EXPECT_GT(lit, 64 * 30);
	EXPECT_GT(passThrough, 64 * 8);
}

// ================================================ wall lines and half-cell occluders ================================================

constexpr uint32_t VFaceSW = 1U << 8;
constexpr uint32_t VFaceSE = 1U << 9;

/** An all-floor explored volume at dLight `level`, for the scenes below. */
std::vector<uint32_t> FloorVolume(uint32_t level = 0)
{
	return std::vector<uint32_t>(112U * 112U, VExplored | (level << DLightShift));
}

TEST(HybridReference, WallCellsOccludeOnlyBehindTheirFacePlanes)
{
	const Params p = SceneParams();
	std::vector<uint32_t> cells = FloorVolume();
	const auto at = [&](int x, int y) -> uint32_t & { return cells[static_cast<size_t>(y) * 112 + x]; };
	at(10, 10) = VExplored | VBlock | VFaceSE | 96U;                 // wall along y on the line x = 10.5, facing +x
	at(20, 10) = VExplored | VBlock | VFaceSW | 96U;                 // wall along x on the line y = 10.5, facing +y
	at(30, 10) = VExplored | VBlock | VFaceSE | VFaceSW | 96U;       // inner corner: open only where x > 30.5 and y > 10.5
	at(40, 10) = VExplored | VBlock | 96U;                           // no face: the whole cell occludes
	const Volume v(cells.data(), cells.size());
	const Point none { -1, -1 };
	// A ray along y through the floor half in front of the face (x = 10.8) passes; one through the wall mass (x = 10.2) is blocked.
	EXPECT_FLOAT_EQ(Visibility3D({ 10.8F, 13.5F, 0.5F }, { 10.8F, 7.5F, 0.5F }, none, none, v, p), 1.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 10.2F, 13.5F, 0.5F }, { 10.2F, 7.5F, 0.5F }, none, none, v, p), 0.0F);
	// A diagonal ray that only clips the front half of the cell passes; one that crosses the face plane is blocked.
	EXPECT_FLOAT_EQ(Visibility3D({ 11.2F, 12.0F, 0.5F }, { 10.6F, 8.0F, 0.5F }, none, none, v, p), 1.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 11.5F, 12.0F, 0.5F }, { 9.0F, 9.0F, 0.5F }, none, none, v, p), 0.0F);
	// The face plane only blocks below the cell's height (3 units).
	EXPECT_FLOAT_EQ(Visibility3D({ 10.2F, 13.5F, 3.5F }, { 10.2F, 7.5F, 3.5F }, none, none, v, p), 1.0F);
	// Mirrored for the SW face.
	EXPECT_FLOAT_EQ(Visibility3D({ 17.5F, 10.8F, 0.5F }, { 23.5F, 10.8F, 0.5F }, none, none, v, p), 1.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 17.5F, 10.2F, 0.5F }, { 23.5F, 10.2F, 0.5F }, none, none, v, p), 0.0F);
	// Inner corner: the quadrant in front of both faces is open, the rest of the cell occludes.
	EXPECT_FLOAT_EQ(Visibility3D({ 30.8F, 13.5F, 0.5F }, { 30.8F, 10.7F, 0.5F }, none, { 30, 10 }, v, p), 1.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 30.8F, 13.5F, 0.5F }, { 30.8F, 7.5F, 0.5F }, none, none, v, p), 0.0F);
	EXPECT_FLOAT_EQ(Visibility3D({ 27.5F, 10.8F, 0.5F }, { 33.5F, 10.8F, 0.5F }, none, none, v, p), 0.0F);
	// A cell without faces blocks wherever the ray crosses it.
	EXPECT_FLOAT_EQ(Visibility3D({ 40.8F, 13.5F, 0.5F }, { 40.8F, 7.5F, 0.5F }, none, none, v, p), 0.0F);
	// Hidden-information rule: an unexplored wall cell exposes no face and occludes as a whole, at infinite height.
	at(10, 10) &= ~VExplored;
	EXPECT_FLOAT_EQ(Visibility3D({ 10.8F, 13.5F, 3.5F }, { 10.8F, 7.5F, 3.5F }, none, none, v, p), 0.0F);
}

/**
 * A straight wall running along y on the line x = 10.5, built from four wall pieces tagged the way DrawCell tags them:
 * the left half of each cell is WallRight (SE face, +x normal) with the c / 2 foot table anchored at the cell's base row.
 */
struct WallAlongYScene {
	static constexpr int W = 320, H = 360;
	static constexpr int WallX = 10, FirstY = 4, LastY = 7, Courses = 3; // 96 px tall
	std::vector<uint8_t> aux, base, index;
	std::vector<uint32_t> cells = FloorVolume();
	Params p = BaseParams(0.0F, 0.0F);
	Planes planes;
	WallAlongYScene()
	    : aux(static_cast<size_t>(W) * H, 0)
	    , base(static_cast<size_t>(W) * H, 0)
	    , index(static_cast<size_t>(W) * H, 0)
	{
		p.viewportHeight = H;
		p.maskScale = 1;
		for (int y = FirstY; y <= LastY; y++) {
			const int leftX = 32 * (WallX - y) - 32;                  // diamond left vertex (origin 0, 0)
			const int anchorRow = 16 * (WallX + y) + 31;              // the cell's bottom (base) row
			for (int c = 0; c < 32; c++) {
				const int foot = c / 2;                               // gbuffer::FootRows(FootKind::SE, c)
				for (int h = 0; h < 32 * Courses; h++) {
					const int row = anchorRow - foot - h;
					const size_t i = static_cast<size_t>(row) * W + static_cast<size_t>(leftX + c);
					aux[i] = static_cast<uint8_t>(Aux(KWallR, 2));
					base[i] = static_cast<uint8_t>((anchorRow - foot) & 0xFF);
				}
			}
			cells[static_cast<size_t>(y) * 112 + WallX] = VExplored | VBlock | VFaceSE | 96U | (static_cast<uint32_t>(std::abs(y - 5)) << DLightShift);
			cells[static_cast<size_t>(y) * 112 + WallX - 1] = VExplored | VBlock | 96U | (5U << DLightShift);
		}
		planes = Planes { aux.data(), base.data(), index.data(), static_cast<uint32_t>(W), static_cast<uint32_t>(W), static_cast<uint32_t>(H) };
	}
	[[nodiscard]] Volume volume() const { return Volume(cells.data(), cells.size()); }
};

TEST(HybridReference, WallPiecesJoinIntoOneContinuousFace)
{
	// The "cubes of light" regression: with the wall lines of neighbouring pieces joining into one line, every
	// texel of the face is lit from a ground point on x = 10.5, and the lighting is continuous across the 32-px piece
	// boundaries, with ambient, direct light and visibility all smooth along the wall.
	WallAlongYScene scene;
	const Volume v = scene.volume();
	const std::array<Light, 1> lights { MakeLight(13.5F, 5.5F, 0.5F, 8.0F) };
	scene.p.lightCount = 1;
	const int rows[] = { 220, 240, 260 };
	for (const int row : rows) {
		float previous = -1.0F;
		int previousX = -1;
		int checked = 0;
		for (int x = 0; x < WallAlongYScene::W; x++) {
			if (AuxTexelAt(scene.planes, { x, row }) != Aux(KWallR, 2)) continue;
			const uint32_t h = HeightRows(KWallR, BaseTexelAt(scene.planes, { x, row }, scene.p), static_cast<uint32_t>(row), 0);
			const Vec2 ground = ScreenToWorld({ static_cast<float>(x), static_cast<float>(row) + static_cast<float>(h) }, scene.p);
			ASSERT_NEAR(ground.x, 10.5F, 1.0F / 32.0F + 1e-4F) << "every face texel stands on the wall line, x " << x << " row " << row;
			const MaskTexel t = LightTexel({ x, row }, scene.planes, v, lights, TestLutSpan(), scene.p);
			ASSERT_FLOAT_EQ(t.a, 1.0F);
			if (previousX == x - 1) {
				EXPECT_NEAR(t.r, previous, 0.02F) << "x " << x << " row " << row << (x % 32 == 0 ? " (piece boundary)" : "");
			}
			previous = t.r;
			previousX = x;
			checked++;
		}
		EXPECT_GT(checked, 64) << "row " << row << " crosses several pieces of the wall";
	}
	// A light grazing along the face, in front of the wall line, is not shadowed by the neighbouring wall cells.
	const std::array<Light, 1> grazing { MakeLight(10.85F, 1.5F, 0.5F, 8.0F) };
	const int x = 32 * (WallAlongYScene::WallX - 6) - 32 + 20; // cell (10, 6), c = 20
	const int row = 16 * (WallAlongYScene::WallX + 6) + 31 - 10 - 8; // 8 rows above the foot
	ASSERT_EQ(AuxTexelAt(scene.planes, { x, row }), Aux(KWallR, 2));
	const uint32_t h = HeightRows(KWallR, BaseTexelAt(scene.planes, { x, row }, scene.p), static_cast<uint32_t>(row), 0);
	const Vec2 ground = ScreenToWorld({ static_cast<float>(x), static_cast<float>(row) + static_cast<float>(h) }, scene.p);
	const Vec3 receiver { ground.x + scene.p.receiverNudge, ground.y, static_cast<float>(h) / scene.p.heightPixelsPerWorldUnit };
	EXPECT_FLOAT_EQ(Visibility3D(receiver, grazing[0].position, FloorPoint(ground), { 10, 1 }, v, scene.p), 1.0F);
}

// =========================================================== present pass ===============================================================

TEST(HybridReference, SrgbRoundTrip)
{
	const Rgb c { 0.02F, 0.5F, 0.9F };
	const Rgb lin = SrgbToLinear(c);
	EXPECT_NEAR(lin.r, 0.02F / 12.92F, 1e-7F);
	EXPECT_NEAR(lin.g, 0.214041F, 1e-5F);
	EXPECT_NEAR(lin.b, 0.787412F, 1e-5F);
	const Rgb back = LinearToSrgb(lin);
	EXPECT_TRUE(RgbNear(back, c, 1e-5F));
}

TEST(HybridReference, PaletteEmissiveAtIndexesThePackedVec4s)
{
	std::array<float, 256> e {};
	for (size_t i = 0; i < e.size(); i++) e[i] = static_cast<float>(i) / 255.0F;
	const Emissive es(e);
	EXPECT_FLOAT_EQ(PaletteEmissiveAt(es, 0), 0.0F);
	EXPECT_FLOAT_EQ(PaletteEmissiveAt(es, 1), 1.0F / 255.0F);
	EXPECT_FLOAT_EQ(PaletteEmissiveAt(es, 7), 7.0F / 255.0F);
	EXPECT_FLOAT_EQ(PaletteEmissiveAt(es, 255), 1.0F);
}

TEST(HybridReference, BilateralUpsampleRejectsKindAndViewportAndFallsBackToM0)
{
	// 8x8 planes, 4x4 mask (maskScale 2). Pixel (2,2): mf = (0.5, 0.5), m0 = (0,0), taps at sp (1,1) (3,1) (1,3) (3,3), each w = 0.25.
	std::array<uint8_t, 64> aux {};
	std::array<uint8_t, 64> base {};
	std::array<uint8_t, 64> index {};
	aux.fill(static_cast<uint8_t>(Aux(KFloor, 0)));
	aux[1 * 8 + 3] = static_cast<uint8_t>(Aux(KWallL, 1)); // tap (3,1) is a wall: rejected for floors
	base[1 * 8 + 3] = 1;                                     // h = 0 for that wall texel (B = row 1)
	const Planes planes { aux.data(), base.data(), index.data(), 8, 8, 8 };
	std::array<MaskTexel, 16> texels {};
	for (auto &t : texels) t = { 0.0F, 0.0F, 0.0F, 1.0F };
	texels[0 * 4 + 0] = { 0.2F, 0.2F, 0.2F, 1.0F };
	texels[0 * 4 + 1] = { 0.9F, 0.9F, 0.9F, 1.0F };
	texels[1 * 4 + 0] = { 0.4F, 0.4F, 0.4F, 1.0F };
	texels[1 * 4 + 1] = { 0.6F, 0.6F, 0.6F, 1.0F };
	const Mask mask { texels.data(), 4, 4 };
	Params p = FixtureParams();
	p.viewportHeight = 8;
	// Kind rejection: (0.2 + 0.4 + 0.6) / 3.
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KFloor, 0, mask, planes, p).r, 0.4F, 1e-6F);
	// The wall tap is accepted when the requested kind is WallLeft with a matching height ... and the floors rejected.
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KWallL, 0, mask, planes, p).r, 0.9F, 1e-6F);
	// ... and rejected again when the height differs by 8 rows or more.
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KWallL, 8, mask, planes, p).r, 0.2F, 1e-6F) << "wsum == 0 -> m0 = (0,0)";
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KWallL, 7, mask, planes, p).r, 0.9F, 1e-6F) << "|dh| < 8 is accepted";
	// Viewport rejection: rows >= 3 hold pass-through, so the (1,3) and (3,3) taps drop out.
	p.viewportHeight = 3;
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KFloor, 0, mask, planes, p).r, 0.2F, 1e-6F);
	p.viewportHeight = 8;
	// wsum == 0 with a kind nobody has: nearest m0.
	EXPECT_NEAR(SampleLighting({ 2, 2 }, KCard, 0, mask, planes, p).r, 0.2F, 1e-6F);
	// Pixel (0,0): m0 = (-1,-1); every tap with a negative sp reads aux 0 (robust) = Unlit and is rejected, (1,1) is the only floor tap.
	EXPECT_NEAR(SampleLighting({ 0, 0 }, KFloor, 0, mask, planes, p).r, 0.2F, 1e-6F);
	// ... and with no accepted tap the m0 fallback reads out of range: 0 (robust access), never a garbage texel.
	EXPECT_FLOAT_EQ(SampleLighting({ 0, 0 }, KCard, 0, mask, planes, p).r, 0.0F);
	// Full-resolution mask: the texel itself.
	p.maskScale = 1;
	EXPECT_NEAR(SampleLighting({ 1, 1 }, KFloor, 0, mask, planes, p).r, 0.6F, 1e-6F);
}

TEST(HybridReference, FrameColorChoosesPaletteInsideTheViewport)
{
	std::array<Rgb, 256> paletteData {};
	for (size_t i = 0; i < paletteData.size(); i++) paletteData[i] = { static_cast<float>(i) / 255.0F, static_cast<float>(255 - i) / 255.0F, 0.5F };
	const Palette palette(paletteData);
	Params p = FixtureParams();
	const Rgb frameSrgb { 0.1F, 0.2F, 0.3F };
	EXPECT_TRUE(RgbNear(FrameColor({ 5, 5 }, frameSrgb, palette, 17, p), paletteData[17], 0.0F));
	EXPECT_TRUE(RgbNear(FrameColor({ 5, ViewportRows }, frameSrgb, palette, 17, p), frameSrgb, 0.0F)) << "panel rows read the RGBA frame";
	p.frameHasUnlit = true;
	EXPECT_TRUE(RgbNear(FrameColor({ 5, 5 }, frameSrgb, palette, 17, p), frameSrgb, 0.0F)) << "virtual gamepad: the whole frame";
}

struct CompositeScene {
	Params p = FixtureParams();
	std::array<Rgb, 256> paletteData {};
	std::array<float, 256> emissiveData {};
	std::vector<MaskTexel> texels;
	Mask mask {};
	std::array<Light, 1> lights { MakeLight(10.5F, 8.5F, 0.5F, 3.0F) };
	CompositeScene()
	{
		for (size_t i = 0; i < 256; i++) {
			paletteData[i] = { static_cast<float>(i) / 255.0F, static_cast<float>((i * 3) & 255) / 255.0F, static_cast<float>((i * 7) & 255) / 255.0F };
			emissiveData[i] = i >= 200 ? 0.5F : 0.0F;
		}
		p.lightCount = 1;
		texels.resize(64 * 48);
		mask = { texels.data(), 64, 48 };
		RunLightPass(mask, ThePlanes().planes, TheVolume().span(), lights, TestLutSpan(), p);
	}
	[[nodiscard]] Rgb composite(Point px, Rgb frame = { 0.33F, 0.66F, 0.99F }) const
	{
		return Composite(px, frame, Palette(paletteData), ThePlanes().planes, mask, Emissive(emissiveData), TestLutSpan(), p);
	}
	[[nodiscard]] Rgb composite(Point px, const Params &params, Rgb frame = { 0.33F, 0.66F, 0.99F }) const
	{
		return Composite(px, frame, Palette(paletteData), ThePlanes().planes, mask, Emissive(emissiveData), TestLutSpan(), params);
	}
};

TEST(HybridReference, CompositeReproducesPaletteBitExactlyForUnlitViewportPixels)
{
	const CompositeScene scene;
	const Planes &planes = ThePlanes().planes;
	// Unlit region x 112..127 rows 0..15.
	for (int y = 0; y < 16; y++) {
		for (int x = 112; x < 128; x++) {
			ASSERT_EQ(AuxTexelAt(planes, { x, y }) >> 5U, KUnlit);
			const uint32_t idx = IndexTexelAt(planes, { x, y });
			const Rgb out = scene.composite({ x, y });
			EXPECT_EQ(out.r, scene.paletteData[idx].r) << x << "," << y;
			EXPECT_EQ(out.g, scene.paletteData[idx].g) << x << "," << y;
			EXPECT_EQ(out.b, scene.paletteData[idx].b) << x << "," << y;
		}
	}
	// CardEmissive too (pass-through kind).
	EXPECT_TRUE(RgbNear(scene.composite({ 105, 5 }), scene.paletteData[IndexTexelAt(planes, { 105, 5 })], 0.0F));
	// Panel rows read the RGBA frame regardless of the planes.
	const Rgb frame { 0.11F, 0.22F, 0.33F };
	EXPECT_TRUE(RgbNear(scene.composite({ 40, ViewportRows + 3 }, frame), frame, 0.0F));
	EXPECT_TRUE(RgbNear(scene.composite({ 40, ViewportRows }, frame), frame, 0.0F));
	// frameHasUnlit: a lit floor pixel in the viewport also starts from the RGBA frame, then gets lit.
	Params gamepad = scene.p;
	gamepad.frameHasUnlit = true;
	const Rgb litFromFrame = scene.composite({ 65, 61 }, gamepad, frame);
	const Rgb litFromPalette = scene.composite({ 65, 61 }, frame);
	EXPECT_FALSE(RgbNear(litFromFrame, litFromPalette, 1e-4F));
	// The legacy path (hybrid flag clear) is the frame.
	Params legacy = scene.p;
	legacy.flags &= ~8U;
	EXPECT_TRUE(RgbNear(scene.composite({ 65, 61 }, legacy, frame), frame, 0.0F));
}

TEST(HybridReference, CompositeLightsFloorPixelsAndAddsEmissive)
{
	const CompositeScene scene;
	const Planes &planes = ThePlanes().planes;
	const Point px { 65, 61 }; // floor of (10,8), next to the light: lighting > ambient
	ASSERT_EQ(AuxTexelAt(planes, px) >> 5U, KFloor);
	const uint32_t idx = IndexTexelAt(planes, px);
	const Rgb lighting = SampleLighting(px, KFloor, 0, scene.mask, planes, scene.p);
	EXPECT_GT(lighting.r, scene.p.ambientScale);
	const Rgb albedo = SrgbToLinear(scene.paletteData[idx]);
	const float emissive = scene.emissiveData[idx] * scene.p.emissiveScale;
	Rgb expected { albedo.r * lighting.r + albedo.r * emissive, albedo.g * lighting.g + albedo.g * emissive, albedo.b * lighting.b + albedo.b * emissive };
	expected = LinearToSrgb({ std::min(expected.r, 1.0F), std::min(expected.g, 1.0F), std::min(expected.b, 1.0F) });
	EXPECT_TRUE(RgbNear(scene.composite(px), expected, 1e-5F));
	// A pixel whose palette index carries emissive weight gets albedo * emissive * emissiveScale on top: find one in the floor region.
	Point emissivePx { -1, -1 };
	for (int y = 48; y < 80 && emissivePx.x < 0; y++)
		for (int x = 64; x < 96; x++)
			if (IndexTexelAt(planes, { x, y }) >= 200 && (AuxTexelAt(planes, { x, y }) >> 5U) == KFloor) {
				emissivePx = { x, y };
				break;
			}
	ASSERT_GE(emissivePx.x, 0);
	Params noEmissive = scene.p;
	noEmissive.emissiveScale = 0.0F;
	const Rgb with = scene.composite(emissivePx);
	const Rgb without = scene.composite(emissivePx, noEmissive);
	EXPECT_GT(with.r + with.g + with.b, without.r + without.g + without.b);
	// Exposure scales the linear result.
	Params dim = scene.p;
	dim.exposure = 0.5F;
	const Rgb half = scene.composite(px, dim);
	EXPECT_LT(half.r, scene.composite(px).r);
}

TEST(HybridReference, CompositeDebugViews)
{
	const CompositeScene scene;
	const Planes &planes = ThePlanes().planes;
	const auto view = [&](uint32_t which) {
		Params p = scene.p;
		p.flags |= which << 8;
		return p;
	};
	// Kind: wall SW face pixel (11, 31) -> red; floor -> grey 0.25; Unlit -> black.
	EXPECT_TRUE(RgbNear(scene.composite({ 11, 31 }, view(1)), { 1.0F, 0.0F, 0.0F }, 0.0F));
	EXPECT_TRUE(RgbNear(scene.composite({ 65, 61 }, view(1)), { 0.25F, 0.25F, 0.25F }, 0.0F));
	EXPECT_TRUE(RgbNear(scene.composite({ 113, 9 }, view(1)), { 0.0F, 0.0F, 0.0F }, 0.0F));
	// Height: (11, 31) has h = 6.
	EXPECT_NEAR(scene.composite({ 11, 31 }, view(2)).r, 6.0F / 255.0F, 1e-6F);
	EXPECT_NEAR(scene.composite({ 3, 75 }, view(2)).r, 250.0F / 255.0F, 1e-6F) << "the Height view shows the raw wrapped value";
	// Normal: LUT[1] * 0.5 + 0.5 = (0.5, 1.0, 0.5).
	EXPECT_TRUE(RgbNear(scene.composite({ 11, 31 }, view(3)), { 0.5F, 1.0F, 0.5F }, 1e-6F));
	// Lighting: the mask texel at px / maskScale.
	const MaskTexel m = scene.texels[30 * 64 + 32];
	EXPECT_TRUE(RgbNear(scene.composite({ 65, 61 }, view(4)), { m.r, m.g, m.b }, 0.0F));
	// Emissive: grey from the palette weight.
	const Point emissivePx { 0, 0 };
	const float weight = IndexTexelAt(planes, emissivePx) >= 200 ? 0.5F : 0.0F;
	EXPECT_NEAR(scene.composite(emissivePx, view(6)).r, weight, 0.0F);
	// Unknown view: the frame.
	EXPECT_TRUE(RgbNear(scene.composite({ 65, 61 }, view(9)), scene.paletteData[IndexTexelAt(planes, { 65, 61 })], 0.0F));
}

TEST(HybridReference, CompositeAppliesWrapGuardBeforeSampling)
{
	const CompositeScene scene;
	const Planes &planes = ThePlanes().planes;
	// (2, 74): WallLeft with h = 250 -> composite samples the mask as Floor, h = 0. Its bilateral taps are the mask texels at
	// output pixels (1,73) (3,73) (1,75) (floors, accepted) and (3,75) (the guard texel itself, WallLeft: rejected for floors);
	// as a 250-row wall only the (3,75) tap would be accepted, so the guard changes the result.
	const Point px { 2, 74 };
	ASSERT_EQ(AuxTexelAt(planes, px) >> 5U, KWallL);
	ASSERT_EQ(HeightRows(KWallL, BaseTexelAt(planes, px, scene.p), 74, 0), 250U);
	const Rgb asFloor = SampleLighting(px, KFloor, 0, scene.mask, planes, scene.p);
	const Rgb asWall = SampleLighting(px, KWallL, 250, scene.mask, planes, scene.p);
	EXPECT_FALSE(RgbNear(asFloor, asWall, 1e-6F)) << "the guard changes which taps are accepted";
	const uint32_t idx = IndexTexelAt(planes, px);
	const Rgb albedo = SrgbToLinear(scene.paletteData[idx]);
	const float emissive = idx >= 200 ? 0.5F : 0.0F;
	Rgb expected { albedo.r * asFloor.r + albedo.r * emissive * scene.p.emissiveScale, albedo.g * asFloor.g + albedo.g * emissive * scene.p.emissiveScale, albedo.b * asFloor.b + albedo.b * emissive * scene.p.emissiveScale };
	expected = LinearToSrgb({ std::min(expected.r, 1.0F), std::min(expected.g, 1.0F), std::min(expected.b, 1.0F) });
	EXPECT_TRUE(RgbNear(scene.composite(px), expected, 1e-5F));
}

} // namespace
} // namespace devilution
