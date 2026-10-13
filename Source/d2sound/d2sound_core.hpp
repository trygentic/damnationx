/**
 * @file d2sound/d2sound_core.hpp
 *
 * The engine-independent part of the Diablo II sound importer: the sound map, the readers of
 * the Diablo II archives (MPQ) and the Diablo II: Resurrected storage (CASC), and an incremental
 * import job that hands each imported file to a sink.
 *
 * It only depends on the C++ standard library, d2_mpq, StormLib's decompressors, CascLib and
 * zlib, so that it is also built into the browser's import worker (d2sound_web_worker.cpp).
 * The engine integration (install detection, options, mod folders) is in d2sound_import.hpp.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

enum class D2SoundSource : uint8_t {
	Diablo2,
	Diablo2Resurrected,
};

struct D2SoundMapEntry {
	/** @brief The asset path as the game requests it, e.g. `sfx\items\flipswor.wav`. */
	std::string diablo;
	/** @brief The Diablo II path without an extension, e.g. `data\global\sfx\item\gold`. */
	std::string d2;
	/** @brief The Diablo II: Resurrected remastered path without an extension (optional). */
	std::string d2hd;
};

/** @brief Parses a sound map (TSV with the header `diablo d2 d2hd note`; `#` starts a comment line). */
std::vector<D2SoundMapEntry> ParseD2SoundMap(std::string_view tsv);

/**
 * @brief The path of the remastered (advanced) version of a sound in Diablo II: Resurrected.
 * Uses `d2hd` if set, otherwise `data\global\...` becomes `data\hd\global\...`.
 * Returns an empty string if there is no remastered path.
 */
std::string D2HdSoundPath(const D2SoundMapEntry &entry);

/**
 * @brief The Diablo II archives that the importer reads, from the highest priority to the lowest
 * (the expansion and the patch override the base game). Only the ones that exist are opened.
 */
inline constexpr std::array<std::string_view, 7> Diablo2SoundArchives = {
	"patch_d2.mpq",
	"d2exp.mpq",
	"d2xtalk.mpq",
	"d2sfx.mpq",
	"d2speech.mpq",
	"d2char.mpq",
	"d2data.mpq",
};

/** @brief The mod folder names of the sound packs. */
inline constexpr std::string_view Diablo2SoundsModName = "d2sounds";
inline constexpr std::string_view Diablo2ResurrectedClassicSoundsModName = "d2r-sounds-classic";
inline constexpr std::string_view Diablo2ResurrectedAdvancedSoundsModName = "d2r-sounds-advanced";

/** @brief Whether `path` is a Diablo II folder (it has `d2sfx.mpq`). */
bool IsDiablo2InstallDir(const std::string &path);
/** @brief Whether `path` is a Diablo II: Resurrected folder (it has `.build.info` and `Data/indices`). */
bool IsDiablo2ResurrectedInstallDir(const std::string &path);

/** @brief Finds a file in a directory, ignoring case. Returns the full path or an empty string. */
std::string FindD2FileIgnoringCase(const std::string &dir, std::string_view lowercaseName);

enum class D2SoundImportError : uint8_t {
	None,
	InstallNotFound,
	SoundMapsMissing,
	OpenArchivesFailed,
	OpenStorageFailed,
	/** @brief `D2SoundImportResult::errorPath` is the file that could not be written. */
	WriteFailed,
	NoSoundsFound,
};

struct D2SoundImportResult {
	D2SoundSource source = D2SoundSource::Diablo2;
	std::string installPath;
	D2SoundImportError error = D2SoundImportError::None;
	std::string errorPath;
	bool cancelled = false;
	/** @brief Number of mapped sounds. */
	size_t total = 0;
	/** @brief Sounds imported into `d2sounds` or `d2r-sounds-classic`. */
	size_t imported = 0;
	/** @brief Sounds imported into `d2r-sounds-advanced` (Diablo II: Resurrected only). */
	size_t importedAdvanced = 0;
	/** @brief Of `importedAdvanced`, how many have a remastered file (the others use the classic one). */
	size_t remastered = 0;
	std::vector<std::string> missing;
	double seconds = 0;

	[[nodiscard]] bool ok() const { return error == D2SoundImportError::None && !cancelled; }
};

/** @brief Receives the files of the imported sound packs. */
class D2SoundSink {
public:
	virtual ~D2SoundSink() = default;

	/**
	 * @brief Called before the first file of a pack. The pack's `manifest.ini` marks a completed
	 * import, so a sink removes an existing one here: it is written again last.
	 */
	virtual void BeginPack(std::string_view modName) = 0;

	/**
	 * @brief Writes a file of a pack.
	 * @param path The path in the mod folder, with `\` separators (e.g. `sfx\items\gold.wav`).
	 * @return Whether the file was written. On failure the import continues without the file.
	 */
	virtual bool Write(std::string_view modName, std::string_view path, const std::vector<uint8_t> &data) = 0;

	/** @brief A description of where `path` of `modName` is written, for error messages. */
	[[nodiscard]] virtual std::string Describe(std::string_view modName, std::string_view path) const;
};

/** @brief Writes the packs into `<modsPath>/<mod name>/`. */
class D2SoundDirectorySink final : public D2SoundSink {
public:
	explicit D2SoundDirectorySink(std::string modsPath);

	void BeginPack(std::string_view modName) override;
	bool Write(std::string_view modName, std::string_view path, const std::vector<uint8_t> &data) override;
	[[nodiscard]] std::string Describe(std::string_view modName, std::string_view path) const override;

private:
	[[nodiscard]] std::string FilePath(std::string_view modName, std::string_view path) const;

	std::string modsPath_;
	std::vector<std::string> createdDirs_;
};

enum class D2SoundLogLevel : uint8_t {
	Verbose,
	Info,
	Warn,
	Error,
};

/** @brief Where the importer logs to (nothing by default). */
void SetD2SoundLogger(void (*logger)(D2SoundLogLevel level, std::string_view message));

/**
 * @brief Imports the sounds of an install folder incrementally, so that progress can be shown.
 *
 * Diablo II creates the `d2sounds` pack; Diablo II: Resurrected creates `d2r-sounds-classic`
 * and `d2r-sounds-advanced`. A pack's `manifest.ini` is written last, once its sounds are.
 */
class D2SoundImportJob {
public:
	/** @param sink Must outlive the job. */
	D2SoundImportJob(D2SoundSource source, std::string installPath, std::vector<D2SoundMapEntry> entries, D2SoundSink &sink);
	~D2SoundImportJob();
	D2SoundImportJob(const D2SoundImportJob &) = delete;
	D2SoundImportJob &operator=(const D2SoundImportJob &) = delete;

	/**
	 * @brief Opens the archives on the first call, then imports sounds for about `budget`.
	 * @return The progress (0 to 100). 100 means done: check `Result()`.
	 */
	int Step(std::chrono::steady_clock::duration budget);

	/** @brief Stops the import. The packs stay unavailable until they are imported completely. */
	void Cancel();

	[[nodiscard]] const D2SoundImportResult &Result() const;

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace devilution
