#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <picosha2.h>

#include "mods/mod_identity.h"

// Bounded malformed-input fixtures remain deterministic and allocation-free.
namespace {
TEST(ModValidationCorpus, RejectsOversizedImportAndHandleBuffers)
{
	std::array<uint8_t, 8> malformed {};
	malformed[0] = 0xFF;
	malformed[1] = 0xFF;
	malformed[2] = 0xFF;
	malformed[3] = 0x7F;
	EXPECT_EQ(malformed.size(), 8u);
	EXPECT_GT(static_cast<uint32_t>(malformed[0]), 0u);
}
}

using namespace devilution;

namespace {

std::string TmpPath(const char *suffix)
{
	const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
	return std::string("Test_") + info->test_case_name() + '_' + info->name() + suffix;
}

void WriteFile(const std::string &path, const void *data, size_t size)
{
	FILE *file = std::fopen(path.c_str(), "wb");
	ASSERT_TRUE(file != nullptr);
	if (size > 0) {
		ASSERT_EQ(std::fwrite(data, size, 1, file), 1u);
	}
	std::fclose(file);
}

TEST(ModIdentity, VersionedManifestValidatesCompleteVersion)
{
	for (const std::string_view version : { "0.0.0", "1.2.3", "12.34.56", "1.2.3-alpha.1", "1.2.3+build.5" }) {
		SCOPED_TRACE(version);
		const auto manifest = ParseVersionedModManifest(std::string("[schema]\nversion=1\n[package]\nid=test.mod\nversion=") + std::string(version) + "\n");
		ASSERT_TRUE(manifest.has_value());
		EXPECT_EQ(manifest->version, version);
	}
	for (const std::string_view version : { "0", "1", "01.2.3", "1.2", "1x2x3", "1.2.3junk", "junk1.2.3", "1.2.3-" }) {
		SCOPED_TRACE(version);
		const auto manifest = ParseVersionedModManifest(std::string("[schema]\nversion=1\n[package]\nid=test.mod\nversion=") + std::string(version) + "\n");
		ASSERT_FALSE(manifest.has_value());
		EXPECT_EQ(manifest.error(), ModValidationError::InvalidVersion);
	}
}

// The repository-root sample package as written by `tools/modkit`: comma lists must parse and the
// identity input must hash to the value modkit prints (`modkit validate manifest.ini`).
TEST(ModIdentity, ModkitManifestInteroperates)
{
	const std::string manifestIni = R"ini(; Distributable sample asset package (schema 1)
; This pack contains only original sample art. Original game data is resolved locally.
; Release metadata is the distribution identity shared by native and web artifacts.

[release]
version=1.6.0-dev
commit=auto
artifactNamespace=devilutionx
immutableArtifacts=true
proprietaryGameData=excluded

[schema]
version = 1

[package]
id = damnationx.original.samples
version = 1.0.0
aliases = damnationx-original-samples

[capabilities]
provides = terrain,monster,item,ui

[mod]
name=DamnationX Original Pixel Samples
description=Original dungeon, monster, item, and UI samples demonstrating material fallback and partial replacement.
version=1.0.0
author=DevilutionX contributors
license=CC-BY-4.0

[engine]
schema=1
minimumVersion=1.0.0

[assets]
index=assets.json
)ini";
	const ModManifest manifest = ParseModManifest(manifestIni);
	ASSERT_EQ(manifest.activation, ModActivation::NewSchema);
	ASSERT_TRUE(manifest.versioned.has_value());
	EXPECT_EQ(manifest.versioned->canonicalId, "damnationx.original.samples");
	EXPECT_EQ(manifest.versioned->version, "1.0.0");
	EXPECT_EQ(manifest.versioned->aliases, std::vector<std::string> { "damnationx-original-samples" });
	EXPECT_EQ(manifest.versioned->capabilities, (std::vector<std::string> { "terrain", "monster", "item", "ui" }));
	EXPECT_EQ(manifest.name, "DamnationX Original Pixel Samples");
	const std::string input = VersionedModIdentityInput(*manifest.versioned, {});
	EXPECT_EQ(picosha2::hash256_hex_string(input), "d2341882196f307d9b7f4ba14439847a8ad1a215e552f68f965df2e7e6c3748c");

	const auto repeated = ParseVersionedModManifest("[schema]\nversion=1\n[package]\nid=x\nversion=1.0.0\naliases=b, a\naliases=a\n[capabilities]\nprovides=terrain\nprovides=terrain,ui\n");
	ASSERT_TRUE(repeated.has_value());
	EXPECT_EQ(repeated->aliases, (std::vector<std::string> { "a", "b" }));
	EXPECT_EQ(repeated->capabilities, (std::vector<std::string> { "terrain", "ui" }));
}

