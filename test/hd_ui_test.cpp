#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "engine/render/hd_ui_core.hpp"

namespace devilution {
namespace {

using hd_ui::ClxOpaqueMask;
using hd_ui::LayerTracker;
using hd_ui::Mask;
using hd_ui::Placed;
using hd_ui::PlacedMasked;
using hd_ui::Rect;

constexpr char Sample[] = "\xEF\xBB\xBF# DamnationX HD UI\n"
                          "scale 4\n"
                          "sheet 0 ui/ctrlpan-panel8-0.png 2560 576\n"
                          "sheet 1 ui/fonts-12-00-0.png 512 256 indexed  # glyphs\r\n"
                          "frame ctrlpan/panel8 0 0 0 0 2560 576\n"
                          "frame fonts/12-00 65 1 40 0 32 48\n"
                          "frame fonts/12-00 66 1 72 0 32 48\n"
                          "frame Data\\Inv\\ObjCurs.CEL 3 0 0 0 112 112\n"
                          "frame ctrlpan/panel8 1 7 0 0 4 4\n"
                          "frame ctrlpan/panel8 2 0 2550 0 20 20\n"
                          "sheet 5 bad.png 4 4\n"
                          "bogus line\n";

TEST(HdUiIndexTest, ParsesSheetsAndFrames)
{
	const hd_ui::UiIndex index = hd_ui::ParseUiIndex(Sample);
	EXPECT_EQ(index.scale, 4);
	ASSERT_EQ(index.sheets.size(), 2U);
	EXPECT_EQ(index.sheets[0].png, "ui/ctrlpan-panel8-0.png");
	EXPECT_FALSE(index.sheets[0].indexed);
	EXPECT_TRUE(index.sheets[1].indexed);
	const hd_ui::FrameInfo *panel = index.find("ctrlpan/panel8", 0);
	ASSERT_NE(panel, nullptr);
	EXPECT_EQ(panel->w, 2560);
	const hd_ui::FrameInfo *glyph = index.find("fonts/12-00", 66);
	ASSERT_NE(glyph, nullptr);
	EXPECT_EQ(glyph->sheet, 1);
	EXPECT_EQ(glyph->x, 72);
	EXPECT_EQ(index.find("fonts/12-00", 64), nullptr); // gap before frame 65
	EXPECT_EQ(index.find("fonts/12-00", 67), nullptr);
	EXPECT_NE(index.find("data/inv/objcurs", 3), nullptr); // keys are normalised
	EXPECT_EQ(index.find("ctrlpan/panel8", 1), nullptr);   // unknown sheet: rejected
	EXPECT_EQ(index.find("ctrlpan/panel8", 2), nullptr);   // outside its sheet: rejected
	EXPECT_EQ(index.errors.size(), 4U);
}

TEST(HdUiIndexTest, NormalisesKeys)
{
	EXPECT_EQ(hd_ui::NormalizeAssetKey("ctrlpan\\Panel8"), "ctrlpan/panel8");
	EXPECT_EQ(hd_ui::NormalizeAssetKey("data\\inv\\objcurs.clx"), "data/inv/objcurs");
	EXPECT_EQ(hd_ui::NormalizeAssetKey("ui_art\\mainmenu.PCX"), "ui_art/mainmenu");
	EXPECT_EQ(hd_ui::NormalizeAssetKey("data/foo.clx#2"), "data/foo#2");
	EXPECT_EQ(hd_ui::NormalizeAssetKey("fonts\\12-00.clx"), "fonts/12-00");
	EXPECT_TRUE(hd_ui::IsUiAssetKey("fonts/12-00"));
	EXPECT_TRUE(hd_ui::IsUiAssetKey("items/duricons"));
	EXPECT_FALSE(hd_ui::IsUiAssetKey("monsters/zombie/zombiew"));
	EXPECT_FALSE(hd_ui::IsUiAssetKey("items/armor2"));
	EXPECT_TRUE(hd_ui::IsUiAssetKey("ui_art/mainmenu"));
	EXPECT_FALSE(hd_ui::IsUiAssetKey("ui_art/focus"));
	EXPECT_FALSE(hd_ui::IsUiAssetKey("ui_art/focus16"));
	EXPECT_FALSE(hd_ui::IsUiAssetKey("ui_art/focus42"));
}

TEST(HdUiIndexTest, InternsKeys)
{
	const int a = hd_ui::InternKey("test/a");
	const int b = hd_ui::InternKey("test/b");
	EXPECT_NE(a, b);
	EXPECT_EQ(hd_ui::InternKey("test/a"), a);
	EXPECT_EQ(hd_ui::KeyName(b), "test/b");
	EXPECT_EQ(hd_ui::KeyName(-1), "");
}

/** A 4x3 CLX frame: row 2 (bottom) = 4 opaque, row 1 = skip 1 + fill 2 + skip 1, row 0 = skip 3 + 1 opaque. */
std::vector<uint8_t> SampleClx()
{
	return {
		0xFC, 1, 2, 3, 4,  // 4 literal pixels (bottom row)
		0x01,              // 1 transparent
		0xBD, 7,           // fill 2 (0xBF - 0xBD) with colour 7
		0x04,              // 1 transparent (end of row 1) + 3 transparent (row 0)
		0xFF, 9,           // 1 literal
	};
}

TEST(HdUiMaskTest, DecodesClxOpacity)
{
	const std::vector<uint8_t> clx = SampleClx();
	const Mask mask = ClxOpaqueMask(clx.data(), clx.size(), 4, 3, 10, 20, Rect { 0, 0, 100, 100 });
	EXPECT_EQ(mask.rect, (Rect { 10, 20, 4, 3 }));
	const std::array<std::array<int, 4>, 3> expected { { { 0, 0, 0, 1 }, { 0, 1, 1, 0 }, { 1, 1, 1, 1 } } };
	for (int y = 0; y < 3; ++y) {
		for (int x = 0; x < 4; ++x)
			EXPECT_EQ(mask.at(10 + x, 20 + y), expected[y][x] != 0) << x << "," << y;
	}
	EXPECT_EQ(mask.count(), 7U);
}

TEST(HdUiMaskTest, ClipsTheMask)
{
	const std::vector<uint8_t> clx = SampleClx();
	const Mask mask = ClxOpaqueMask(clx.data(), clx.size(), 4, 3, 10, 20, Rect { 11, 21, 10, 10 });
	EXPECT_EQ(mask.rect, (Rect { 11, 21, 3, 2 }));
	EXPECT_TRUE(mask.at(11, 21));
	EXPECT_FALSE(mask.at(10, 22)); // clipped away
	EXPECT_EQ(mask.count(), 5U);
}

TEST(HdUiMaskTest, Covers)
{
	const Mask full = Mask::Full(Rect { 0, 0, 4, 4 });
	Mask part = Mask::Full(Rect { 1, 1, 2, 2 });
	EXPECT_TRUE(full.covers(part));
	EXPECT_FALSE(part.covers(full));
	part.bits[0] = 0;
	const Mask cut = full.intersected(part.translated(0, 0));
	EXPECT_EQ(cut.count(), 3U);
}

PlacedMasked Image(int key, Rect rect, Mask mask)
{
	PlacedMasked p;
	p.placed.keyId = key;
	p.placed.rect = rect;
	p.placed.clip = rect;
	p.mask = std::move(mask);
	return p;
}

struct Buffer {
	int width = 16;
	int height = 8;
	std::vector<uint8_t> pixels = std::vector<uint8_t>(16 * 8, 0);
	void fill(Rect r, uint8_t v)
	{
		for (int y = r.y; y < r.bottom(); ++y)
			for (int x = r.x; x < r.right(); ++x)
				pixels[y * width + x] = v;
	}
	[[nodiscard]] uint16_t plane(const LayerTracker &t, int x, int y) const { return t.plane()[y * width + x]; }
};

const auto AllDrawable = [](const LayerTracker::Layer &) { return true; };

TEST(HdUiLayerTest, ClassicDrawsLaterHideTheLayer)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect panel { 0, 0, 16, 8 };
	buffer.fill(panel, 5);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, panel, Mask::Full(panel)));
	buffer.fill(Rect { 2, 2, 2, 2 }, 9); // classic-only text
	tracker.resolve(buffer.pixels.data(), buffer.width, AllDrawable);
	ASSERT_EQ(tracker.layers().size(), 1U);
	EXPECT_EQ(buffer.plane(tracker, 0, 0), 0); // panel visible
	EXPECT_EQ(buffer.plane(tracker, 2, 2), 1); // hidden under the classic text
}

