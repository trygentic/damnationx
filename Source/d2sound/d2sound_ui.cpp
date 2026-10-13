#include "d2sound/d2sound_ui.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "DiabloUI/diabloui.h"
#include "DiabloUI/selok.h"
#include "DiabloUI/settingsmenu.h"
#include "lua/lua_global.hpp"
#include "options.h"
#include "utils/console.h"
#include "utils/language.h"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

constexpr SoundPack ImportedSoundPacks[] = {
	SoundPack::Diablo2,
	SoundPack::Diablo2ResurrectedClassic,
	SoundPack::Diablo2ResurrectedAdvanced,
};

#ifndef __EMSCRIPTEN__
std::unique_ptr<D2SoundImporter> CurrentImporter;
#endif

/** @brief Set while the sound pack option is reverted, so that the revert is not applied as a change. */
bool IgnoreSoundPackChange = false;

std::string_view SourceTitle(D2SoundSource source)
{
	return source == D2SoundSource::Diablo2 ? _("Diablo II Sounds") : _("Diablo II Resurrected Sounds");
}

void ShowMessage(D2SoundSource source, const std::string &body)
{
	const std::string title(SourceTitle(source));
	UiSelOkDialog(title.c_str(), body.c_str(), /*background=*/false);
}

#ifdef __EMSCRIPTEN__
/**
 * @brief The browser can't read the install folder: the page's dialog imports the sounds (see
 * Packaging/emscripten/d2sound-ui.js). It writes the packs into the mod folders, then the pack
 * can be selected.
 */
void RequestImport(D2SoundSource source)
{
	RequestBrowserD2SoundImport(source);
	ShowMessage(source, source == D2SoundSource::Diablo2
	        ? std::string(_("Choose your Diablo II folder or its MPQ files in the \"Diablo II Sounds\" dialog of the page.\nOnce imported, the sounds are used from the main menu on."))
	        : std::string(_("Choose your Diablo II Resurrected folder in the \"Diablo II Sounds\" dialog of the page.\nOnce imported, the sounds are used from the main menu on.")));
}
#else
int ImportProgress()
{
	return CurrentImporter->Step(std::chrono::milliseconds(30));
}

D2SoundImportResult ImportWithProgressDialog(D2SoundSource source)
{
	CurrentImporter = std::make_unique<D2SoundImporter>(source);
	if (!UiProgressDialog(ImportProgress))
		CurrentImporter->Cancel();
	D2SoundImportResult result = CurrentImporter->Result();
	CurrentImporter = nullptr;
	return result;
}

void ShowImportResult(const D2SoundImportResult &result)
{
	ShowMessage(result.source, DescribeD2SoundImportResult(result));
}
#endif

OptionEntryBoolean *FindModEntry(std::string_view modName)
{
	for (OptionEntryBase *entry : GetOptions().Mods.GetEntries()) {
		if (entry->key == modName)
			return static_cast<OptionEntryBoolean *>(entry);
	}
	return nullptr;
}

/** @brief Adds the mods of the imported packs to the mod list (it is otherwise only updated on startup). */
void AddImportedPacksToModList()
{
	for (const SoundPack pack : ImportedSoundPacks) {
		const std::string modName(SoundPackModName(pack));
		if (IsSoundPackImported(pack) && FindModEntry(modName) == nullptr)
			GetOptions().Mods.AddModEntry(modName);
	}
}

/** @brief The pack whose mod is enabled. */
SoundPack ActiveSoundPack()
{
	for (const SoundPack pack : ImportedSoundPacks) {
		const OptionEntryBoolean *entry = FindModEntry(SoundPackModName(pack));
		if (entry != nullptr && **entry)
			return pack;
	}
	return SoundPack::Diablo;
}

/** @brief Enables the mod of `pack` and disables the other packs. Returns whether a mod changed. */
bool EnableSoundPackMod(SoundPack pack)
{
	AddImportedPacksToModList();
	bool changed = false;
	for (const SoundPack candidate : ImportedSoundPacks) {
		const std::string modName(SoundPackModName(candidate));
		OptionEntryBoolean *entry = FindModEntry(modName);
		if (entry == nullptr || **entry == (candidate == pack))
			continue;
		entry->SetValue(candidate == pack);
		changed = true;
	}
	return changed;
}

/**
 * @brief Enables the mod of `pack` (and disables the other packs), then reloads the mods and the sounds.
 * @param reload Reload even if no mod changed (the files of the active pack were imported again).
 */
void ActivateSoundPack(SoundPack pack, bool reload = false)
{
	if (EnableSoundPackMod(pack) || reload) {
		LuaReloadActiveMods();
		LuaApplyPendingModReload();
	}
}

void SetSoundPackOption(SoundPack pack)
{
	IgnoreSoundPackChange = true;
	GetOptions().Audio.soundPack.SetValue(pack);
	IgnoreSoundPackChange = false;
}

