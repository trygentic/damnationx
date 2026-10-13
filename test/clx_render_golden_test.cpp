/**
 * @file clx_render_golden_test.cpp
 *
 * Classic byte-identity goldens for the CLX sprite blitter.
 *
 * Every public entry point (ClxDraw / ClxDrawTRN / ClxDrawBlended / ClxDrawBlendedTRN / ClxDrawOutline /
 * ClxDrawOutlineSkipColorZero) is driven over every frame of `data/resistance.clx` and `ui_art/dvl_lrpopup.clx`
 * at nine positions that exercise the unclipped path, the ClipY path (top / bottom) and the ClipXY path
 * (left / right and every corner). The FNV-64 hash of the resulting surface is compared against
 * `test/fixtures/clx_render/goldens.tsv`, which was recorded before any hybrid-lighting change to
 * `clx_render.cpp` / `blit_impl.hpp`.
 *
 * Re-record with `DEVILUTIONX_CLX_RENDER_GOLDEN_RECORD=<path> ./clx_render_golden_test` (only when the
 * classic output is intentionally changed).
 */
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/clx_sprite.hpp"
#include "engine/load_clx.hpp"
#include "engine/point.hpp"
#include "engine/render/clx_normal_map.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/gbuffer.hpp"
#include "engine/surface.hpp"
#include "utils/clx_decode.hpp"
#include "utils/palette_blending.hpp"

namespace devilution {
namespace {

constexpr const char *GoldensPath = "test/fixtures/clx_render/goldens.tsv";
constexpr int SurfaceWidth = 400;
constexpr int SurfaceHeight = 300;
constexpr uint8_t OutlineColor = 0xFE;
// RenderClxOutline's contract (clx_render.cpp MaxOutlineSpriteWidth): wider sprites are never outlined by the game.
constexpr unsigned MaxOutlineSpriteWidth = 253;

struct GoldenSprite {
	const char *name;
	const char *path;
};

constexpr GoldenSprite GoldenSprites[] = {
	{ "resistance", "data\\resistance.clx" },
	{ "dvl_lrpopup", "ui_art\\dvl_lrpopup.clx" },
};

enum class Op : uint8_t {
	Draw,
	TRN,
	Blended,
	BlendedTRN,
	Outline,
	OutlineSkipColorZero,
};

constexpr Op AllOps[] = { Op::Draw, Op::TRN, Op::Blended, Op::BlendedTRN, Op::Outline, Op::OutlineSkipColorZero };

const char *OpName(Op op)
{
	switch (op) {
	case Op::Draw: return "Draw";
	case Op::TRN: return "TRN";
	case Op::Blended: return "Blended";
	case Op::BlendedTRN: return "BlendedTRN";
	case Op::Outline: return "Outline";
	case Op::OutlineSkipColorZero: return "OutlineSkipColorZero";
	}
	return "?";
}

bool OpSupports(Op op, ClxSprite sprite)
{
	if (op == Op::Outline || op == Op::OutlineSkipColorZero)
		return sprite.width() < MaxOutlineSpriteWidth;
	return true;
}

struct GoldenPosition {
	const char *name;
	Point position; // bottom-left, as passed to ClxDraw*
};

// Nine positions: unclipped, ClipY (top / bottom) and ClipXY (left / right / all four corners).
std::vector<GoldenPosition> GoldenPositions(int w, int h)
{
	const int cx = (SurfaceWidth - w) / 2;
	const int cy = (SurfaceHeight + h) / 2 - 1;
	const int leftX = -(w / 2);
	const int rightX = SurfaceWidth - w / 2;
	const int topY = h / 2;
	const int bottomY = SurfaceHeight + h / 2;
	return {
		{ "Full", { cx, cy } },
		{ "ClipLeft", { leftX, cy } },
		{ "ClipRight", { rightX, cy } },
		{ "ClipTop", { cx, topY } },
		{ "ClipBottom", { cx, bottomY } },
		{ "ClipLeftTop", { leftX, topY } },
		{ "ClipRightBottom", { rightX, bottomY } },
		{ "ClipLeftBottom", { leftX, bottomY } },
		{ "ClipRightTop", { rightX, topY } },
	};
}

// Deterministic synthetic TRN (never identity; index 0 stays 0 so shadows stay shadows).
const uint8_t *TestTrn()
{
	static const std::vector<uint8_t> trn = [] {
		std::vector<uint8_t> t(256);
		t[0] = 0;
		for (int i = 1; i < 256; ++i)
			t[i] = static_cast<uint8_t>(1 + ((i * 7 + 3) % 255));
		return t;
	}();
	return trn.data();
}

// Deterministic synthetic transparency table (standalone tests have no palette).
void InitTestTransparencyTable()
{
	static bool initialized = false;
	if (initialized)
		return;
	for (int a = 0; a < 256; ++a) {
		for (int b = 0; b < 256; ++b)
			paletteTransparencyLookup[a][b] = static_cast<uint8_t>((a * 3 + b * 5 + 11) & 0xFF);
	}
	initialized = true;
}

void FillSurfacePattern(const Surface &out)
{
	for (int y = 0; y < out.h(); ++y) {
		for (int x = 0; x < out.w(); ++x)
			*out.at(x, y) = static_cast<uint8_t>((x * 31 + y * 17) & 0xFF);
	}
}

uint64_t HashSurface(const Surface &out)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (int y = 0; y < out.h(); ++y) {
		const uint8_t *row = out.at(0, y);
		for (int x = 0; x < out.w(); ++x) {
			h ^= row[x];
			h *= 0x100000001b3ULL;
		}
	}
	return h;
}

void RunOp(Op op, const Surface &out, Point position, ClxSprite sprite)
{
	switch (op) {
	case Op::Draw: ClxDraw(out, position, sprite); break;
	case Op::TRN: ClxDrawTRN(out, position, sprite, TestTrn()); break;
	case Op::Blended: ClxDrawBlended(out, position, sprite); break;
	case Op::BlendedTRN: ClxDrawBlendedTRN(out, position, sprite, TestTrn()); break;
	case Op::Outline: ClxDrawOutline(out, OutlineColor, position, sprite); break;
	case Op::OutlineSkipColorZero: ClxDrawOutlineSkipColorZero(out, OutlineColor, position, sprite); break;
	}
}

std::string CaseKey(const char *spriteName, size_t frame, Op op, const char *positionName)
{
	std::ostringstream key;
	key << spriteName << '\t' << frame << '\t' << OpName(op) << '\t' << positionName;
	return key.str();
}

// key -> "x\ty\thash"
std::map<std::string, std::string> ReadGoldens(const char *path)
{
	std::map<std::string, std::string> goldens;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		// sprite \t frame \t op \t position \t x \t y \t hash
		size_t tabs = 0;
		size_t split = std::string::npos;
		for (size_t i = 0; i < line.size(); ++i) {
			if (line[i] != '\t')
				continue;
			if (++tabs == 4) {
				split = i;
				break;
			}
		}
		if (split == std::string::npos)
			continue;
		goldens.emplace(line.substr(0, split), line.substr(split + 1));
	}
	return goldens;
}

