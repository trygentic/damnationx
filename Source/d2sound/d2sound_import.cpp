#include "d2sound/d2sound_import.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "engine/assets.hpp"
#include "options.h"
#include "utils/file_util.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

std::string ModPath(SoundPack pack)
{
	return StrCat(paths::PrefPath(), "mods" DIRECTORY_SEPARATOR_STR, SoundPackModName(pack), DIRECTORY_SEPARATOR_STR);
}

std::string ManifestPath(SoundPack pack)
{
	return StrCat(ModPath(pack), "manifest.ini");
}

#ifndef __EMSCRIPTEN__
constexpr std::string_view Diablo2Name = "Diablo II";
constexpr std::string_view Diablo2ResurrectedName = "Diablo II Resurrected";

constexpr std::string_view SoundMapPaths[] = {
	"txtdata\\sound\\d2_sfx_map.tsv",
	"txtdata\\sound\\d2_speech_map.tsv",
};

std::string JoinPath(std::string_view dir, std::string_view name)
{
	if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
		return StrCat(dir, name);
	return StrCat(dir, DIRECTORY_SEPARATOR_STR, name);
}

std::string TrimPath(std::string_view path)
{
	while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
		path.remove_suffix(1);
	return std::string(path);
}

void LogD2Sound(D2SoundLogLevel level, std::string_view message)
{
	switch (level) {
	case D2SoundLogLevel::Verbose:
		LogVerbose("{}", message);
		break;
	case D2SoundLogLevel::Info:
		LogInfo("{}", message);
		break;
	case D2SoundLogLevel::Warn:
		LogWarn("{}", message);
		break;
	case D2SoundLogLevel::Error:
		LogError("{}", message);
		break;
	}
}

