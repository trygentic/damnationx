/**
 * @file d2sound/d2sound_web_worker.cpp
 *
 * The browser's Diablo II sound importer: a small WebAssembly module (d2sound-importer.js/.wasm)
 * that runs in a Web Worker, separately from the game.
 *
 * The game can't read the user's install folder in the browser, and a Diablo II: Resurrected
 * storage is tens of GB, so it can't be copied into the browser's file system either. Instead,
 * the page passes the File objects that the user chose (a folder or MPQ files) to this worker,
 * which mounts them with WORKERFS: reads use FileReaderSync on slices of the files, so only the
 * bytes that the importer needs are read and nothing is copied. Each imported file is posted
 * back to the page (see Packaging/emscripten/d2sound-worker-pre.js and d2sound-ui.js), which
 * writes it into the game's persistent mod folders.
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten.h>

#include "d2sound/d2sound_core.hpp"

namespace devilution {
namespace {

// clang-format off
EM_JS(void, PostBeginPack, (const char *mod), {
	postMessage({ type: 'begin-pack', mod: UTF8ToString(mod) });
});

EM_JS(void, PostFile, (const char *mod, const char *path, const uint8_t *data, size_t size), {
	const bytes = HEAPU8.slice(data, data + size);
	postMessage({ type: 'file', mod: UTF8ToString(mod), path: UTF8ToString(path), data: bytes }, [bytes.buffer]);
});

EM_JS(void, PostLog, (int level, const char *message), {
	postMessage({ type: 'log', level: ['verbose', 'info', 'warn', 'error'][level], message: UTF8ToString(message) });
});
// clang-format on

class WorkerSink final : public D2SoundSink {
public:
	void BeginPack(std::string_view modName) override
	{
		PostBeginPack(std::string(modName).c_str());
	}

	bool Write(std::string_view modName, std::string_view path, const std::vector<uint8_t> &data) override
	{
		PostFile(std::string(modName).c_str(), std::string(path).c_str(), data.data(), data.size());
		return true;
	}
};

void Log(D2SoundLogLevel level, std::string_view message)
{
	if (level == D2SoundLogLevel::Verbose)
		return;
	PostLog(static_cast<int>(level), std::string(message).c_str());
}

std::string JsonString(std::string_view str)
{
	std::string result = "\"";
	for (const char c : str) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char escaped[8];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				result += escaped;
			} else {
				result += c;
			}
		}
	}
	result += '"';
	return result;
}

std::string_view ErrorName(D2SoundImportError error)
{
	switch (error) {
	case D2SoundImportError::None:
		return "";
	case D2SoundImportError::InstallNotFound:
		return "install-not-found";
	case D2SoundImportError::SoundMapsMissing:
		return "sound-maps-missing";
	case D2SoundImportError::OpenArchivesFailed:
		return "open-archives-failed";
	case D2SoundImportError::OpenStorageFailed:
		return "open-storage-failed";
	case D2SoundImportError::WriteFailed:
		return "write-failed";
	case D2SoundImportError::NoSoundsFound:
		return "no-sounds-found";
	}
	return "unknown";
}

std::unique_ptr<WorkerSink> Sink;
std::unique_ptr<D2SoundImportJob> Job;
std::string ResultJson;

} // namespace
} // namespace devilution

using namespace devilution;

extern "C" {

/**
 * @brief Starts an import.
 * @param source 0: Diablo II (MPQ archives), 1: Diablo II: Resurrected (CASC storage).
 * @param installPath The folder with the game's files (a WORKERFS mount).
 * @param mapPaths The sound map files (TSV), separated by `\n`.
 * @return The number of mapped sounds.
 */
EMSCRIPTEN_KEEPALIVE int d2w_begin(int source, const char *installPath, const char *mapPaths)
{
	SetD2SoundLogger(&Log);
	std::vector<D2SoundMapEntry> entries;
	std::string_view paths = mapPaths;
	while (!paths.empty()) {
		const size_t end = paths.find('\n');
		const std::string path(paths.substr(0, end));
		paths.remove_prefix(end == std::string_view::npos ? paths.size() : end + 1);
		std::ifstream file(path, std::ios::binary);
		const std::string tsv { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		std::vector<D2SoundMapEntry> mapEntries = ParseD2SoundMap(tsv);
		entries.insert(entries.end(), std::make_move_iterator(mapEntries.begin()), std::make_move_iterator(mapEntries.end()));
	}
	const int count = static_cast<int>(entries.size());
	Sink = std::make_unique<WorkerSink>();
	Job = std::make_unique<D2SoundImportJob>(source == 1 ? D2SoundSource::Diablo2Resurrected : D2SoundSource::Diablo2, installPath, std::move(entries), *Sink);
	return count;
}

/** @brief Imports for about `budgetMs` milliseconds. Returns the progress (100: done). */
EMSCRIPTEN_KEEPALIVE int d2w_step(int budgetMs)
{
	if (Job == nullptr)
		return 100;
	return Job->Step(std::chrono::milliseconds(budgetMs));
}

/** @brief The result of the import as JSON. */
EMSCRIPTEN_KEEPALIVE const char *d2w_result_json()
{
	if (Job == nullptr)
		return "{}";
	const D2SoundImportResult &result = Job->Result();
	ResultJson = "{\"source\":";
	ResultJson += result.source == D2SoundSource::Diablo2 ? "\"d2\"" : "\"d2r\"";
	ResultJson += ",\"ok\":";
	ResultJson += result.ok() ? "true" : "false";
	ResultJson += ",\"error\":" + JsonString(ErrorName(result.error));
	ResultJson += ",\"errorPath\":" + JsonString(result.errorPath);
	ResultJson += ",\"total\":" + std::to_string(result.total);
	ResultJson += ",\"imported\":" + std::to_string(result.imported);
	ResultJson += ",\"importedAdvanced\":" + std::to_string(result.importedAdvanced);
	ResultJson += ",\"remastered\":" + std::to_string(result.remastered);
	ResultJson += ",\"seconds\":" + std::to_string(result.seconds);
	ResultJson += ",\"missing\":[";
	for (size_t i = 0; i < result.missing.size(); i++) {
		if (i != 0)
			ResultJson += ',';
		ResultJson += JsonString(result.missing[i]);
	}
	ResultJson += "]}";
	return ResultJson.c_str();
}

} // extern "C"