std::string FormatValue(Point position, uint64_t hash)
{
	std::ostringstream value;
	value << position.x << '\t' << position.y << '\t' << std::hex << hash;
	return value.str();
}

TEST(ClxRenderGolden, ClassicByteIdentity)
{
	InitTestTransparencyTable();
	ASSERT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off);
	ASSERT_EQ(clx_normal_map::GetStats().entries, 0U);

	const char *recordPath = std::getenv("DEVILUTIONX_CLX_RENDER_GOLDEN_RECORD");
	const std::map<std::string, std::string> goldens = ReadGoldens(GoldensPath);
	if (recordPath == nullptr) {
		ASSERT_FALSE(goldens.empty()) << "no golden cases in " << GoldensPath;
	}

	std::vector<std::string> recorded;
	size_t checked = 0;
	OwnedSurface out(SurfaceWidth, SurfaceHeight);
	for (const GoldenSprite &spriteInfo : GoldenSprites) {
		const OwnedClxSpriteList sprites = LoadClx(spriteInfo.path);
		ASSERT_GT(sprites.numSprites(), 0U) << spriteInfo.path;
		for (size_t frame = 0; frame < sprites.numSprites(); ++frame) {
			const ClxSprite sprite = sprites[frame];
			for (const GoldenPosition &pos : GoldenPositions(sprite.width(), sprite.height())) {
				for (const Op op : AllOps) {
					if (!OpSupports(op, sprite))
						continue;
					FillSurfacePattern(out);
					RunOp(op, out, pos.position, sprite);
					const std::string key = CaseKey(spriteInfo.name, frame, op, pos.name);
					const std::string value = FormatValue(pos.position, HashSurface(out));
					if (recordPath != nullptr) {
						recorded.push_back(key + '\t' + value);
						continue;
					}
					const auto it = goldens.find(key);
					ASSERT_NE(it, goldens.end()) << "missing golden for " << key;
					EXPECT_EQ(it->second, value) << key;
					++checked;
				}
			}
		}
	}
	// The classic path never touches the hybrid state.
	EXPECT_EQ(gbuffer::CurrentMode, gbuffer::Mode::Off);
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);

	if (recordPath != nullptr) {
		std::ofstream outFile(recordPath);
		ASSERT_TRUE(outFile.is_open()) << recordPath;
		outFile << "# clx_render classic goldens: sprite\tframe\top\tposition\tx\ty\tfnv64 (surface "
		        << SurfaceWidth << "x" << SurfaceHeight << ", see README.md)\n";
		for (const std::string &line : recorded)
			outFile << line << '\n';
		SUCCEED() << "recorded " << recorded.size() << " golden cases to " << recordPath;
		return;
	}
	EXPECT_EQ(checked, goldens.size()) << "goldens.tsv holds cases this test no longer produces";
}

// ---------------------------------------------------------------------------------------------------------------
// Capture / stamp parity: the hybrid path must write the colour exactly like the classic path
// and tag exactly the written pixels in the G-buffer planes. The planes are driven directly through
// gbuffer::CurrentPlanes / CurrentMode (gbuffer::Allocate is not needed) and the card normal plane comes from
// clx_normal_map::SetNormalPlaneProviderForTesting (the generator is not needed).
// ---------------------------------------------------------------------------------------------------------------

constexpr int16_t Transparent = -1;
constexpr uint8_t AuxSentinel = 0xA5;
constexpr uint8_t BaseSentinel = 0x5A;

/** A bitmap (rows top-down, -1 = transparent) and its CL2 encoding with a 6-byte CLX frame header. */
struct SyntheticSprite {
	int w = 0;
	int h = 0;
	std::vector<int16_t> pixels;
	std::vector<uint8_t> fill; // 1 where the encoder emitted the pixel as part of a fill run
	std::vector<uint8_t> data;

	[[nodiscard]] int16_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * w + x]; }
	[[nodiscard]] bool isFill(int x, int y) const { return fill[static_cast<size_t>(y) * w + x] != 0; }
	[[nodiscard]] ClxSprite sprite() const { return ClxSprite(data.data(), static_cast<uint32_t>(data.size())); }
};

