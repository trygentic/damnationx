#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/assets.hpp"
#include "engine/clx_sprite.hpp"
#include "utils/file_util.h"
#include "utils/paths.h"

using namespace devilution;

namespace {

class HasLooseLogicAssetsTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		savedOverridePaths_ = OverridePaths;
		savedAssetsPath_ = paths::AssetsPath();
		OverridePaths.clear();

		const auto *testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
		root_ = std::string("Test_") + testInfo->test_suite_name() + "_" + testInfo->name() + DIRECTORY_SEPARATOR_STR;
		assetsRoot_ = root_ + "fake_assets" DIRECTORY_SEPARATOR_STR;

		// A fake `/assets` directory providing the canonical assets that loose files can shadow.
		CreateFileAt(assetsRoot_ + "levels" DIRECTORY_SEPARATOR_STR "l1data" DIRECTORY_SEPARATOR_STR "l1.sol");
		CreateFileAt(assetsRoot_ + "txtdata" DIRECTORY_SEPARATOR_STR "towners" DIRECTORY_SEPARATOR_STR "towners.tsv");
		paths::SetAssetsPath(assetsRoot_);
	}

	void TearDown() override
	{
		for (const std::string &file : createdFiles_)
			RemoveFile(file.c_str());
		OverridePaths = savedOverridePaths_;
		paths::SetAssetsPath(savedAssetsPath_);
	}

	// Creates an empty file in the override root, creating parent directories as needed.
	void CreateFile(const std::string &relativePath)
	{
		CreateFileAt(root_ + relativePath);
	}

	std::string root_;

private:
	void CreateFileAt(const std::string &path)
	{
		const std::string dir { Dirname(path) };
		RecursivelyCreateDir(dir.c_str());
		FILE *file = OpenFile(path.c_str(), "wb");
		ASSERT_NE(file, nullptr);
		std::fclose(file);
		createdFiles_.push_back(path);
	}

	std::string assetsRoot_;
	std::vector<std::string> savedOverridePaths_;
	std::string savedAssetsPath_;
	std::vector<std::string> createdFiles_;
};

TEST(ClxMalformedInputTest, RejectsTruncatedAndOverflowedTables)
{
	std::array<uint8_t, 8> truncated {};
	EXPECT_FALSE(IsValidClxSpriteList(truncated.data(), truncated.size()));

	std::array<uint8_t, 16> overflow {};
	overflow[0] = 0xFF;
	overflow[1] = 0xFF;
	overflow[2] = 0xFF;
	overflow[3] = 0x7F;
	EXPECT_FALSE(IsValidClxSpriteList(overflow.data(), overflow.size()));
	EXPECT_EQ(GetNumListsFromClxListOrSheetBuffer(overflow.data(), overflow.size()), 0);
}

TEST(ClxSpriteSheetTest, DetectsValidatedSpriteLists)
{
	std::array<uint8_t, 44> sheet {};
	auto write32 = [&sheet](size_t offset, uint32_t value) {
		sheet[offset] = static_cast<uint8_t>(value);
		sheet[offset + 1] = static_cast<uint8_t>(value >> 8);
		sheet[offset + 2] = static_cast<uint8_t>(value >> 16);
		sheet[offset + 3] = static_cast<uint8_t>(value >> 24);
	};
	auto writeList = [&write32, &sheet](size_t offset) {
		write32(offset, 1);
		write32(offset + 4, 12);
		write32(offset + 8, 18);
		sheet[offset + 12] = 6;
		sheet[offset + 14] = 1;
		sheet[offset + 16] = 1;
	};

	write32(0, 8);
	write32(4, 26);
	writeList(8);
	writeList(26);

	EXPECT_EQ(GetNumListsFromClxListOrSheetBuffer(sheet.data(), sheet.size()), 2);
}

TEST_F(HasLooseLogicAssetsTest, NoOverridePaths)
{
	EXPECT_FALSE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, OnlyNonLogicFiles)
{
	CreateFile("readme.txt");
	CreateFile("sub" DIRECTORY_SEPARATOR_STR "sprite.clx");
	CreateFile("tsv"); // Extension-less name shorter than the extensions being matched.
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
}

