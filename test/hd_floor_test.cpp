#include <gtest/gtest.h>

#include "engine/render/hd_floor.hpp"

namespace devilution {
namespace {

TEST(HdFloorManifest, ParsesPagesAndPieces)
{
	const auto manifest = ParseHdFloorManifest(
	    "\xEF\xBB\xBF# DamnationX HD town floor\n"
	    "atlas 1 town-floor-atlas-1.png 4096 3328  # short page\r\n"
	    "atlas 0 hd/town-floor-atlas-0.png 4096 4096\n"
	    "\n"
	    "piece 0 0 0\n"
	    "piece\t7\t1\t415\n"
	    "piece 3 0 511\n");
	ASSERT_TRUE(manifest.has_value()) << manifest.error();
	ASSERT_EQ(manifest->pages.size(), 2U);
	EXPECT_EQ(manifest->pages[0].path, "hd\\town-floor-atlas-0.png");
	EXPECT_EQ(manifest->pages[1].path, "hd\\town-floor-atlas-1.png");
	EXPECT_EQ(manifest->pages[1].height, 3328);
	EXPECT_EQ(manifest->MaxPageHeight(), 4096);

	ASSERT_TRUE(manifest->Lookup(0).has_value());
	EXPECT_EQ(manifest->Lookup(0)->page, 0);
	EXPECT_EQ(manifest->Lookup(0)->cell, 0);
	ASSERT_TRUE(manifest->Lookup(7).has_value());
	EXPECT_EQ(manifest->Lookup(7)->page, 1);
	EXPECT_EQ(manifest->Lookup(7)->cell, 415);
	EXPECT_EQ(manifest->Lookup(3)->cell, 511);
	EXPECT_FALSE(manifest->Lookup(1).has_value());
	EXPECT_FALSE(manifest->Lookup(8).has_value());
	EXPECT_FALSE(manifest->Lookup(60000).has_value());
}

TEST(HdFloorManifest, RejectsMalformedInput)
{
	const char *const invalid[] = {
		"",                                                            // no pages
		"atlas 0 a.png 2048 4096\n",                                   // wrong width
		"atlas 0 a.png 4096 4100\n",                                   // height too large
		"atlas 0 a.png 4096 100\n",                                    // height not a cell multiple
		"atlas 1 a.png 4096 4096\n",                                   // page 0 missing
		"atlas 0 a.png 4096 4096\natlas 0 b.png 4096 4096\n",          // page defined twice
		"atlas 64 a.png 4096 4096\n",                                  // page index too large
		"atlas 0 a.png 4096 128\npiece 1 0 16\n",                      // cell outside a short page
		"atlas 0 a.png 4096 4096\npiece 1 1 0\n",                      // undefined page
		"atlas 0 a.png 4096 4096\npiece 1 0 0\npiece 1 0 1\n",         // duplicate piece
		"atlas 0 a.png 4096 4096\npiece -1 0 0\n",                     // negative piece
		"atlas 0 a.png 4096 4096\npiece 70000 0 0\n",                  // piece beyond uint16
		"atlas 0 a.png 4096 4096\npiece 1 0\n",                        // missing field
		"atlas 0 a.png 4096 4096\npiece 1 0 x\n",                      // not a number
		"atlas 0 a.png 4096 4096\ntile 1 0 0\n",                       // unknown directive
	};
	for (const char *text : invalid) {
		EXPECT_FALSE(ParseHdFloorManifest(text).has_value()) << text;
	}
}

TEST(HdFloorManifest, ErrorNamesLine)
{
	const auto manifest = ParseHdFloorManifest("atlas 0 a.png 4096 4096\n# comment\npiece 1 0 512\n");
	ASSERT_FALSE(manifest.has_value());
	EXPECT_NE(manifest.error().find("line 3"), std::string::npos) << manifest.error();
}

} // namespace
} // namespace devilution