// CL2 frame data: rows bottom-up; transparent runs (control 1..127) may cross row boundaries, opaque runs never do.
// Fill runs (0x80..0xBE, width 0xBF - control) for >= 3 equal pixels, pixel runs (0xBF..0xFF, width 256 - control) otherwise.
void EncodeClx(SyntheticSprite &s)
{
	std::vector<uint8_t> out(6);
	out[0] = 6;
	out[1] = 0;
	out[2] = static_cast<uint8_t>(s.w & 0xFF);
	out[3] = static_cast<uint8_t>(s.w >> 8);
	out[4] = static_cast<uint8_t>(s.h & 0xFF);
	out[5] = static_cast<uint8_t>(s.h >> 8);
	s.fill.assign(s.pixels.size(), 0);
	int pendingTransparent = 0;
	auto flushTransparent = [&] {
		while (pendingTransparent > 0) {
			const int n = std::min(pendingTransparent, 127);
			out.push_back(static_cast<uint8_t>(n));
			pendingTransparent -= n;
		}
	};
	for (int y = s.h - 1; y >= 0; --y) {
		int x = 0;
		while (x < s.w) {
			const int16_t c = s.at(x, y);
			if (c == Transparent) {
				++pendingTransparent;
				++x;
				continue;
			}
			flushTransparent();
			int n = 1;
			while (x + n < s.w && s.at(x + n, y) == c)
				++n;
			if (n >= 3) {
				n = std::min(n, 63);
				out.push_back(static_cast<uint8_t>(0xBF - n));
				out.push_back(static_cast<uint8_t>(c));
				for (int i = 0; i < n; ++i)
					s.fill[static_cast<size_t>(y) * s.w + x + i] = 1;
			} else {
				n = 1;
				while (x + n < s.w && n < 65 && s.at(x + n, y) != Transparent
				    && !(x + n + 2 < s.w && s.at(x + n, y) == s.at(x + n + 1, y) && s.at(x + n, y) == s.at(x + n + 2, y)))
					++n;
				out.push_back(static_cast<uint8_t>(256 - n));
				for (int i = 0; i < n; ++i)
					out.push_back(static_cast<uint8_t>(s.at(x + i, y)));
			}
			x += n;
		}
	}
	flushTransparent();
	s.data = std::move(out);
}

/** 41x33 blob with holes, a fully transparent row, shadow (colour 0) rows and a shadow stripe, fills and pixel runs. */
SyntheticSprite MakeTestSprite()
{
	SyntheticSprite s;
	s.w = 41;
	s.h = 33;
	s.pixels.assign(static_cast<size_t>(s.w) * s.h, Transparent);
	for (int y = 0; y < s.h; ++y) {
		for (int x = 0; x < s.w; ++x) {
			const double dx = (x - 20.0) / 20.5;
			const double dy = (y - 16.0) / 16.5;
			if (dx * dx + dy * dy > 1.0)
				continue; // outside the ellipse
			if (y == 12)
				continue; // a fully transparent row inside the shape (multi-line skip)
			if ((x / 3 + y / 2) % 7 == 0 && y > 4 && y < 28)
				continue; // holes: transparent runs inside a row
			int16_t c;
			if (y >= s.h - 3)
				c = 0; // shadow rows at the feet
			else if (x >= 28 && x < 33)
				c = 0; // shadow stripe
			else if ((y / 4) % 2 == 0)
				c = static_cast<int16_t>(1 + (y * 5) % 250); // fill runs
			else
				c = static_cast<int16_t>(1 + (x * 7 + y * 13) % 250); // pixel runs
			s.pixels[static_cast<size_t>(y) * s.w + x] = c;
		}
	}
	EncodeClx(s);
	return s;
}

/** 96x24 sprite with long runs: whole-row fills (63 + 33), whole-row pixel runs (65 + 31), mixed short and medium
 *  runs, a holed row, a shadow row and a transparent row, so every run-length class of the plane writers
 *  (1 / 2..3 / 4..7 / 8..15 / 16..32 / > 32) is exercised under every clip path. */
SyntheticSprite MakeWideTestSprite()
{
	SyntheticSprite s;
	s.w = 96;
	s.h = 24;
	s.pixels.assign(static_cast<size_t>(s.w) * s.h, Transparent);
	for (int y = 0; y < s.h; ++y) {
		for (int x = 0; x < s.w; ++x) {
			int16_t c;
			switch (y % 6) {
			case 0: c = static_cast<int16_t>(1 + y); break;                                                    // one colour: fill 63 + fill 33
			case 1: c = static_cast<int16_t>(1 + (x * 7 + y * 13) % 250); break;                               // distinct: pixels 65 + 31
			case 2: c = static_cast<int16_t>(1 + ((x * x) / 7) % 200); break;                                  // growing equal blocks: 1..32 px runs
			case 3: c = x % 11 == 10 ? Transparent : static_cast<int16_t>(1 + (x / 11) % 250); break;          // 10 px fills split by holes
			case 4: c = 0; break;                                                                                // shadow row (ShadowByte in the plane)
			default: c = y == 11 ? Transparent : static_cast<int16_t>(1 + (x % 3 == 0 ? x : y) % 250); break; // y == 11: whole-row skip; else 1-3 px runs
			}
			s.pixels[static_cast<size_t>(y) * s.w + x] = c;
		}
	}
	EncodeClx(s);
	return s;
}

/** Longest opaque run (fill or pixel) in a CL2 encoding. */
unsigned LongestOpaqueRun(ClxSprite sprite)
{
	unsigned longest = 0;
	const uint8_t *src = sprite.pixelData();
	const uint8_t *const end = src + sprite.pixelDataSize();
	while (src < end) {
		const uint8_t control = *src++;
		if (!IsClxOpaque(control))
			continue;
		if (IsClxOpaqueFill(control)) {
			longest = std::max<unsigned>(longest, GetClxOpaqueFillWidth(control));
			++src;
		} else {
			const unsigned n = GetClxOpaquePixelsWidth(control);
			longest = std::max(longest, n);
			src += n;
		}
	}
	return longest;
}