TEST(HdUiLayerTest, HdDrawsLaterKeepTheLayerBelow)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect panel { 0, 0, 16, 8 };
	buffer.fill(panel, 5);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, panel, Mask::Full(panel)));
	const Rect glyph { 2, 2, 2, 2 };
	buffer.fill(glyph, 9);
	tracker.record(buffer.pixels.data(), buffer.width, Image(2, glyph, Mask::Full(glyph)));
	tracker.resolve(buffer.pixels.data(), buffer.width, AllDrawable);
	ASSERT_EQ(tracker.layers().size(), 2U);
	EXPECT_EQ(buffer.plane(tracker, 2, 2), 0); // both drawn: the glyph layer on top of the panel layer
	// A glyph whose art is not resident counts as classic: the panel is hidden under its pixels (and the glyph is not
	// drawn), so the classic pixel shows there.
	tracker.resolve(buffer.pixels.data(), buffer.width, [](const LayerTracker::Layer &layer) { return layer.placed.keyId != 2; });
	EXPECT_EQ(buffer.plane(tracker, 2, 2), 2);
}

TEST(HdUiLayerTest, UndrawableLayersFromOneBlitHideTheLayersBelow)
{
	// A panel buffer blit replays the panel and a button face drawn over it in one go: both snapshots are taken after
	// the blit, so only the face's own mask can tell that the panel is covered there.
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect panel { 0, 0, 16, 8 };
	const Rect face { 4, 2, 6, 3 };
	buffer.fill(panel, 5);
	buffer.fill(face, 6);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, panel, Mask::Full(panel)));
	tracker.record(buffer.pixels.data(), buffer.width, Image(2, face, Mask::Full(face)));
	tracker.resolve(buffer.pixels.data(), buffer.width, [](const LayerTracker::Layer &layer) { return layer.placed.keyId != 2; });
	ASSERT_EQ(tracker.layers().size(), 2U);
	EXPECT_EQ(buffer.plane(tracker, 5, 3), 2); // the face has no HD art: classic shows, the HD panel is cut out
	EXPECT_EQ(buffer.plane(tracker, 1, 1), 0);
	EXPECT_EQ(tracker.drawableFlags()[1], 0);
}