#ifdef _WIN32
std::optional<std::string> ReadRegistryString(HKEY root, const char *subKey, const char *valueName)
{
	char buffer[MAX_PATH * 2];
	DWORD size = sizeof(buffer);
	if (RegGetValueA(root, subKey, valueName, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
		return std::nullopt;
	return std::string(buffer);
}
#endif

void AddWinePrefixes(std::vector<std::string> &prefixes, const std::string &bottlesDir)
{
	for (const std::string &bottle : ListDirectories(bottlesDir.c_str()))
		prefixes.push_back(JoinPath(JoinPath(bottlesDir, bottle), "drive_c"));
}

std::vector<std::string> CandidateInstallDirs(D2SoundSource source)
{
	const std::string_view name = source == D2SoundSource::Diablo2 ? Diablo2Name : Diablo2ResurrectedName;
	std::vector<std::string> candidates;

#ifdef _WIN32
	if (source == D2SoundSource::Diablo2) {
		if (std::optional<std::string> path = ReadRegistryString(HKEY_CURRENT_USER, "Software\\Blizzard Entertainment\\Diablo II", "InstallPath"))
			candidates.push_back(*std::move(path));
	} else {
		if (std::optional<std::string> path = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Diablo II Resurrected", "InstallLocation"))
			candidates.push_back(*std::move(path));
	}
	for (const char *root : { "C:\\Program Files (x86)\\", "C:\\Program Files\\", "C:\\Games\\" })
		candidates.push_back(StrCat(root, name));
#else
	std::vector<std::string> winePrefixes;
	const char *home = std::getenv("HOME");
	if (home != nullptr && *home != '\0') {
		const std::string homeDir = home;
		winePrefixes.push_back(JoinPath(homeDir, ".wine/drive_c"));
#ifdef __APPLE__
		if (source == D2SoundSource::Diablo2) {
			candidates.push_back(JoinPath(homeDir, "Applications/Diablo II"));
		}
		AddWinePrefixes(winePrefixes, JoinPath(homeDir, "Library/Application Support/CrossOver/Bottles"));
		AddWinePrefixes(winePrefixes, JoinPath(homeDir, "Library/Containers/com.isaacmarovitz.Whisky/Bottles"));
#else
		AddWinePrefixes(winePrefixes, JoinPath(homeDir, ".local/share/bottles/bottles"));
		AddWinePrefixes(winePrefixes, JoinPath(homeDir, ".var/app/com.usebottles.bottles/data/bottles/bottles"));
		// Lutris creates a prefix per game in ~/Games.
		AddWinePrefixes(winePrefixes, JoinPath(homeDir, "Games"));
#endif
	}
#ifdef __APPLE__
	if (source == D2SoundSource::Diablo2) {
		candidates.emplace_back("/Applications/Diablo II");
	}
	// Bottles on external drives, e.g. `/Volumes/<drive>/Bottles/<bottle>/drive_c`.
	for (const std::string &volume : ListDirectories("/Volumes"))
		AddWinePrefixes(winePrefixes, StrCat("/Volumes/", volume, "/Bottles"));
#endif
	for (const std::string &prefix : winePrefixes) {
		for (const char *dir : { "Program Files (x86)", "Program Files", "Games" })
			candidates.push_back(JoinPath(JoinPath(prefix, dir), name));
		candidates.push_back(JoinPath(prefix, name));
	}
#endif
	return candidates;
}

std::vector<D2SoundMapEntry> LoadSoundMaps()
{
	std::vector<D2SoundMapEntry> entries;
	for (const std::string_view mapPath : SoundMapPaths) {
		std::expected<AssetData, std::string> data = LoadAsset(mapPath);
		if (!data.has_value()) {
			LogWarn("Failed to load the sound map {}: {}", mapPath, data.error());
			continue;
		}
		std::vector<D2SoundMapEntry> mapEntries = ParseD2SoundMap(static_cast<std::string_view>(*data));
		entries.insert(entries.end(), std::make_move_iterator(mapEntries.begin()), std::make_move_iterator(mapEntries.end()));
	}
	return entries;
}
#endif

} // namespace

std::string_view SoundPackModName(SoundPack pack)
{
	switch (pack) {
	case SoundPack::Diablo2:
		return Diablo2SoundsModName;
	case SoundPack::Diablo2ResurrectedClassic:
		return Diablo2ResurrectedClassicSoundsModName;
	case SoundPack::Diablo2ResurrectedAdvanced:
		return Diablo2ResurrectedAdvancedSoundsModName;
	default:
		return {};
	}
}

D2SoundSource SoundPackSource(SoundPack pack)
{
	return pack == SoundPack::Diablo2 ? D2SoundSource::Diablo2 : D2SoundSource::Diablo2Resurrected;
}

bool IsSoundPackImported(SoundPack pack)
{
	if (pack == SoundPack::Diablo)
		return true;
	return FileExists(ManifestPath(pack));
}

#ifdef __EMSCRIPTEN__
void RequestBrowserD2SoundImport(D2SoundSource source)
{
	// Defined by Packaging/emscripten/d2sound-ui.js.
	EM_ASM({
		if (typeof window !== 'undefined' && window.DvlD2Sounds)
			window.DvlD2Sounds.open($0 ? 'd2r' : 'd2');
	},
	    source == D2SoundSource::Diablo2Resurrected ? 1 : 0);
}
#else
bool IsDiablo2Install(const std::string &path)
{
	return IsDiablo2InstallDir(path);
}

bool IsDiablo2ResurrectedInstall(const std::string &path)
{
	return IsDiablo2ResurrectedInstallDir(path);
}

std::optional<std::string> FindD2SoundSourceInstall(D2SoundSource source)
{
	const bool isD2 = source == D2SoundSource::Diablo2;
	const auto isInstall = isD2 ? &IsDiablo2Install : &IsDiablo2ResurrectedInstall;
	const std::string &configured = isD2 ? GetOptions().Audio.diablo2Path : GetOptions().Audio.diablo2ResurrectedPath;
	if (!configured.empty()) {
		const std::string path = TrimPath(configured);
		if (isInstall(path))
			return path;
		LogWarn("The configured {} path is not a {} install: {}", isD2 ? Diablo2Name : Diablo2ResurrectedName, isD2 ? Diablo2Name : Diablo2ResurrectedName, path);
	}
	for (const std::string &candidate : CandidateInstallDirs(source)) {
		if (isInstall(candidate)) {
			LogVerbose("Found {} at {}", isD2 ? Diablo2Name : Diablo2ResurrectedName, candidate);
			return candidate;
		}
	}
	return std::nullopt;
}

D2SoundImporter::D2SoundImporter(D2SoundSource source)
{
	result_.source = source;
}

D2SoundImporter::~D2SoundImporter() = default;

int D2SoundImporter::Step(std::chrono::steady_clock::duration budget)
{
	if (done_)
		return 100;
	if (job_ == nullptr) {
		SetD2SoundLogger(&LogD2Sound);
		const std::optional<std::string> installPath = FindD2SoundSourceInstall(result_.source);
		if (!installPath) {
			result_.error = D2SoundImportError::InstallNotFound;
			done_ = true;
			return 100;
		}
		result_.installPath = *installPath;
		sink_ = std::make_unique<D2SoundDirectorySink>(StrCat(paths::PrefPath(), "mods"));
		job_ = std::make_unique<D2SoundImportJob>(result_.source, *installPath, LoadSoundMaps(), *sink_);
		// Finding the install can take a while, so we return to update the progress dialog.
		return 1;
	}
	const int progress = job_->Step(budget);
	if (progress >= 100) {
		result_ = job_->Result();
		done_ = true;
	}
	return progress;
}

void D2SoundImporter::Cancel()
{
	if (done_)
		return;
	if (job_ != nullptr) {
		job_->Cancel();
		result_ = job_->Result();
	}
	result_.cancelled = true;
	done_ = true;
}

const D2SoundImportResult &D2SoundImporter::Result() const
{
	return result_;
}

D2SoundImportResult ImportD2Sounds(D2SoundSource source)
{
	D2SoundImporter importer(source);
	while (importer.Step(std::chrono::seconds(1)) < 100) { }
	return importer.Result();
}

std::string DescribeD2SoundImportResult(const D2SoundImportResult &result)
{
	if (result.cancelled)
		return std::string(_("The import was cancelled."));
	switch (result.error) {
	case D2SoundImportError::None:
		break;
	case D2SoundImportError::InstallNotFound:
		if (result.source == D2SoundSource::Diablo2)
			return std::string(_("Diablo II was not found.\nSet [Audio] Diablo II Path in diablo.ini to its folder."));
		return std::string(_("Diablo II Resurrected was not found.\nSet [Audio] Diablo II Resurrected Path in diablo.ini to its folder."));
	case D2SoundImportError::SoundMapsMissing:
		return std::string(_("The sound maps (txtdata\\sound\\d2_*_map.tsv) are missing."));
	case D2SoundImportError::OpenArchivesFailed:
		return FormatRuntime(_("Failed to open the Diablo II archives in {:s}."), result.installPath);
	case D2SoundImportError::OpenStorageFailed:
		return FormatRuntime(_("Failed to open the Diablo II Resurrected data in {:s}."), result.installPath);
	case D2SoundImportError::WriteFailed:
		return FormatRuntime(_("Failed to write {:s}."), result.errorPath);
	case D2SoundImportError::NoSoundsFound:
		return std::string(_("No sounds were found."));
	}
	std::string description;
	if (result.source == D2SoundSource::Diablo2) {
		description = FormatRuntime(_("Imported {:d} of {:d} sounds from Diablo II."), result.imported, result.total);
	} else {
		description = FormatRuntime(_("Imported {:d} of {:d} sounds from Diablo II Resurrected ({:d} remastered)."), (std::max)(result.imported, result.importedAdvanced), result.total, result.remastered);
	}
	if (!result.missing.empty())
		StrAppend(description, "\n", FormatRuntime(_("{:d} sounds were not found and keep their original sound."), result.missing.size()));
	return description;
}
#endif

} // namespace devilution