/** Decodes a CLX frame to a top-down bitmap (-1 = transparent), the layout SyntheticSprite uses. */
std::vector<int16_t> DecodeClxTopDown(ClxSprite sprite)
{
	const int w = sprite.width();
	const int h = sprite.height();
	std::vector<int16_t> pixels(static_cast<size_t>(w) * h, Transparent);
	const uint8_t *src = sprite.pixelData();
	const uint8_t *const end = src + sprite.pixelDataSize();
	int x = 0;
	int row = 0; // bottom-up
	while (src < end && row < h) {
		const uint8_t control = *src++;
		if (!IsClxOpaque(control)) {
			x += control;
		} else {
			const size_t rowBase = static_cast<size_t>(h - 1 - row) * w;
			if (IsClxOpaqueFill(control)) {
				const uint8_t n = GetClxOpaqueFillWidth(control);
				const uint8_t color = *src++;
				for (uint8_t i = 0; i < n && x + i < w; ++i)
					pixels[rowBase + x + i] = color;
				x += n;
			} else {
				const uint8_t n = GetClxOpaquePixelsWidth(control);
				for (uint8_t i = 0; i < n && x + i < w; ++i)
					pixels[rowBase + x + i] = src[i];
				src += n;
				x += n;
			}
		}
		while (x >= w) {
			x -= w;
			++row;
		}
	}
	return pixels;
}

/** Row-span plane for a synthetic sprite: opaque -> PackAux(Card, 3 + f(x, r)), colour 0 -> ShadowByte, gaps -> 0. */
struct TestPlane {
	std::vector<uint8_t> data;
	std::vector<uint32_t> rowOffset;
	std::vector<uint16_t> rowX0;
	uint16_t width = 0;
	uint16_t height = 0;

	[[nodiscard]] clx_normal_map::Plane view() const
	{
		clx_normal_map::Plane p;
		p.data = data.data();
		p.rowOffset = rowOffset.data();
		p.rowX0 = rowX0.data();
		p.width = width;
		p.height = height;
		return p;
	}
};

uint8_t ExpectedCardByte(int x, int r)
{
	return gbuffer::PackAux(gbuffer::Kind::Card, static_cast<uint8_t>(3 + (x * 5 + r * 3) % 29));
}

TestPlane MakePlaneFromBitmap(int w, int h, const std::vector<int16_t> &pixelsTopDown)
{
	TestPlane p;
	p.width = static_cast<uint16_t>(w);
	p.height = static_cast<uint16_t>(h);
	p.rowOffset.push_back(0);
	for (int r = 0; r < h; ++r) {
		const int y = h - 1 - r; // r = 0 is the feet (bottom) row
		const auto at = [&](int x) { return pixelsTopDown[static_cast<size_t>(y) * w + x]; };
		int x0 = w;
		int x1 = 0;
		for (int x = 0; x < w; ++x) {
			if (at(x) == Transparent)
				continue;
			x0 = std::min(x0, x);
			x1 = x + 1;
		}
		if (x1 <= x0) {
			x0 = 0;
			x1 = 0;
		}
		p.rowX0.push_back(static_cast<uint16_t>(x0));
		for (int x = x0; x < x1; ++x) {
			const int16_t c = at(x);
			p.data.push_back(c == Transparent ? 0 : c == 0 ? gbuffer::ShadowByte : ExpectedCardByte(x, r));
		}
		p.rowOffset.push_back(static_cast<uint32_t>(p.data.size()));
	}
	return p;
}

TestPlane MakeTestPlane(const SyntheticSprite &s)
{
	return MakePlaneFromBitmap(s.w, s.h, s.pixels);
}

// Provider state (the provider is a plain function pointer).
struct ProviderState {
	TestPlane plane;
	const uint8_t *expectedPixelData = nullptr;
	bool flat = false;
	int calls = 0;
	int mirroredCalls = 0;
	int wrongSpriteCalls = 0;
};
ProviderState TestProvider;

clx_normal_map::Plane ProvidePlane(ClxSprite sprite, bool mirrored)
{
	++TestProvider.calls;
	if (mirrored)
		++TestProvider.mirroredCalls;
	if (sprite.pixelData() != TestProvider.expectedPixelData)
		++TestProvider.wrongSpriteCalls;
	if (TestProvider.flat)
		return clx_normal_map::Plane {};
	return TestProvider.plane.view();
}

/** Back buffer + planes mirrored into gbuffer::CurrentPlanes; restores the global state on destruction. */
struct PlanesFixture {
	OwnedSurface out;
	std::vector<uint8_t> aux;
	std::vector<uint8_t> base;

	PlanesFixture(int width, int height)
	    : out(width, height)
	    , aux(static_cast<size_t>(out.pitch()) * height, AuxSentinel)
	    , base(static_cast<size_t>(out.pitch()) * height, BaseSentinel)
	{
		gbuffer::Planes &planes = gbuffer::CurrentPlanes;
		planes.aux = aux.data();
		planes.base = base.data();
		planes.outBase = out.begin();
		planes.pitch = out.pitch();
		planes.width = static_cast<uint16_t>(width);
		planes.height = static_cast<uint16_t>(height);
		planes.pitchRecip40 = ((static_cast<uint64_t>(1) << 40) + planes.pitch - 1) / planes.pitch;
	}
	~PlanesFixture()
	{
		gbuffer::CurrentPlanes = gbuffer::Planes {};
		gbuffer::CurrentMode = gbuffer::Mode::Off;
		gbuffer::CurrentCardKind = gbuffer::Kind::Card;
		clx_normal_map::SetNormalPlaneProviderForTesting(nullptr);
	}

	void reset()
	{
		FillSurfacePattern(out);
		std::fill(aux.begin(), aux.end(), AuxSentinel);
		std::fill(base.begin(), base.end(), BaseSentinel);
	}
	[[nodiscard]] uint8_t auxAt(int x, int y) const { return aux[static_cast<size_t>(y) * out.pitch() + x]; }
	[[nodiscard]] uint8_t baseAt(int x, int y) const { return base[static_cast<size_t>(y) * out.pitch() + x]; }
};