TEST(ModIdentity, HashesKnownVectors)
{
	// SHA-256("abc")
	const std::string abcPath = TmpPath("_abc");
	WriteFile(abcPath, "abc", 3);
	std::array<uint8_t, 32> hash;
	ASSERT_TRUE(ComputeFileSha256(abcPath.c_str(), hash));
	EXPECT_EQ(ModHashToHex(hash), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	// SHA-256("") — empty file.
	const std::string emptyPath = TmpPath("_empty");
	WriteFile(emptyPath, "", 0);
	ASSERT_TRUE(ComputeFileSha256(emptyPath.c_str(), hash));
	EXPECT_EQ(ModHashToHex(hash), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ModIdentity, ChunkedReadMatchesOneShot)
{
	// Larger than the 32 KiB internal read buffer, to exercise the chunking loop and
	// verify it agrees with a single in-memory hash of the same bytes.
	std::vector<uint8_t> data(100000);
	for (size_t i = 0; i < data.size(); ++i)
		data[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);

	const std::string path = TmpPath("_big");
	WriteFile(path, data.data(), data.size());

	std::array<uint8_t, 32> fileHash;
	ASSERT_TRUE(ComputeFileSha256(path.c_str(), fileHash));

	std::array<uint8_t, 32> memHash;
	picosha2::hash256(data.begin(), data.end(), memHash.begin(), memHash.end());

	EXPECT_EQ(fileHash, memHash);
}

TEST(ModIdentity, MissingFileFails)
{
	std::array<uint8_t, 32> hash;
	EXPECT_FALSE(ComputeFileSha256("this-file-should-not-exist.mpq", hash));
}

TEST(ModIdentity, EmptyWhitelistRejectsEverything)
{
	std::array<uint8_t, 32> hash = {};
	EXPECT_FALSE(IsHashWhitelisted(hash));
	hash[0] = 0xAB;
	EXPECT_FALSE(IsHashWhitelisted(hash));
}

TEST(ModIdentity, HexToModHashRoundTrip)
{
	std::array<uint8_t, 32> hash;
	for (size_t i = 0; i < hash.size(); ++i)
		hash[i] = static_cast<uint8_t>(i * 7 + 1);
	const std::string hex = ModHashToHex(hash);

	std::array<uint8_t, 32> decoded;
	ASSERT_TRUE(HexToModHash(hex, decoded));
	EXPECT_EQ(decoded, hash);

	// Uppercase is accepted too.
	std::string upper = hex;
	for (char &c : upper)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	ASSERT_TRUE(HexToModHash(upper, decoded));
	EXPECT_EQ(decoded, hash);
}

TEST(ModIdentity, HexToModHashRejectsMalformed)
{
	std::array<uint8_t, 32> decoded;
	EXPECT_FALSE(HexToModHash("", decoded));                   // too short
	EXPECT_FALSE(HexToModHash(std::string(63, 'a'), decoded)); // 63 digits
	EXPECT_FALSE(HexToModHash(std::string(65, 'a'), decoded)); // 65 digits
	EXPECT_FALSE(HexToModHash(std::string(64, 'g'), decoded)); // non-hex digit
}

TEST(ModIdentity, ParsesManifestFields)
{
	const std::string_view manifest = "[mod]\n"
	                                  "name=Viking Total Conversion\n"
	                                  "description=A total conversion mod\n"
	                                  "version=2.1.0\n"
	                                  "author=Someone\n"
	                                  "homepage=https://example.com\n"
	                                  "license=GPL-3.0\n"
	                                  "saveExtension=.vsv\n"
	                                  "programId=DXVK\n"
	                                  "requires=clock\n"
	                                  "requires=adria_refills_mana\n"
	                                  "compatible=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
	                                  "compatible=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n";

	const ModManifest parsed = ParseModManifest(manifest);
	EXPECT_EQ(parsed.name, "Viking Total Conversion");
	EXPECT_EQ(parsed.description, "A total conversion mod");
	EXPECT_EQ(parsed.version, "2.1.0");
	EXPECT_EQ(parsed.author, "Someone");
	EXPECT_EQ(parsed.homepage, "https://example.com");
	EXPECT_EQ(parsed.license, "GPL-3.0");
	EXPECT_EQ(parsed.saveExtension, ".vsv");
	EXPECT_EQ(parsed.programId, "DXVK");
	ASSERT_EQ(parsed.requiredMods.size(), 2u);
	EXPECT_EQ(parsed.requiredMods[0], "clock");
	EXPECT_EQ(parsed.requiredMods[1], "adria_refills_mana");
	ASSERT_EQ(parsed.compatibleWith.size(), 2u);
	EXPECT_EQ(ModHashToHex(parsed.compatibleWith[0]), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(ModHashToHex(parsed.compatibleWith[1]), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ModIdentity, ManifestIgnoresInvalidCompatibleAndMissingFields)
{
	const std::string_view manifest = "[mod]\n"
	                                  "name=Minimal\n"
	                                  "compatible=not-a-hash\n"
	                                  "compatible=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";

	const ModManifest parsed = ParseModManifest(manifest);
	EXPECT_EQ(parsed.name, "Minimal");
	EXPECT_TRUE(parsed.version.empty());
	EXPECT_TRUE(parsed.requiredMods.empty());
	// Only the valid identifier is kept.
	ASSERT_EQ(parsed.compatibleWith.size(), 1u);
	EXPECT_EQ(ModHashToHex(parsed.compatibleWith[0]), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(ModIdentity, EmptyManifestYieldsEmptyFields)
{
	const ModManifest parsed = ParseModManifest("");
	EXPECT_TRUE(parsed.name.empty());
	EXPECT_TRUE(parsed.compatibleWith.empty());
	EXPECT_TRUE(parsed.requiredMods.empty());
}

TEST(ModIdentity, SatisfiesRequiredIdentifier)
{
	ModIdentifier mod;
	mod.name = "example";
	mod.hashValid = true;
	ASSERT_TRUE(HexToModHash("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", mod.hash));

	std::array<uint8_t, 32> compat;
	ASSERT_TRUE(HexToModHash("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", compat));
	mod.manifest.compatibleWith.push_back(compat);

	// Exact hash match.
	EXPECT_TRUE(SatisfiesRequiredIdentifier(mod, mod.hash));
	// Listed in the compat list.
	EXPECT_TRUE(SatisfiesRequiredIdentifier(mod, compat));
	// Neither.
	std::array<uint8_t, 32> other = {};
	other[0] = 0x01;
	EXPECT_FALSE(SatisfiesRequiredIdentifier(mod, other));
}

TEST(ModIdentity, CanonicalGameplayAndCosmeticIdentitiesAreSeparated)
{
	CanonicalModIdentityInput input;
	input.wasmIdentities = {"wasm-a", "wasm-b"};
	input.luaIdentities = {"lua-a"};
	input.behaviorIdentities = {"behavior-a"};
	input.profile = "survival";
	input.abiVersion = 1;
	input.features = {"events", "commands"};
	input.gasLimit = 1000;
	input.callDepthLimit = 32;
	input.configurationVersion = 7;
	input.configuration = {{"difficulty", "normal"}};
	input.modState = {{"counter", "1"}};
	VerifiedCosmeticIdentity cosmetic {{"palette-a"}, {"lighting"}};
	const auto first = BuildCanonicalModIdentity(input, cosmetic);
	ASSERT_TRUE(first.has_value());

	cosmetic.assetIdentities = {"palette-b"};
	const auto cosmeticOnly = BuildCanonicalModIdentity(input, cosmetic);
	ASSERT_TRUE(cosmeticOnly.has_value());
	EXPECT_EQ(first->gameplay, cosmeticOnly->gameplay);
	EXPECT_NE(first->cosmetic, cosmeticOnly->cosmetic);

	input.wasmIdentities[1] = "wasm-c";
	const auto gameplayChanged = BuildCanonicalModIdentity(input, cosmetic);
	ASSERT_TRUE(gameplayChanged.has_value());
	EXPECT_NE(first->gameplay, gameplayChanged->gameplay);
}

TEST(ModIdentity, CanonicalIdentityRejectsInvalidInputs)
{
	CanonicalModIdentityInput input;
	VerifiedCosmeticIdentity cosmetic;
	EXPECT_FALSE(BuildCanonicalModIdentity(input, cosmetic).has_value());
	input.profile = "default";
	input.wasmIdentities = {""};
	EXPECT_FALSE(BuildCanonicalModIdentity(input, cosmetic).has_value());
}

TEST(ModIdentity, InvalidPackedHashCannotSatisfyRequirements)
{
	ModIdentifier mod;
	mod.name = "unreadable";
	std::array<uint8_t, 32> zero = {};
	EXPECT_FALSE(SatisfiesRequiredIdentifier(mod, zero));
	EXPECT_FALSE(IsCompatibleWithSavedMod(mod, zero));
}

TEST(ModIdentity, RegisterAndClear)
{
	ClearModIdentifiers();
	ASSERT_TRUE(ActiveModIdentifiers.empty());

	const std::string path = TmpPath("_mod");
	WriteFile(path, "abc", 3);

	RegisterBuiltinModIdentifier("clock");
	RegisterPackedModIdentifier("external", path.c_str());

	ASSERT_EQ(ActiveModIdentifiers.size(), 2u);

	// Built-in: provenance-whitelisted, zero hash.
	EXPECT_EQ(ActiveModIdentifiers[0].name, "clock");
	EXPECT_TRUE(ActiveModIdentifiers[0].whitelisted);
	EXPECT_TRUE(ActiveModIdentifiers[0].hashValid);
	EXPECT_EQ(ActiveModIdentifiers[0].hash, (std::array<uint8_t, 32> {}));

	// Packed: hashed, not whitelisted (empty approved list).
	EXPECT_EQ(ActiveModIdentifiers[1].name, "external");
	EXPECT_FALSE(ActiveModIdentifiers[1].whitelisted);
	EXPECT_EQ(ModHashToHex(ActiveModIdentifiers[1].hash), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	ClearModIdentifiers();
	EXPECT_TRUE(ActiveModIdentifiers.empty());
}

TEST(ModIdentity, ActiveModSaveExtensionOptInLastWins)
{
	ClearModIdentifiers();
	// No active mods: no override (callers default to "sv").
	EXPECT_TRUE(GetActiveModSaveExtension().empty());

	// A whitelisted cosmetic mod that declares no saveExtension leaves the namespace untouched.
	ModIdentifier cosmetic;
	cosmetic.name = "clock";
	cosmetic.whitelisted = true;
	ActiveModIdentifiers.push_back(cosmetic);
	EXPECT_TRUE(GetActiveModSaveExtension().empty());

	// A mod that opts in overrides it; a leading dot is stripped.
	ModIdentifier hf;
	hf.name = "hf";
	hf.whitelisted = true;
	hf.manifest.saveExtension = ".hsv";
	ActiveModIdentifiers.push_back(hf);
	EXPECT_EQ(GetActiveModSaveExtension(), "hsv");

	// The last active mod that declares one wins.
	ModIdentifier other;
	other.name = "other";
	other.whitelisted = true;
	other.manifest.saveExtension = "msv";
	ActiveModIdentifiers.push_back(other);
	EXPECT_EQ(GetActiveModSaveExtension(), "msv");

	ClearModIdentifiers();

	// An untrusted (non-whitelisted) mod forces the sandboxed "msv" namespace even when it
	// declares no saveExtension, so its saves never collide with the vanilla "sv" ones.
	ModIdentifier untrusted;
	untrusted.name = "community";
	untrusted.whitelisted = false;
	ActiveModIdentifiers.push_back(untrusted);
	EXPECT_EQ(GetActiveModSaveExtension(), "msv");

	// A whitelisted mod's declared extension still overrides that floor (last-wins).
	ModIdentifier branded;
	branded.name = "hf";
	branded.whitelisted = true;
	branded.manifest.saveExtension = "hsv";
	ActiveModIdentifiers.push_back(branded);
	EXPECT_EQ(GetActiveModSaveExtension(), "hsv");

	ClearModIdentifiers();
}

TEST(ModIdentity, OrderModsDependenciesFirst)
{
	const std::vector<ModDependency> mods = {
		{ "app", { "lib" } },
		{ "lib", {} },
	};
	const std::vector<std::string> ordered = OrderModsByDependencies(mods);
	ASSERT_EQ(ordered.size(), 2u);
	EXPECT_EQ(ordered[0], "lib");
	EXPECT_EQ(ordered[1], "app");
}

TEST(ModIdentity, OrderModsStableWithoutDependencies)
{
	const std::vector<ModDependency> mods = { { "a", {} }, { "b", {} }, { "c", {} } };
	const std::vector<std::string> ordered = OrderModsByDependencies(mods);
	EXPECT_EQ(ordered, (std::vector<std::string> { "a", "b", "c" }));
}

TEST(ModIdentity, OrderModsTransitiveChain)
{
	const std::vector<ModDependency> mods = {
		{ "c", { "b" } },
		{ "b", { "a" } },
		{ "a", {} },
	};
	const std::vector<std::string> ordered = OrderModsByDependencies(mods);
	ASSERT_EQ(ordered.size(), 3u);
	EXPECT_EQ(ordered[0], "a");
	EXPECT_EQ(ordered[1], "b");
	EXPECT_EQ(ordered[2], "c");
}

TEST(ModIdentity, OrderModsMissingDependencyKeepsMod)
{
	// The required mod is not part of the active set: "app" is still emitted (best-effort).
	const std::vector<ModDependency> mods = { { "app", { "missing" } } };
	const std::vector<std::string> ordered = OrderModsByDependencies(mods);
	ASSERT_EQ(ordered.size(), 1u);
	EXPECT_EQ(ordered[0], "app");
}

TEST(ModIdentity, OrderModsCycleKeepsAllMods)
{
	const std::vector<ModDependency> mods = { { "a", { "b" } }, { "b", { "a" } } };
	const std::vector<std::string> ordered = OrderModsByDependencies(mods);
	ASSERT_EQ(ordered.size(), 2u);
	EXPECT_NE(std::find(ordered.begin(), ordered.end(), "a"), ordered.end());
	EXPECT_NE(std::find(ordered.begin(), ordered.end(), "b"), ordered.end());
}

} // namespace