TEST(HdUiLayerTest, CoveredLayersAreDropped)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect box { 4, 2, 4, 4 };
	buffer.fill(box, 3);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, box, Mask::Full(box)));
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, box, Mask::Full(box))); // redrawn next frame
	EXPECT_EQ(tracker.layers().size(), 1U);
	Mask sparse = Mask::Full(box);
	sparse.bits[0] = 0;
	tracker.record(buffer.pixels.data(), buffer.width, Image(3, box, sparse)); // grime overlay: keeps the face below
	EXPECT_EQ(tracker.layers().size(), 2U);
}

TEST(HdUiLayerTest, OverwrittenLayersAreDropped)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect box { 0, 0, 8, 8 };
	buffer.fill(box, 3);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, box, Mask::Full(box)));
	buffer.fill(Rect { 0, 0, 8, 7 }, 4); // the world repainted almost all of it
	tracker.resolve(buffer.pixels.data(), buffer.width, AllDrawable);
	EXPECT_TRUE(tracker.layers().empty());
}

TEST(HdUiLayerTest, InvalidateDropsIntersectingLayers)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, Rect { 0, 0, 4, 4 }, Mask::Full(Rect { 0, 0, 4, 4 })));
	tracker.record(buffer.pixels.data(), buffer.width, Image(2, Rect { 8, 4, 4, 4 }, Mask::Full(Rect { 8, 4, 4, 4 })));
	tracker.invalidate(Rect { 0, 0, 16, 4 });
	ASSERT_EQ(tracker.layers().size(), 1U);
	EXPECT_EQ(tracker.layers()[0].placed.keyId, 2);
}