constexpr int FixtureWidth = 130; // pitch (132) != width so the reciprocal row math is exercised
constexpr int FixtureHeight = 70;

constexpr Op CaptureOps[] = { Op::Draw, Op::TRN, Op::Blended, Op::BlendedTRN };

struct ParityPosition {
	const char *name;
	Point position;
};

// Relative to a `w x h` sprite drawn into a `W x H` (sub)surface: unclipped, ClipY (top / bottom incl. a far-bottom
// position that skips whole lines), ClipXY (left / right / corners) and two fully-offscreen positions.
std::vector<ParityPosition> ParityPositions(int w, int h, int W, int H)
{
	return {
		{ "Full", { (W - w) / 2, (H + h) / 2 } },
		{ "ClipTop", { (W - w) / 2, h / 3 } },
		{ "ClipBottom", { (W - w) / 2, H + h / 3 } },
		{ "ClipBottomFar", { (W - w) / 2, H + h - 4 } },
		{ "ClipLeft", { -w / 2, (H + h) / 2 } },
		{ "ClipRight", { W - w / 3, (H + h) / 2 } },
		{ "ClipLeftTop", { -w / 2, h / 3 } },
		{ "ClipRightBottom", { W - w / 3, H + h / 3 } },
		{ "ClipLeftBottom", { -w / 2, H + h / 2 } },
		{ "ClipRightTop", { W - w / 3, h / 3 } },
		{ "Clip1pxLeft", { -w + 1, (H + h) / 2 } },
		{ "Clip1pxRight", { W - 1, (H + h) / 2 } },
		{ "OffscreenLeft", { -w, (H + h) / 2 } },
		{ "OffscreenTop", { (W - w) / 2, -1 } },
	};
}

enum class AuxExpect : uint8_t {
	Plane,      // per-pixel plane byte, base = feet
	Const,      // constant auxByte / baseByte
	StampUnlit, // aux = Unlit, base untouched
	Untouched,  // planes untouched (classic / offscreen / Mode::Off)
};

struct ParityCase {
	Op op;
	Point position;    // relative to `region`
	Surface region;    // the surface passed to ClxDraw* (the back buffer or a subregion of it)
	int regionX = 0;   // region origin in back-buffer coordinates
	int regionY = 0;
	AuxExpect expect;
	uint8_t auxByte = 0;
	uint8_t baseByte = 0;
};

// Classic blit semantics (blit_impl.hpp). The synthetic blend table is deliberately asymmetric, so the operand order
// matters: BlitBlendedWithMap blends fill runs as pal[trn[src]][dst] and pixel runs as pal[dst][trn[src]].
uint8_t InnerColor(Op op, int16_t src, bool fillRun, uint8_t dstBefore)
{
	const auto c = static_cast<uint8_t>(src);
	switch (op) {
	case Op::Draw: return c;
	case Op::TRN: return TestTrn()[c];
	case Op::Blended: return paletteTransparencyLookup[c][dstBefore];
	case Op::BlendedTRN: return fillRun ? paletteTransparencyLookup[TestTrn()[c]][dstBefore] : paletteTransparencyLookup[dstBefore][TestTrn()[c]];
	default: return c;
	}
}

/** Independent oracle: for every back-buffer pixel, derive from the bitmap whether the classic blit writes it and check
 *  colour / aux / base accordingly. Returns the number of written pixels. */
size_t CheckParity(PlanesFixture &f, const SyntheticSprite &s, const ParityCase &c, const std::string &label)
{
	f.reset();
	// Pre-image for the blended ops.
	std::vector<uint8_t> before(static_cast<size_t>(f.out.pitch()) * f.out.h());
	std::memcpy(before.data(), f.out.begin(), before.size());

	RunOp(c.op, c.region, c.position, s.sprite());

	const int absLeft = c.regionX + c.position.x;
	const int absFeetY = c.regionY + c.position.y;
	const int absTop = absFeetY - s.h + 1;
	const auto feetByte = static_cast<uint8_t>(absFeetY & 0xFF);
	size_t written = 0;
	size_t failures = 0;
	for (int y = 0; y < f.out.h() && failures < 8; ++y) {
		for (int x = 0; x < f.out.w() && failures < 8; ++x) {
			const uint8_t beforeColor = before[static_cast<size_t>(y) * f.out.pitch() + x];
			const uint8_t color = *f.out.at(x, y);
			const uint8_t aux = f.auxAt(x, y);
			const uint8_t base = f.baseAt(x, y);
			const int sx = x - absLeft;
			const int sy = y - absTop;
			const bool inRegion = x >= c.regionX && x < c.regionX + c.region.w() && y >= c.regionY && y < c.regionY + c.region.h();
			const bool opaque = inRegion && sx >= 0 && sx < s.w && sy >= 0 && sy < s.h && s.at(sx, sy) != Transparent;
			const std::string where = label + " @(" + std::to_string(x) + "," + std::to_string(y) + ")";
			if (!opaque) {
				if (color != beforeColor || aux != AuxSentinel || base != BaseSentinel) {
					ADD_FAILURE() << where << ": untouched pixel changed (color " << int(beforeColor) << "->" << int(color)
					              << ", aux " << int(aux) << ", base " << int(base) << ")";
					++failures;
				}
				continue;
			}
			++written;
			const uint8_t expectedColor = InnerColor(c.op, s.at(sx, sy), s.isFill(sx, sy), beforeColor);
			uint8_t expectedAux = AuxSentinel;
			uint8_t expectedBase = BaseSentinel;
			switch (c.expect) {
			case AuxExpect::Plane: {
				const int r = s.h - 1 - sy;
				expectedAux = s.at(sx, sy) == 0 ? gbuffer::ShadowByte : ExpectedCardByte(sx, r);
				expectedBase = feetByte;
				break;
			}
			case AuxExpect::Const:
				expectedAux = c.auxByte;
				expectedBase = c.baseByte;
				break;
			case AuxExpect::StampUnlit:
				expectedAux = gbuffer::UnlitByte;
				break;
			case AuxExpect::Untouched:
				break;
			}
			if (color != expectedColor || aux != expectedAux || base != expectedBase) {
				ADD_FAILURE() << where << " sprite(" << sx << "," << sy << "): color " << int(color) << " (want " << int(expectedColor)
				              << "), aux " << int(aux) << " (want " << int(expectedAux) << "), base " << int(base) << " (want "
				              << int(expectedBase) << ")";
				++failures;
			}
		}
	}
	return written;
}

