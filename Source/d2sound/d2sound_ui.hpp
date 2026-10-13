/**
 * @file d2sound/d2sound_ui.hpp
 *
 * Settings menu and command line integration of the Diablo II sound importer.
 */
#pragma once

#include <optional>
#include <string_view>

#include "d2sound/d2sound_import.hpp"

namespace devilution {

/**
 * @brief Makes the enabled sound pack mod match the Sound Pack option. Called once the options
 * are loaded, before the mods are.
 */
void ApplySoundPackOptionOnStartup();

/**
 * @brief In the browser, activates a sound pack that was selected while the page imported it,
 * once the import is complete. Called from the main menu.
 */
void ApplyPendingSoundPack();

#ifndef __EMSCRIPTEN__

/** @brief Parses the argument of `--import-d2-sounds`: `d2` or `d2r`. */
std::optional<D2SoundSource> ParseD2SoundSource(std::string_view name);

/**
 * @brief Imports the sounds, prints the result to the console and returns the process exit code.
 * Needs the options and the core archives (for the sound maps) to be loaded.
 */
int ImportD2SoundsFromCommandLine(D2SoundSource source);

#endif

} // namespace devilution