TEST(HdUiLayerTest, PlaneIsRenumberedAfterDrops)
{
	Buffer buffer;
	LayerTracker tracker;
	tracker.reset(buffer.width, buffer.height);
	const Rect left { 0, 0, 4, 8 };
	const Rect right { 8, 0, 8, 8 };
	buffer.fill(left, 1);
	tracker.record(buffer.pixels.data(), buffer.width, Image(1, left, Mask::Full(left)));
	buffer.fill(right, 2);
	tracker.record(buffer.pixels.data(), buffer.width, Image(2, right, Mask::Full(right)));
	buffer.fill(left, 7);                  // layer 0 gone
	buffer.fill(Rect { 9, 1, 1, 1 }, 8); // classic dot over layer 1
	tracker.resolve(buffer.pixels.data(), buffer.width, AllDrawable);
	ASSERT_EQ(tracker.layers().size(), 1U);
	EXPECT_EQ(buffer.plane(tracker, 9, 1), 1); // was 2 (old index 1 + 1), now 1 after the drop
	EXPECT_EQ(buffer.plane(tracker, 10, 1), 0);
}

TEST(HdUiOffscreenTest, CollectsClippedAndMoved)
{
	hd_ui::OffscreenStore store;
	const int surface = 0;
	store.add(&surface, Image(1, Rect { 0, 0, 640, 144 }, Mask::Full(Rect { 0, 0, 640, 144 })));
	store.add(&surface, Image(2, Rect { 10, 20, 71, 19 }, Mask::Full(Rect { 10, 20, 71, 19 })));
	const std::vector<PlacedMasked> got = store.collect(&surface, Rect { 0, 16, 100, 20 }, 5, 300);
	ASSERT_EQ(got.size(), 2U);
	EXPECT_EQ(got[0].placed.rect, (Rect { 5, 300, 640, 144 }));
	EXPECT_EQ(got[0].placed.clip, (Rect { 5, 316, 100, 20 }));
	EXPECT_EQ(got[1].placed.clip, (Rect { 15, 320, 71, 16 }));
	EXPECT_EQ(got[1].mask.rect, got[1].placed.clip);
	store.add(&surface, Image(3, Rect { 0, 0, 640, 144 }, Mask::Full(Rect { 0, 0, 640, 144 }))); // repaint covers both
	EXPECT_EQ(store.size(&surface), 1U);
	store.reset(&surface);
	EXPECT_FALSE(store.has(&surface));
}

TEST(HdUiRegistryTest, FindsByPointerAndFingerprint)
{
	hd_ui::SpriteRegistry registry;
	std::vector<uint8_t> frame { 6, 0, 4, 0, 3, 0, 0xFC, 1, 2, 3, 4 };
	const hd_ui::SpriteFingerprint fp = hd_ui::FingerprintOf(frame.data(), static_cast<uint32_t>(frame.size()));
	EXPECT_EQ(fp.width, 4);
	EXPECT_EQ(fp.height, 3);
	registry.registerFrame(frame.data(), fp, 7, 2);
	const auto *entry = registry.find(frame.data(), fp);
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->keyId, 7);
	EXPECT_EQ(entry->frame, 2);
	hd_ui::SpriteFingerprint other = fp;
	other.width = 5;
	EXPECT_EQ(registry.find(frame.data(), other), nullptr); // a reused address with other content
	registry.forget(frame.data(), frame.size());
	EXPECT_EQ(registry.find(frame.data(), fp), nullptr);
}