std::string ParityLabel(const char *test, Op op, const char *position, bool subregion)
{
	return std::string(test) + "/" + OpName(op) + "/" + position + (subregion ? "/subregion" : "/full");
}

// Runs every op at every position on the full back buffer and on an offset subregion of it.
void RunParitySuite(const char *test, PlanesFixture &f, const SyntheticSprite &s, AuxExpect expect, uint8_t auxByte, uint8_t baseByte, size_t &writtenTotal)
{
	const Surface full = f.out;
	const Surface sub = f.out.subregion(9, 7, 90, 50);
	for (const bool useSub : { false, true }) {
		const Surface &region = useSub ? sub : full;
		for (const ParityPosition &pos : ParityPositions(s.w, s.h, region.w(), region.h())) {
			for (const Op op : CaptureOps) {
				ParityCase c { op, pos.position, region, useSub ? 9 : 0, useSub ? 7 : 0, expect, auxByte, baseByte };
				writtenTotal += CheckParity(f, s, c, ParityLabel(test, op, pos.name, useSub));
			}
		}
	}
}

TEST(ClxRenderCapture, ClassicModeLeavesPlanesUntouched)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::Off;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	size_t written = 0;
	RunParitySuite("Off", f, s, AuxExpect::Untouched, 0, 0, written);
	EXPECT_GT(written, 0U);
	EXPECT_EQ(TestProvider.calls, 0);
}

TEST(ClxRenderCapture, CardPlanePath)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	size_t written = 0;
	RunParitySuite("CardPlane", f, s, AuxExpect::Plane, 0, 0, written);
	EXPECT_GT(written, 0U);
	// One lookup per draw, never mirrored, always for the drawn sprite.
	EXPECT_EQ(TestProvider.calls, static_cast<int>(2 * ParityPositions(s.w, s.h, 1, 1).size() * std::size(CaptureOps)));
	EXPECT_EQ(TestProvider.mirroredCalls, 0);
	EXPECT_EQ(TestProvider.wrongSpriteCalls, 0);
}

TEST(ClxRenderCapture, CardFlatTier)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.flat = true;
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	size_t written = 0;
	// base = feet byte: the Const oracle takes it per case below, so run the suite by hand.
	const Surface full = f.out;
	const Surface sub = f.out.subregion(9, 7, 90, 50);
	for (const bool useSub : { false, true }) {
		const Surface &region = useSub ? sub : full;
		for (const ParityPosition &pos : ParityPositions(s.w, s.h, region.w(), region.h())) {
			for (const Op op : CaptureOps) {
				const int absFeetY = (useSub ? 7 : 0) + pos.position.y;
				ParityCase c { op, pos.position, region, useSub ? 9 : 0, useSub ? 7 : 0, AuxExpect::Const, gbuffer::FlatCardByte, static_cast<uint8_t>(absFeetY & 0xFF) };
				written += CheckParity(f, s, c, ParityLabel("CardFlat", op, pos.name, useSub));
			}
		}
	}
	EXPECT_GT(written, 0U);
	EXPECT_GT(TestProvider.calls, 0);
}

TEST(ClxRenderCapture, CardEmissive)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	gbuffer::CurrentCardKind = gbuffer::Kind::CardEmissive;
	size_t written = 0;
	const Surface full = f.out;
	const Surface sub = f.out.subregion(9, 7, 90, 50);
	for (const bool useSub : { false, true }) {
		const Surface &region = useSub ? sub : full;
		for (const ParityPosition &pos : ParityPositions(s.w, s.h, region.w(), region.h())) {
			for (const Op op : CaptureOps) {
				const int absFeetY = (useSub ? 7 : 0) + pos.position.y;
				ParityCase c { op, pos.position, region, useSub ? 9 : 0, useSub ? 7 : 0, AuxExpect::Const, gbuffer::EmissiveCardByte, static_cast<uint8_t>(absFeetY & 0xFF) };
				written += CheckParity(f, s, c, ParityLabel("CardEmissive", op, pos.name, useSub));
			}
		}
	}
	EXPECT_GT(written, 0U);
	// Emissive cards never touch the normal cache.
	EXPECT_EQ(TestProvider.calls, 0);
}

TEST(ClxRenderCapture, ArchesUseTileContext)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	for (const gbuffer::Kind kind : { gbuffer::Kind::WallLeft, gbuffer::Kind::WallRight }) {
		gbuffer::CurrentCardKind = kind;
		const uint8_t normal = kind == gbuffer::Kind::WallLeft ? gbuffer::NormalWallLeft : gbuffer::NormalWallRight;
		gbuffer::CurrentTileAux = gbuffer::PackAux(kind, normal);
		gbuffer::CurrentTileAuxBlended = gbuffer::CurrentTileAux;
		gbuffer::CurrentTileBaseRow.fill(0x11);
		gbuffer::CurrentTileBaseRow[0] = kind == gbuffer::Kind::WallLeft ? 0x77 : 0x99;
		size_t written = 0;
		RunParitySuite(kind == gbuffer::Kind::WallLeft ? "ArchLeft" : "ArchRight", f, s, AuxExpect::Const, gbuffer::CurrentTileAux, gbuffer::CurrentTileBaseRow[0], written);
		EXPECT_GT(written, 0U);
	}
	gbuffer::CurrentTileAux = gbuffer::UnlitByte;
	gbuffer::CurrentTileAuxBlended = gbuffer::UnlitByte;
	gbuffer::CurrentTileBaseRow.fill(0);
	EXPECT_EQ(TestProvider.calls, 0);
}