/** @brief The pack that an import from `source` selects: the selected one if it comes from `source`. */
SoundPack SoundPackToSelectAfterImport(D2SoundSource source)
{
	const SoundPack selected = *GetOptions().Audio.soundPack;
	if (selected != SoundPack::Diablo && SoundPackSource(selected) == source)
		return selected;
	return source == D2SoundSource::Diablo2 ? SoundPack::Diablo2 : SoundPack::Diablo2ResurrectedAdvanced;
}

#ifdef __EMSCRIPTEN__
/** @brief The pack that is selected while the page imports it. It is activated once it is imported. */
std::optional<SoundPack> PendingSoundPack;
#endif

void ApplySelectedSoundPack()
{
	const SoundPack pack = *GetOptions().Audio.soundPack;
	if (!IsSoundPackImported(pack)) {
#ifdef __EMSCRIPTEN__
		// The page imports asynchronously. The selection is kept, and the pack is activated once
		// it is imported (see ApplyPendingSoundPack).
		PendingSoundPack = pack;
		RequestImport(SoundPackSource(pack));
		return;
#else
		const D2SoundImportResult result = ImportWithProgressDialog(SoundPackSource(pack));
		ShowImportResult(result);
		if (!IsSoundPackImported(pack)) {
			SetSoundPackOption(ActiveSoundPack());
			return;
		}
#endif
	}
#ifdef __EMSCRIPTEN__
	PendingSoundPack = std::nullopt;
#endif
	ActivateSoundPack(pack);
}

void ImportAndReload(D2SoundSource source)
{
	const SoundPack pack = SoundPackToSelectAfterImport(source);
#ifdef __EMSCRIPTEN__
	// The page imports asynchronously: the pack is selected now and activated once it is imported.
	SetSoundPackOption(pack);
	PendingSoundPack = pack;
	RequestImport(source);
#else
	const D2SoundImportResult result = ImportWithProgressDialog(source);
	ShowImportResult(result);
	if (!result.ok() || !IsSoundPackImported(pack)) {
		AddImportedPacksToModList();
		return;
	}
	// Use the imported sounds right away (an import without selecting the pack would keep the
	// original sounds), and reload them if the pack was already active.
	SetSoundPackOption(pack);
	ActivateSoundPack(pack, /*reload=*/true);
#endif
}

void OnSoundPackChanged()
{
	if (IgnoreSoundPackChange)
		return;
	DeferSettingsMenuAction(ApplySelectedSoundPack);
}

void OnImportDiablo2Sounds()
{
	DeferSettingsMenuAction([]() { ImportAndReload(D2SoundSource::Diablo2); });
}

void OnImportDiablo2ResurrectedSounds()
{
	DeferSettingsMenuAction([]() { ImportAndReload(D2SoundSource::Diablo2Resurrected); });
}

const auto OptionChangeHandlerSoundPack = (GetOptions().Audio.soundPack.SetValueChangedCallback(OnSoundPackChanged), true);
const auto OptionChangeHandlerImportDiablo2 = (GetOptions().Audio.importDiablo2Sounds.SetValueChangedCallback(OnImportDiablo2Sounds), true);
const auto OptionChangeHandlerImportDiablo2Resurrected = (GetOptions().Audio.importDiablo2ResurrectedSounds.SetValueChangedCallback(OnImportDiablo2ResurrectedSounds), true);

} // namespace

void ApplySoundPackOptionOnStartup()
{
	const SoundPack selected = *GetOptions().Audio.soundPack;
	if (!IsSoundPackImported(selected)) {
		// For example a browser import that was not completed.
		SetSoundPackOption(ActiveSoundPack());
		return;
	}
#ifdef __EMSCRIPTEN__
	// The pack was selected while the page imported it: the mods are loaded after this.
	if (selected != SoundPack::Diablo)
		EnableSoundPackMod(selected);
#endif
}

void ApplyPendingSoundPack()
{
#ifdef __EMSCRIPTEN__
	if (!PendingSoundPack)
		return;
	const SoundPack pack = *PendingSoundPack;
	if (*GetOptions().Audio.soundPack != pack) {
		// Another pack was selected since.
		PendingSoundPack = std::nullopt;
		return;
	}
	if (!IsSoundPackImported(pack))
		return;
	PendingSoundPack = std::nullopt;
	ActivateSoundPack(pack, /*reload=*/true);
	SaveOptions();
#endif
}

#ifndef __EMSCRIPTEN__
std::optional<D2SoundSource> ParseD2SoundSource(std::string_view name)
{
	if (name == "d2")
		return D2SoundSource::Diablo2;
	if (name == "d2r")
		return D2SoundSource::Diablo2Resurrected;
	return std::nullopt;
}

int ImportD2SoundsFromCommandLine(D2SoundSource source)
{
	const D2SoundImportResult result = ImportD2Sounds(source);
	if (!result.installPath.empty())
		printInConsole(StrCat("Source: ", result.installPath, "\n"));
	printInConsole(DescribeD2SoundImportResult(result));
	printInConsole("\n");
	if (!result.ok())
		return 1;
	printInConsole(StrCat("Time: ", static_cast<int>(result.seconds * 1000), " ms\n"));
	for (const std::string &missing : result.missing)
		printInConsole(StrCat("Not found: ", missing, "\n"));
	return 0;
}
#endif

} // namespace devilution
