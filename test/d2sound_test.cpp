#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <Aulib/Stream.h>
#include <SDL.h>
#endif

#ifdef DEVILUTIONX_AULIB_HAS_DRFLAC
#include <Aulib/DecoderDrflac.h>
#endif

#include "d2sound/d2_mpq.hpp"
#include "d2sound/d2sound_import.hpp"
#include "engine/sound.h"
#include "utils/audio_format.hpp"
#include "utils/sdl_compat.h"
#include "utils/soundsample.h"
#include "utils/stdcompat/shared_ptr_array.hpp"

namespace devilution {
namespace {

constexpr char FixturesDir[] = "test/fixtures/d2sound/";

std::vector<uint8_t> ReadFixture(const std::string &name)
{
	std::ifstream file(FixturesDir + name, std::ios::binary);
	return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

TEST(D2SoundMapTest, ParsesEntries)
{
	const std::vector<D2SoundMapEntry> entries = ParseD2SoundMap(
	    "diablo\td2\td2hd\tnote\r\n"
	    "# a comment\r\n"
	    "sfx\\items\\gold.wav\tdata\\global\\sfx\\item\\gold\tdata\\hd\\global\\sfx\\item\\item_gold_hd\tgold\r\n"
	    "\r\n"
	    "sfx\\warrior\\wario14.wav\tdata\\local\\sfx\\common\\barbarian\\bar_cant\t\n"
	    "sfx\\misc\\nothing.wav\t\t\tunmapped\n");
	ASSERT_EQ(entries.size(), 2);
	EXPECT_EQ(entries[0].diablo, "sfx\\items\\gold.wav");
	EXPECT_EQ(entries[0].d2, "data\\global\\sfx\\item\\gold");
	EXPECT_EQ(entries[0].d2hd, "data\\hd\\global\\sfx\\item\\item_gold_hd");
	EXPECT_EQ(entries[1].diablo, "sfx\\warrior\\wario14.wav");
	EXPECT_EQ(entries[1].d2, "data\\local\\sfx\\common\\barbarian\\bar_cant");
	EXPECT_EQ(entries[1].d2hd, "");
}

TEST(D2SoundMapTest, HdSoundPath)
{
	EXPECT_EQ(D2HdSoundPath({ "a.wav", "data\\global\\sfx\\item\\gold", "data\\hd\\global\\sfx\\item\\item_gold_hd" }), "data\\hd\\global\\sfx\\item\\item_gold_hd");
	EXPECT_EQ(D2HdSoundPath({ "a.wav", "data\\global\\sfx\\item\\gold", "" }), "data\\hd\\global\\sfx\\item\\gold");
	EXPECT_EQ(D2HdSoundPath({ "a.wav", "data\\local\\sfx\\common\\barbarian\\bar_cant", "" }), "");
}

TEST(D2MpqArchiveTest, ReadsDiablo2Compressions)
{
	const std::unique_ptr<D2MpqArchive> archive = D2MpqArchive::Open(std::string(FixturesDir) + "test.mpq");
	ASSERT_NE(archive, nullptr);

	const std::vector<uint8_t> text = ReadFixture("text.txt");
	const std::vector<uint8_t> tone = ReadFixture("tone.wav");
	ASSERT_FALSE(text.empty());
	ASSERT_FALSE(tone.empty());
	const std::pair<const char *, std::vector<uint8_t>> expectedFiles[] = {
		{ "data\\global\\sfx\\Test\\wave_mono.wav", ReadFixture("wave_mono_expected.wav") },
		{ "data\\global\\sfx\\Test\\wave_stereo.wav", ReadFixture("wave_stereo_expected.wav") },
		{ "data\\local\\sfx\\implode.wav", tone },
		{ "data\\pkware.txt", text },
		{ "data\\zlib.txt", text },
		{ "data\\huff.txt", text },
		{ "data\\single.txt", text },
		{ "data\\stored.txt", text },
	};
	for (const auto &[name, expected] : expectedFiles) {
		ASSERT_FALSE(expected.empty()) << name;
		const std::optional<std::vector<uint8_t>> data = archive->ReadFile(name);
		ASSERT_TRUE(data.has_value()) << name;
		EXPECT_EQ(*data, expected) << name;
	}

	// Names are case-insensitive and both separators work.
	EXPECT_TRUE(archive->HasFile("DATA/GLOBAL/SFX/test/WAVE_MONO.WAV"));
	EXPECT_FALSE(archive->HasFile("data\\missing.wav"));
	EXPECT_FALSE(archive->ReadFile("data\\missing.wav").has_value());
}

TEST(D2MpqArchiveTest, RejectsNonMpqFiles)
{
	EXPECT_EQ(D2MpqArchive::Open(std::string(FixturesDir) + "text.txt"), nullptr);
	EXPECT_EQ(D2MpqArchive::Open(std::string(FixturesDir) + "missing.mpq"), nullptr);
}

/** The import job reads the archives of an install folder (ignoring the case of their names) and writes the pack through its sink. */
TEST(D2SoundImportJobTest, ImportsFromMpqFolder)
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "dvl_d2sound_import_test";
	std::filesystem::remove_all(root);
	const std::filesystem::path install = root / "Diablo II";
	const std::filesystem::path mods = root / "mods";
	std::filesystem::create_directories(install);
	std::filesystem::copy_file(std::string(FixturesDir) + "test.mpq", install / "D2SFX.MPQ");
	ASSERT_TRUE(IsDiablo2InstallDir(install.string()));
	EXPECT_FALSE(IsDiablo2ResurrectedInstallDir(install.string()));

	std::vector<D2SoundMapEntry> entries = ParseD2SoundMap(
	    "diablo\td2\td2hd\tnote\n"
	    "sfx\\test\\mono.wav\tdata\\global\\sfx\\Test\\wave_mono\t\t\n"
	    "sfx\\test\\missing.wav\tdata\\global\\sfx\\missing\t\t\n");
	ASSERT_EQ(entries.size(), 2);

	D2SoundDirectorySink sink(mods.string());
	// A stale manifest is removed when the import starts.
	std::filesystem::create_directories(mods / "d2sounds");
	std::ofstream(mods / "d2sounds" / "manifest.ini") << "stale";
	D2SoundImportJob job(D2SoundSource::Diablo2, install.string(), std::move(entries), sink);
	ASSERT_EQ(job.Step(std::chrono::seconds(1)), 1);
	EXPECT_FALSE(std::filesystem::exists(mods / "d2sounds" / "manifest.ini"));
	while (job.Step(std::chrono::seconds(1)) < 100) { }

	const D2SoundImportResult &result = job.Result();
	EXPECT_TRUE(result.ok());
	EXPECT_EQ(result.total, 2);
	EXPECT_EQ(result.imported, 1);
	ASSERT_EQ(result.missing.size(), 1);
	EXPECT_EQ(result.missing[0], "sfx\\test\\missing.wav");

	std::ifstream imported(mods / "d2sounds" / "sfx" / "test" / "mono.wav", std::ios::binary);
	const std::vector<uint8_t> data { std::istreambuf_iterator<char>(imported), std::istreambuf_iterator<char>() };
	EXPECT_EQ(data, ReadFixture("wave_mono_expected.wav"));
	EXPECT_TRUE(std::filesystem::exists(mods / "d2sounds" / "manifest.ini"));
	EXPECT_FALSE(std::filesystem::exists(mods / "d2sounds" / "sfx" / "test" / "missing.wav"));

	std::filesystem::remove_all(root);
}

TEST(D2SoundImportJobTest, FailsWithoutArchives)
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "dvl_d2sound_empty_test";
	std::filesystem::create_directories(root);
	D2SoundDirectorySink sink((root / "mods").string());
	D2SoundImportJob job(D2SoundSource::Diablo2, root.string(), ParseD2SoundMap("a.wav\tdata\\a\n"), sink);
	while (job.Step(std::chrono::seconds(1)) < 100) { }
	EXPECT_EQ(job.Result().error, D2SoundImportError::OpenArchivesFailed);
	EXPECT_FALSE(job.Result().ok());
	std::filesystem::remove_all(root);
}

TEST(AudioFileFormatTest, DetectsFormatFromData)
{
	const std::vector<uint8_t> flac = ReadFixture("tone.flac");
	const std::vector<uint8_t> wav = ReadFixture("tone.wav");
	EXPECT_EQ(DetectAudioFileFormat(flac, /*isMp3=*/false), AudioFileFormat::Flac);
	EXPECT_EQ(DetectAudioFileFormat(flac, /*isMp3=*/true), AudioFileFormat::Flac);
	EXPECT_EQ(DetectAudioFileFormat(wav, /*isMp3=*/true), AudioFileFormat::Wav);
	const uint8_t id3[] = { 'I', 'D', '3', 4 };
	EXPECT_EQ(DetectAudioFileFormat(id3, /*isMp3=*/true), AudioFileFormat::Mp3);
	EXPECT_EQ(DetectAudioFileFormat(id3, /*isMp3=*/false), AudioFileFormat::Wav);
	EXPECT_EQ(DetectAudioFileFormat({}, /*isMp3=*/false), AudioFileFormat::Wav);
}

#ifdef DEVILUTIONX_AULIB_HAS_DRFLAC
TEST(AudioFileFormatTest, DecodesFlac)
{
	std::vector<uint8_t> flac = ReadFixture("tone.flac");
	ASSERT_FALSE(flac.empty());
	SDL_IOStream *handle = SDL_IOFromConstMem(flac.data(), static_cast<int>(flac.size()));
	ASSERT_NE(handle, nullptr);
	Aulib::DecoderDrflac decoder;
	ASSERT_TRUE(decoder.open(handle));
	EXPECT_EQ(decoder.getRate(), 22050);
	EXPECT_EQ(decoder.getChannels(), 1);
	std::vector<float> samples(1024);
	size_t total = 0;
	for (;;) {
		bool callAgain = false;
		const int numRead = decoder.decode(samples.data(), static_cast<int>(samples.size()), callAgain);
		if (numRead <= 0 && !callAgain)
			break;
		if (numRead > 0)
			total += static_cast<size_t>(numRead);
	}
	// 0.25 s at 22050 Hz.
	EXPECT_EQ(total, 5512);
	SDL_CloseIO(handle);
}
#endif

/** The game plays FLAC data stored under a `.wav` name (as imported from Diablo II: Resurrected). */
TEST(SoundSampleTest, LoadsFlacFromDataNotFileName)
{
	// The dummy driver never outputs anything, and nothing is played.
	SDL_SetHint("SDL_AUDIO_DRIVER", "dummy");
	SDL_SetHint("SDL_AUDIODRIVER", "dummy");
#ifdef USE_SDL3
	const bool audioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
#else
	const bool audioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
#endif
	if (!audioInitialized)
		GTEST_SKIP() << "No audio: " << SDL_GetError();
	snd_init();
	if (!gbSndInited) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		GTEST_SKIP() << "No audio";
	}

	const std::vector<uint8_t> flac = ReadFixture("tone.flac");
	ASSERT_FALSE(flac.empty());
	{
		// In memory, as for most sound effects.
		SoundSample sample;
		ArraySharedPtr<std::uint8_t> data = MakeArraySharedPtr<std::uint8_t>(flac.size());
		std::copy(flac.begin(), flac.end(), data.get());
		ASSERT_EQ(sample.SetChunk(data, flac.size(), /*isMp3=*/false), 0);
		EXPECT_NEAR(sample.GetLength(), 250, 2);
	}
	{
		// Streamed, as for speech.
		SoundSample sample;
		const std::string path = std::filesystem::absolute(std::string(FixturesDir) + "tone.flac").string();
		ASSERT_EQ(sample.SetChunkStream(path, /*isMp3=*/false), 0);
		EXPECT_NEAR(sample.GetLength(), 250, 2);
	}

	snd_deinit();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

} // namespace
} // namespace devilution