TEST(ClxRenderCapture, OffscreenSurfaceTakesClassicPath)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	OwnedSurface offscreen(64, 48);
	ASSERT_FALSE(gbuffer::CurrentPlanes.covers(offscreen));
	for (const gbuffer::Mode mode : { gbuffer::Mode::CaptureWorld, gbuffer::Mode::StampUnlit }) {
		gbuffer::CurrentMode = mode;
		for (const ParityPosition &pos : ParityPositions(s.w, s.h, offscreen.w(), offscreen.h())) {
			for (const Op op : CaptureOps) {
				f.reset();
				FillSurfacePattern(offscreen);
				RunOp(op, offscreen, pos.position, s.sprite());
				// Same bytes as the classic path.
				OwnedSurface reference(64, 48);
				FillSurfacePattern(reference);
				gbuffer::CurrentMode = gbuffer::Mode::Off;
				RunOp(op, reference, pos.position, s.sprite());
				gbuffer::CurrentMode = mode;
				for (int y = 0; y < 48; ++y)
					ASSERT_EQ(std::memcmp(offscreen.at(0, y), reference.at(0, y), 64), 0) << OpName(op) << "/" << pos.name << " row " << y;
				// Planes untouched.
				EXPECT_TRUE(std::all_of(f.aux.begin(), f.aux.end(), [](uint8_t v) { return v == AuxSentinel; })) << OpName(op) << "/" << pos.name;
				EXPECT_TRUE(std::all_of(f.base.begin(), f.base.end(), [](uint8_t v) { return v == BaseSentinel; })) << OpName(op) << "/" << pos.name;
			}
		}
	}
	EXPECT_EQ(TestProvider.calls, 0);
}

TEST(ClxRenderStamp, StampUnlitTagsExactlyTheWrittenPixels)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentMode = gbuffer::Mode::StampUnlit;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	size_t written = 0;
	RunParitySuite("Stamp", f, s, AuxExpect::StampUnlit, 0, 0, written);
	EXPECT_GT(written, 0U);
	EXPECT_EQ(TestProvider.calls, 0);
}

// Outline stamping: every outline pixel written gets aux = Unlit (both the unclipped fast path and the clipped
// SetPixel path); in capture mode the ring keeps the kind under it.
TEST(ClxRenderStamp, OutlineStampsWrittenPixels)
{
	const SyntheticSprite s = MakeTestSprite();
	PlanesFixture f(FixtureWidth, FixtureHeight);
	constexpr uint8_t Ring = 0xFE; // the pre-image below is scrubbed of this value, so "changed" == "written"
	const Surface full = f.out;
	const Surface sub = f.out.subregion(9, 7, 90, 50);
	for (const gbuffer::Mode mode : { gbuffer::Mode::StampUnlit, gbuffer::Mode::CaptureWorld }) {
		gbuffer::CurrentMode = mode;
		for (const bool useSub : { false, true }) {
			const Surface &region = useSub ? sub : full;
			for (const ParityPosition &pos : ParityPositions(s.w, s.h, region.w(), region.h())) {
				for (const bool skipZero : { false, true }) {
					f.reset();
					std::vector<uint8_t> before(static_cast<size_t>(f.out.pitch()) * f.out.h());
					std::memcpy(before.data(), f.out.begin(), before.size());
					for (uint8_t &v : before) {
						if (v == Ring) v = Ring - 1;
					}
					std::memcpy(f.out.begin(), before.data(), before.size());
					if (skipZero)
						ClxDrawOutlineSkipColorZero(region, Ring, pos.position, s.sprite());
					else
						ClxDrawOutline(region, Ring, pos.position, s.sprite());
					size_t ringPixels = 0;
					for (int y = 0; y < f.out.h(); ++y) {
						for (int x = 0; x < f.out.w(); ++x) {
							const bool wrote = *f.out.at(x, y) != before[static_cast<size_t>(y) * f.out.pitch() + x];
							const uint8_t aux = f.auxAt(x, y);
							const uint8_t expectedAux = wrote && mode == gbuffer::Mode::StampUnlit ? gbuffer::UnlitByte : AuxSentinel;
							if (wrote) {
								++ringPixels;
								ASSERT_EQ(*f.out.at(x, y), Ring) << pos.name << " @(" << x << "," << y << ")";
								// Outline pixels are never inside the region drawn outside `region`.
								ASSERT_TRUE(x >= (useSub ? 9 : 0) && x < (useSub ? 99 : f.out.w()) && y >= (useSub ? 7 : 0) && y < (useSub ? 57 : f.out.h()));
							}
							ASSERT_EQ(aux, expectedAux) << pos.name << (useSub ? "/sub" : "/full") << " @(" << x << "," << y << ") wrote=" << wrote;
							ASSERT_EQ(f.baseAt(x, y), BaseSentinel);
						}
					}
					if (std::string(pos.name).rfind("Offscreen", 0) != 0)
						EXPECT_GT(ringPixels, 0U) << pos.name;
				}
			}
		}
	}
}