#ifndef UNPACKED_MPQS
TEST_F(HasLooseLogicAssetsTest, LuaModEntryPoint)
{
	CreateFile("lua" DIRECTORY_SEPARATOR_STR "mods" DIRECTORY_SEPARATOR_STR "cheat" DIRECTORY_SEPARATOR_STR "init.lua");
	OverridePaths.push_back(root_);
	EXPECT_TRUE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, LuaOutsideLuaNamespaceIsInert)
{
	// The game only requests lua modules under `lua\`, so this file can never be loaded.
	CreateFile("cheat.lua");
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, TsvShadowingExistingAsset)
{
	CreateFile("txtdata" DIRECTORY_SEPARATOR_STR "towners" DIRECTORY_SEPARATOR_STR "towners.tsv");
	OverridePaths.push_back(root_);
	EXPECT_TRUE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, SolShadowingExistingAsset)
{
	CreateFile("levels" DIRECTORY_SEPARATOR_STR "l1data" DIRECTORY_SEPARATOR_STR "l1.sol");
	OverridePaths.push_back(root_);
	EXPECT_TRUE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, BackupDirectoryIsInert)
{
	// A renamed directory does not shadow any asset the game requests.
	CreateFile("levels.bak" DIRECTORY_SEPARATOR_STR "l1data" DIRECTORY_SEPARATOR_STR "l1.sol");
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, UppercaseLuaIsFlagged)
{
	CreateFile("LUA" DIRECTORY_SEPARATOR_STR "MODS" DIRECTORY_SEPARATOR_STR "CHEAT" DIRECTORY_SEPARATOR_STR "INIT.LUA");
	OverridePaths.push_back(root_);
	EXPECT_TRUE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, CaseCollisionWithCanonicalAssetIsFlagged)
{
	// Case-folding must detect an override even when the host filesystem is case-sensitive.
	CreateFile("TXTDATA" DIRECTORY_SEPARATOR_STR "TOWNERS" DIRECTORY_SEPARATOR_STR "TOWNERS.TSV");
	OverridePaths.push_back(root_);
	EXPECT_TRUE(HasLooseLogicAssets());
	// The rejected/overridden candidate does not remove the canonical asset.
	EXPECT_TRUE(FileExists((paths::AssetsPath() + "txtdata" DIRECTORY_SEPARATOR_STR "towners" DIRECTORY_SEPARATOR_STR "towners.tsv").c_str()));
}

TEST_F(HasLooseLogicAssetsTest, InactiveLooseModsAreInert)
{
	// Inactive loose mods under `mods/` are not override roots and their paths are never
	// requested relative to the pref path. Active loose mods get their own override root,
	// under which their lua files sit at `lua/mods/<name>/...` and are caught by the lua rule.
	CreateFile("mods" DIRECTORY_SEPARATOR_STR "inactive" DIRECTORY_SEPARATOR_STR "lua" DIRECTORY_SEPARATOR_STR "mods" DIRECTORY_SEPARATOR_STR "inactive" DIRECTORY_SEPARATOR_STR "init.lua");
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, DeepExpansionLikeTreeIsBounded)
{
	// A hostile package can contain an arbitrarily deep directory tree. The integrity scan must
	// stop before recursive processing becomes unbounded, and must not turn an unrelated file into
	// a valid logic override.
	std::string path = "deep";
	for (unsigned depth = 0; depth != 20; ++depth)
		path += DIRECTORY_SEPARATOR_STR "x";
	path += DIRECTORY_SEPARATOR_STR "init.lua";
	CreateFile(path);
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
}

TEST_F(HasLooseLogicAssetsTest, NonCanonicalAndTraversalLikeNamesAreInert)
{
	// Names that resemble traversal or package metadata are not requests made by the engine.
	// They must not replace the canonical asset supplied by the fake assets root.
	CreateFile("package" DIRECTORY_SEPARATOR_STR ".._evil.lua");
	CreateFile("txtdata" DIRECTORY_SEPARATOR_STR "towners" DIRECTORY_SEPARATOR_STR "towners.tsv.bak");
	OverridePaths.push_back(root_);
	EXPECT_FALSE(HasLooseLogicAssets());
	EXPECT_TRUE(FileExists((paths::AssetsPath() + "txtdata" DIRECTORY_SEPARATOR_STR "towners" DIRECTORY_SEPARATOR_STR "towners.tsv").c_str()));
}
#endif // !UNPACKED_MPQS

} // namespace