TEST(HdUiTrnTest, PoolDeduplicatesByContent)
{
	hd_ui::TrnPool pool;
	std::array<uint8_t, 256> a {};
	std::array<uint8_t, 256> b {};
	for (int i = 0; i < 256; ++i) {
		a[i] = static_cast<uint8_t>(i);
		b[i] = static_cast<uint8_t>(255 - i);
	}
	EXPECT_EQ(pool.intern(a.data()), 0);
	EXPECT_EQ(pool.intern(b.data()), 1);
	std::array<uint8_t, 256> copy = a;
	EXPECT_EQ(pool.intern(copy.data()), 0);
	a[3] = 9; // modified in place (SetSpellTrans): a new table
	EXPECT_EQ(pool.intern(a.data()), 2);
}

TEST(HdUiTrnTest, LookupTableAppliesTheTranslation)
{
	std::array<uint8_t, 768> palette {};
	// 0: black, 1: grey, 2: red, 3: dark red, rest black
	palette[3] = palette[4] = palette[5] = 128;
	palette[6] = 255;
	palette[9] = 100;
	std::array<uint8_t, 256> identity {};
	std::array<uint8_t, 256> toRed {};
	for (int i = 0; i < 256; ++i) {
		identity[i] = static_cast<uint8_t>(i);
		toRed[i] = static_cast<uint8_t>(i);
	}
	toRed[1] = 2; // grey -> red
	const auto at = [](const std::vector<uint8_t> &lut, int r, int g, int b) {
		const size_t i = ((static_cast<size_t>(b) * hd_ui::TrnLutSize + g) * hd_ui::TrnLutSize + r) * 4;
		return std::array<int, 3> { lut[i], lut[i + 1], lut[i + 2] };
	};
	const std::vector<uint8_t> same = hd_ui::BuildTrnLut(identity, palette);
	EXPECT_EQ(at(same, 16, 16, 16)[0], 16 * 255 / 31);
	const std::vector<uint8_t> red = hd_ui::BuildTrnLut(toRed, palette);
	const auto grey = at(red, 16, 16, 16); // nearest palette colour: grey -> moved towards red
	EXPECT_GT(grey[0], 200);
	EXPECT_LT(grey[1], 20);
	EXPECT_LT(grey[2], 20);
	EXPECT_EQ(at(red, 0, 0, 0)[0], 0); // black stays black
}

TEST(HdUiMaskTest, RvalueOperationsMatchTheCopies)
{
	const std::vector<uint8_t> clx = SampleClx();
	const Mask mask = ClxOpaqueMask(clx.data(), clx.size(), 4, 3, 10, 20, Rect { 0, 0, 100, 100 });
	const Rect clip { 11, 21, 10, 10 };
	Mask copy = mask;
	const Mask clipped = std::move(copy).clipped(clip);
	EXPECT_EQ(clipped.rect, mask.clipped(clip).rect);
	EXPECT_EQ(clipped.bits, mask.clipped(clip).bits);
	Mask other = Mask::Full(Rect { 12, 19, 5, 3 });
	other.bits[5] = 0;
	Mask copy2 = mask;
	const Mask cut = std::move(copy2).intersected(other);
	EXPECT_EQ(cut.bits, mask.intersected(other).bits);
	EXPECT_EQ(cut.rect, mask.rect);
	for (int y = 0; y < 3; ++y) {
		for (int x = 0; x < 4; ++x)
			EXPECT_EQ(cut.at(10 + x, 20 + y), mask.at(10 + x, 20 + y) && other.at(10 + x, 20 + y)) << x << "," << y;
	}
	EXPECT_TRUE(mask.covers(cut));
	EXPECT_FALSE(cut.covers(mask));
}