// Every writer on a sprite whose runs reach the fixed-store limit and beyond (fills of 63, pixel runs of 65).
TEST(ClxRenderCapture, LongRunsThroughEveryWriter)
{
	InitTestTransparencyTable();
	const SyntheticSprite s = MakeWideTestSprite();
	ASSERT_GE(LongestOpaqueRun(s.sprite()), 63U);
	PlanesFixture f(FixtureWidth, FixtureHeight);
	TestProvider = ProviderState {};
	TestProvider.expectedPixelData = s.sprite().pixelData();
	TestProvider.plane = MakeTestPlane(s);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);

	size_t written = 0;
	gbuffer::CurrentMode = gbuffer::Mode::CaptureWorld;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	RunParitySuite("WidePlane", f, s, AuxExpect::Plane, 0, 0, written);
	EXPECT_GT(written, 0U);

	written = 0;
	gbuffer::CurrentCardKind = gbuffer::Kind::WallRight;
	gbuffer::CurrentTileAux = gbuffer::PackAux(gbuffer::Kind::WallRight, gbuffer::NormalWallRight);
	gbuffer::CurrentTileAuxBlended = gbuffer::CurrentTileAux;
	gbuffer::CurrentTileBaseRow.fill(0x22);
	gbuffer::CurrentTileBaseRow[0] = 0x88;
	RunParitySuite("WideConst", f, s, AuxExpect::Const, gbuffer::CurrentTileAux, 0x88, written);
	EXPECT_GT(written, 0U);
	gbuffer::CurrentTileAux = gbuffer::UnlitByte;
	gbuffer::CurrentTileAuxBlended = gbuffer::UnlitByte;
	gbuffer::CurrentTileBaseRow.fill(0);

	written = 0;
	gbuffer::CurrentMode = gbuffer::Mode::StampUnlit;
	gbuffer::CurrentCardKind = gbuffer::Kind::Card;
	RunParitySuite("WideStamp", f, s, AuxExpect::StampUnlit, 0, 0, written);
	EXPECT_GT(written, 0U);
}

// The hybrid writers must leave the colour plane byte-identical to the classic goldens on the real assets
// (runs up to 65 px, every golden clip position) in every capture / stamp configuration.
TEST(ClxRenderGolden, HybridColourByteIdentity)
{
	InitTestTransparencyTable();
	const std::map<std::string, std::string> goldens = ReadGoldens(GoldensPath);
	ASSERT_FALSE(goldens.empty()) << "no golden cases in " << GoldensPath;

	struct Config {
		const char *name;
		gbuffer::Mode mode;
		gbuffer::Kind kind;
		bool flat; // provider returns no plane
	};
	constexpr Config Configs[] = {
		{ "CapturePlane", gbuffer::Mode::CaptureWorld, gbuffer::Kind::Card, false },
		{ "CaptureFlat", gbuffer::Mode::CaptureWorld, gbuffer::Kind::Card, true },
		{ "CaptureEmissive", gbuffer::Mode::CaptureWorld, gbuffer::Kind::CardEmissive, false },
		{ "CaptureArch", gbuffer::Mode::CaptureWorld, gbuffer::Kind::WallLeft, false },
		{ "StampUnlit", gbuffer::Mode::StampUnlit, gbuffer::Kind::Card, false },
	};

	PlanesFixture f(SurfaceWidth, SurfaceHeight);
	clx_normal_map::SetNormalPlaneProviderForTesting(ProvidePlane);
	gbuffer::CurrentTileAux = gbuffer::PackAux(gbuffer::Kind::WallLeft, gbuffer::NormalWallLeft);
	gbuffer::CurrentTileAuxBlended = gbuffer::CurrentTileAux;
	gbuffer::CurrentTileBaseRow.fill(0x33);
	for (const Config &config : Configs) {
		gbuffer::CurrentMode = config.mode;
		gbuffer::CurrentCardKind = config.kind;
		size_t checked = 0;
		size_t auxWrites = 0;
		for (const GoldenSprite &spriteInfo : GoldenSprites) {
			const OwnedClxSpriteList sprites = LoadClx(spriteInfo.path);
			ASSERT_GT(sprites.numSprites(), 0U) << spriteInfo.path;
			for (size_t frame = 0; frame < sprites.numSprites(); ++frame) {
				const ClxSprite sprite = sprites[frame];
				TestProvider = ProviderState {};
				TestProvider.expectedPixelData = sprite.pixelData();
				TestProvider.flat = config.flat;
				if (!config.flat)
					TestProvider.plane = MakePlaneFromBitmap(sprite.width(), sprite.height(), DecodeClxTopDown(sprite));
				for (const GoldenPosition &pos : GoldenPositions(sprite.width(), sprite.height())) {
					for (const Op op : AllOps) {
						if (!OpSupports(op, sprite))
							continue;
						f.reset();
						RunOp(op, f.out, pos.position, sprite);
						const std::string key = CaseKey(spriteInfo.name, frame, op, pos.name);
						const auto it = goldens.find(key);
						ASSERT_NE(it, goldens.end()) << "missing golden for " << key;
						EXPECT_EQ(it->second, FormatValue(pos.position, HashSurface(f.out))) << config.name << " " << key;
						auxWrites += static_cast<size_t>(std::count_if(f.aux.begin(), f.aux.end(), [](uint8_t v) { return v != AuxSentinel; }));
						++checked;
					}
				}
				EXPECT_EQ(TestProvider.wrongSpriteCalls, 0) << config.name;
				EXPECT_EQ(TestProvider.mirroredCalls, 0) << config.name;
			}
		}
		EXPECT_EQ(checked, goldens.size()) << config.name;
		EXPECT_GT(auxWrites, 0U) << config.name << ": the hybrid path did not run";
	}
	gbuffer::CurrentTileAux = gbuffer::UnlitByte;
	gbuffer::CurrentTileAuxBlended = gbuffer::UnlitByte;
	gbuffer::CurrentTileBaseRow.fill(0);
}

// A stubbed ClearNormalPlaneCache would be a no-op; the hook is still exercised so a link error would surface here.
TEST(ClxRenderCapture, ClearClxDrawCacheAlsoClearsNormalPlanes)
{
	ClearClxDrawCache();
	EXPECT_EQ(clx_normal_map::GetStats().entries, 0U);
}

} // namespace
} // namespace devilution
