/**
 * @file d2sound/d2sound_import.hpp
 *
 * Imports the sounds of an installed Diablo II or Diablo II: Resurrected as sound pack mods.
 *
 * The mapping from the Diablo sounds to the Diablo II ones is defined by
 * `txtdata\sound\d2_sfx_map.tsv` and `txtdata\sound\d2_speech_map.tsv`. Each imported file is
 * written as-is (WAV for Diablo II, FLAC for Diablo II: Resurrected) under the name the game
 * requests into an unpacked mod folder, `<pref path>/mods/<mod name>/`. The sound decoder detects
 * the format from the data, so FLAC data stored under a `.wav` name plays fine.
 *
 * Three packs exist: `d2sounds` (Diablo II), `d2r-sounds-classic` (Diablo II: Resurrected,
 * its original sounds) and `d2r-sounds-advanced` (Diablo II: Resurrected, its remastered sounds).
 * Importing Diablo II: Resurrected creates both of its packs.
 *
 * In the browser, the game cannot read the user's install folder: the page imports the sounds
 * with a worker (see d2sound_web_worker.cpp and Packaging/emscripten/d2sound-ui.js) and writes
 * the packs into the same mod folders, so only the pack helpers are available there.
 */
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "d2sound/d2sound_core.hpp"
#include "options.h"

namespace devilution {

/** @brief The name of the mod that holds a sound pack. Empty for `SoundPack::Diablo`. */
std::string_view SoundPackModName(SoundPack pack);

/** @brief Which installed game a sound pack is imported from. */
D2SoundSource SoundPackSource(SoundPack pack);

/** @brief Whether the sound pack's mod folder exists (it is only completed by a successful import). */
bool IsSoundPackImported(SoundPack pack);

#ifdef __EMSCRIPTEN__
/** @brief Opens the page's import dialog for `source` (the user chooses the game's files there). */
void RequestBrowserD2SoundImport(D2SoundSource source);
#else
bool IsDiablo2Install(const std::string &path);
bool IsDiablo2ResurrectedInstall(const std::string &path);

/**
 * @brief Finds the install folder, from the diablo.ini setting or the default locations
 * (including Wine, CrossOver and Bottles prefixes).
 */
std::optional<std::string> FindD2SoundSourceInstall(D2SoundSource source);

/** @brief Imports incrementally into `<pref path>/mods/`, so that a progress dialog can be shown. */
class D2SoundImporter {
public:
	explicit D2SoundImporter(D2SoundSource source);
	~D2SoundImporter();
	D2SoundImporter(const D2SoundImporter &) = delete;
	D2SoundImporter &operator=(const D2SoundImporter &) = delete;

	/**
	 * @brief Imports sounds for about `budget`.
	 * @return The progress (0 to 100). 100 means done: check `Result()`.
	 */
	int Step(std::chrono::steady_clock::duration budget);

	/** @brief Stops the import. The pack stays unavailable until it is imported completely. */
	void Cancel();

	[[nodiscard]] const D2SoundImportResult &Result() const;

private:
	D2SoundImportResult result_;
	std::unique_ptr<D2SoundDirectorySink> sink_;
	std::unique_ptr<D2SoundImportJob> job_;
	bool done_ = false;
};

/** @brief Imports all sounds at once. */
D2SoundImportResult ImportD2Sounds(D2SoundSource source);

/** @brief A user-facing summary of an import. */
std::string DescribeD2SoundImportResult(const D2SoundImportResult &result);
#endif

} // namespace devilution