/** An indexed sheet holding the 4x art of SampleClx (scale 2 to keep it small) at (2, 1). */
struct IndexedSheet {
	int width = 12;
	int height = 8;
	std::vector<uint8_t> texels = std::vector<uint8_t>(12 * 8 * 4, 0);
	IndexedSheet()
	{
		const std::array<std::array<int, 4>, 3> indices { { { -1, -1, -1, 9 }, { -1, 7, 7, -1 }, { 1, 2, 3, 4 } } };
		for (int cy = 0; cy < 3; ++cy) {
			for (int cx = 0; cx < 4; ++cx) {
				if (indices[cy][cx] < 0)
					continue;
				for (int k = 0; k < 4; ++k) {
					uint8_t *t = &texels[(static_cast<size_t>(1 + cy * 2 + k / 2) * width + 2 + cx * 2 + k % 2) * 4];
					t[0] = static_cast<uint8_t>(indices[cy][cx]);
					t[1] = 255;
					t[3] = 255;
				}
			}
		}
	}
};

TEST(HdUiIndexedSheetTest, AcceptsTheClassicIndices)
{
	const std::vector<uint8_t> clx = SampleClx();
	IndexedSheet sheet;
	const std::array<hd_ui::IndexedSheetFrame, 1> frames { { { hd_ui::FrameInfo { 0, 2, 1, 8, 6 }, clx.data(), clx.size(), 4, 3 } } };
	const hd_ui::IndexedSheetReport report = hd_ui::CheckIndexedSheet(sheet.texels.data(), sheet.width * 4, sheet.width, sheet.height, 2, frames);
	EXPECT_TRUE(report.ok);
	EXPECT_EQ(report.compared, 7U);
	EXPECT_EQ(report.matched, 7U);
	EXPECT_EQ(report.covered, 28U);
}

TEST(HdUiIndexedSheetTest, RejectsShiftedOrMissingTexels)
{
	const std::vector<uint8_t> clx = SampleClx();
	const std::array<hd_ui::IndexedSheetFrame, 1> frames { { { hd_ui::FrameInfo { 0, 2, 1, 8, 6 }, clx.data(), clx.size(), 4, 3 } } };
	IndexedSheet shifted; // colour management moved the indices
	for (size_t i = 0; i < shifted.texels.size(); i += 4)
		shifted.texels[i] = static_cast<uint8_t>(shifted.texels[i] != 0 ? shifted.texels[i] + 1 : 0);
	EXPECT_FALSE(hd_ui::CheckIndexedSheet(shifted.texels.data(), shifted.width * 4, shifted.width, shifted.height, 2, frames).ok);
	IndexedSheet empty; // the upload failed
	std::fill(empty.texels.begin(), empty.texels.end(), uint8_t { 0 });
	EXPECT_FALSE(hd_ui::CheckIndexedSheet(empty.texels.data(), empty.width * 4, empty.width, empty.height, 2, frames).ok);
	IndexedSheet blended; // alpha that is neither 0 nor 255
	blended.texels[(static_cast<size_t>(1) * blended.width + 8) * 4 + 3] = 128;
	EXPECT_FALSE(hd_ui::CheckIndexedSheet(blended.texels.data(), blended.width * 4, blended.width, blended.height, 2, frames).ok);
}

TEST(HdUiTextureTest, DivisorFollowsTheCanvas)
{
	EXPECT_EQ(hd_ui::SelectUiTextureDivisor(4.0F), 1);
	EXPECT_EQ(hd_ui::SelectUiTextureDivisor(3.0F), 1);
	EXPECT_EQ(hd_ui::SelectUiTextureDivisor(2.0F), 2);
	EXPECT_EQ(hd_ui::SelectUiTextureDivisor(1.0F), 4);
}

} // namespace
} // namespace devilution
