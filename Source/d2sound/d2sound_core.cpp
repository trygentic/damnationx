#include "d2sound/d2sound_core.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include <CascLib.h>

#include "d2sound/d2_mpq.hpp"

namespace devilution {

namespace {

void (*Logger)(D2SoundLogLevel level, std::string_view message) = nullptr;

template <typename... Args>
void Log(D2SoundLogLevel level, const Args &...args)
{
	if (Logger == nullptr)
		return;
	std::string message;
	((message += args), ...);
	Logger(level, message);
}

std::filesystem::path ToPath(std::string_view utf8)
{
	return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string FromPath(const std::filesystem::path &path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

std::string ToLower(std::string_view str)
{
	std::string result(str);
	for (char &c : result) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return result;
}

std::string_view Trim(std::string_view str)
{
	while (!str.empty() && (str.front() == ' ' || str.front() == '\t'))
		str.remove_prefix(1);
	while (!str.empty() && (str.back() == ' ' || str.back() == '\t' || str.back() == '\r'))
		str.remove_suffix(1);
	return str;
}

/** @brief Calls `fn` for each `separator`-separated part of `str` (including empty parts). */
template <typename Fn>
void ForEachPart(std::string_view str, char separator, Fn &&fn)
{
	while (true) {
		const size_t pos = str.find(separator);
		fn(str.substr(0, pos));
		if (pos == std::string_view::npos)
			return;
		str.remove_prefix(pos + 1);
	}
}

bool IsFile(const std::filesystem::path &path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

bool IsDirectory(const std::filesystem::path &path)
{
	std::error_code error;
	return std::filesystem::is_directory(path, error);
}

/** @brief A source of Diablo II sound files. */
class D2SoundReader {
public:
	virtual ~D2SoundReader() = default;
	/** @param d2Path A path from the sound map (no extension). */
	virtual std::optional<std::vector<uint8_t>> Read(std::string_view d2Path) = 0;
};

class MpqSoundReader final : public D2SoundReader {
public:
	std::vector<std::unique_ptr<D2MpqArchive>> archives;

	std::optional<std::vector<uint8_t>> Read(std::string_view d2Path) override
	{
		std::string name(d2Path);
		name += ".wav";
		for (const std::unique_ptr<D2MpqArchive> &archive : archives) {
			if (!archive->HasFile(name))
				continue;
			std::optional<std::vector<uint8_t>> data = archive->ReadFile(name);
			if (!data)
				Log(D2SoundLogLevel::Warn, "Failed to decompress ", name);
			return data;
		}
		return std::nullopt;
	}
};

class CascSoundReader final : public D2SoundReader {
public:
	HANDLE storage = nullptr;

	~CascSoundReader() override
	{
		if (storage != nullptr)
			CascCloseStorage(storage);
	}

	std::optional<std::vector<uint8_t>> Read(std::string_view d2Path) override
	{
		std::string name = "data:";
		name += d2Path;
		name += ".flac";
		name = ToLower(name);
		HANDLE file;
		if (!CascOpenFile(storage, name.c_str(), 0, CASC_OPEN_BY_NAME, &file))
			return std::nullopt;
		ULONGLONG size = 0;
		std::optional<std::vector<uint8_t>> result;
		if (CascGetFileSize64(file, &size) && size < (256U << 20)) {
			std::vector<uint8_t> data(static_cast<size_t>(size));
			DWORD numRead = 0;
			if (data.empty() || (CascReadFile(file, data.data(), static_cast<DWORD>(data.size()), &numRead) && numRead == data.size()))
				result = std::move(data);
		}
		CascCloseFile(file);
		if (!result)
			Log(D2SoundLogLevel::Warn, "Failed to read ", name);
		return result;
	}
};

} // namespace

void SetD2SoundLogger(void (*logger)(D2SoundLogLevel level, std::string_view message))
{
	Logger = logger;
}

std::vector<D2SoundMapEntry> ParseD2SoundMap(std::string_view tsv)
{
	std::vector<D2SoundMapEntry> entries;
	bool isFirstLine = true;
	ForEachPart(tsv, '\n', [&](std::string_view line) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (isFirstLine) {
			isFirstLine = false;
			// Skip the header and a UTF-8 BOM.
			if (line.starts_with("\xEF\xBB\xBF"))
				line.remove_prefix(3);
			if (line.starts_with("diablo"))
				return;
		}
		if (Trim(line).empty() || Trim(line).starts_with("#"))
			return;
		D2SoundMapEntry entry;
		size_t column = 0;
		ForEachPart(line, '\t', [&](std::string_view field) {
			switch (column++) {
			case 0:
				entry.diablo = Trim(field);
				break;
			case 1:
				entry.d2 = Trim(field);
				break;
			case 2:
				entry.d2hd = Trim(field);
				break;
			default:
				break;
			}
		});
		if (entry.diablo.empty() || (entry.d2.empty() && entry.d2hd.empty()))
			return;
		entries.push_back(std::move(entry));
	});
	return entries;
}

std::string D2HdSoundPath(const D2SoundMapEntry &entry)
{
	if (!entry.d2hd.empty())
		return entry.d2hd;
	constexpr std::string_view GlobalPrefix = "data\\global\\";
	if (entry.d2.size() > GlobalPrefix.size() && ToLower(std::string_view(entry.d2).substr(0, GlobalPrefix.size())) == GlobalPrefix)
		return std::string("data\\hd\\global\\").append(std::string_view(entry.d2).substr(GlobalPrefix.size()));
	return {};
}

std::string FindD2FileIgnoringCase(const std::string &dir, std::string_view lowercaseName)
{
	const std::filesystem::path dirPath = ToPath(dir);
	const std::filesystem::path exactPath = dirPath / ToPath(lowercaseName);
	if (IsFile(exactPath))
		return FromPath(exactPath);
	std::error_code error;
	for (std::filesystem::directory_iterator it(dirPath, error), end; !error && it != end; it.increment(error)) {
		if (ToLower(FromPath(it->path().filename())) == lowercaseName && IsFile(it->path()))
			return FromPath(it->path());
	}
	return {};
}

bool IsDiablo2InstallDir(const std::string &path)
{
	return !path.empty() && IsDirectory(ToPath(path)) && !FindD2FileIgnoringCase(path, "d2sfx.mpq").empty();
}

bool IsDiablo2ResurrectedInstallDir(const std::string &path)
{
	if (path.empty())
		return false;
	const std::filesystem::path root = ToPath(path);
	return IsDirectory(root / "Data" / "indices") && IsFile(root / ".build.info");
}

std::string D2SoundSink::Describe(std::string_view modName, std::string_view path) const
{
	return std::string(modName).append("/").append(path);
}

D2SoundDirectorySink::D2SoundDirectorySink(std::string modsPath)
    : modsPath_(std::move(modsPath))
{
}

std::string D2SoundDirectorySink::FilePath(std::string_view modName, std::string_view path) const
{
	std::filesystem::path result = ToPath(modsPath_) / ToPath(modName);
	ForEachPart(path, '\\', [&](std::string_view part) {
		if (!part.empty())
			result /= ToPath(part);
	});
	return FromPath(result);
}

std::string D2SoundDirectorySink::Describe(std::string_view modName, std::string_view path) const
{
	return FilePath(modName, path);
}

void D2SoundDirectorySink::BeginPack(std::string_view modName)
{
	std::error_code error;
	std::filesystem::remove(ToPath(FilePath(modName, "manifest.ini")), error);
}

bool D2SoundDirectorySink::Write(std::string_view modName, std::string_view path, const std::vector<uint8_t> &data)
{
	const std::filesystem::path filePath = ToPath(FilePath(modName, path));
	const std::string dir = FromPath(filePath.parent_path());
	if (std::find(createdDirs_.begin(), createdDirs_.end(), dir) == createdDirs_.end()) {
		std::error_code error;
		std::filesystem::create_directories(filePath.parent_path(), error);
		createdDirs_.push_back(dir);
	}
#ifdef _WIN32
	FILE *file = _wfopen(filePath.c_str(), L"wb");
#else
	FILE *file = std::fopen(filePath.c_str(), "wb");
#endif
	if (file == nullptr) {
		Log(D2SoundLogLevel::Error, "Failed to write ", FromPath(filePath));
		return false;
	}
	const bool ok = data.empty() || std::fwrite(data.data(), data.size(), 1, file) == 1;
	if (std::fclose(file) != 0 || !ok) {
		Log(D2SoundLogLevel::Error, "Failed to write ", FromPath(filePath));
		return false;
	}
	return true;
}

struct D2SoundImportJob::Impl {
	enum class State : uint8_t {
		NotStarted,
		Importing,
		Done,
	};

	explicit Impl(D2SoundSink &sink)
	    : sink(sink)
	{
	}

	D2SoundSink &sink;
	D2SoundImportResult result;
	State state = State::NotStarted;
	std::vector<D2SoundMapEntry> entries;
	size_t nextEntry = 0;
	std::unique_ptr<D2SoundReader> reader;
	std::vector<std::string_view> packs;
	std::chrono::steady_clock::time_point startTime;

	bool Begin();
	void ImportEntry(const D2SoundMapEntry &entry);
	bool WriteSound(std::string_view pack, std::string_view diabloPath, const std::vector<uint8_t> &data);
	void Finish();

	void Fail(D2SoundImportError error)
	{
		result.error = error;
		state = State::Done;
		reader = nullptr;
	}
};

bool D2SoundImportJob::Impl::Begin()
{
	startTime = std::chrono::steady_clock::now();
	if (entries.empty()) {
		Fail(D2SoundImportError::SoundMapsMissing);
		return false;
	}
	result.total = entries.size();

	if (result.source == D2SoundSource::Diablo2) {
		auto mpqReader = std::make_unique<MpqSoundReader>();
		for (const std::string_view archiveName : Diablo2SoundArchives) {
			const std::string archivePath = FindD2FileIgnoringCase(result.installPath, archiveName);
			if (archivePath.empty())
				continue;
			std::unique_ptr<D2MpqArchive> archive = D2MpqArchive::Open(archivePath);
			if (archive == nullptr) {
				Log(D2SoundLogLevel::Warn, "Failed to open ", archivePath);
				continue;
			}
			Log(D2SoundLogLevel::Verbose, "Opened ", archivePath);
			mpqReader->archives.push_back(std::move(archive));
		}
		if (mpqReader->archives.empty()) {
			Fail(D2SoundImportError::OpenArchivesFailed);
			return false;
		}
		reader = std::move(mpqReader);
		packs = { Diablo2SoundsModName };
	} else {
		auto cascReader = std::make_unique<CascSoundReader>();
		if (!CascOpenStorage(result.installPath.c_str(), 0, &cascReader->storage)) {
			cascReader->storage = nullptr;
			Log(D2SoundLogLevel::Error, "CascOpenStorage failed for ", result.installPath, ": error ", std::to_string(GetCascError()));
			Fail(D2SoundImportError::OpenStorageFailed);
			return false;
		}
		reader = std::move(cascReader);
		packs = { Diablo2ResurrectedClassicSoundsModName, Diablo2ResurrectedAdvancedSoundsModName };
	}

	for (const std::string_view pack : packs)
		sink.BeginPack(pack);

	state = State::Importing;
	return true;
}

bool D2SoundImportJob::Impl::WriteSound(std::string_view pack, std::string_view diabloPath, const std::vector<uint8_t> &data)
{
	return sink.Write(pack, diabloPath, data);
}

void D2SoundImportJob::Impl::ImportEntry(const D2SoundMapEntry &entry)
{
	std::optional<std::vector<uint8_t>> classic;
	if (!entry.d2.empty())
		classic = reader->Read(entry.d2);
	if (classic && WriteSound(packs[0], entry.diablo, *classic))
		result.imported++;

	if (packs.size() > 1) {
		std::optional<std::vector<uint8_t>> remastered;
		const std::string hdPath = D2HdSoundPath(entry);
		if (!hdPath.empty())
			remastered = reader->Read(hdPath);
		const std::optional<std::vector<uint8_t>> &advanced = remastered ? remastered : classic;
		if (advanced && WriteSound(packs[1], entry.diablo, *advanced)) {
			result.importedAdvanced++;
			if (remastered)
				result.remastered++;
		}
		if (!classic && !remastered) {
			Log(D2SoundLogLevel::Info, "Diablo II sound not found for ", entry.diablo, ": ", entry.d2, " / ", hdPath);
			result.missing.push_back(entry.diablo);
		}
		return;
	}

	if (!classic) {
		Log(D2SoundLogLevel::Info, "Diablo II sound not found for ", entry.diablo, ": ", entry.d2);
		result.missing.push_back(entry.diablo);
	}
}

void D2SoundImportJob::Impl::Finish()
{
	state = State::Done;
	reader = nullptr;
	for (size_t i = 0; i < packs.size(); i++) {
		const std::string_view pack = packs[i];
		const size_t count = i == 0 ? result.imported : result.importedAdvanced;
		if (count == 0)
			continue;
		std::string_view name;
		std::string_view description;
		if (pack == Diablo2SoundsModName) {
			name = "Diablo II Sounds";
			description = "Sounds imported from Diablo II.";
		} else if (pack == Diablo2ResurrectedClassicSoundsModName) {
			name = "Diablo II Resurrected Sounds (Classic)";
			description = "Sounds imported from Diablo II: Resurrected (original quality).";
		} else {
			name = "Diablo II Resurrected Sounds (Advanced)";
			description = "Sounds imported from Diablo II: Resurrected (remastered).";
		}
		std::string manifest = "[mod]\nname=";
		manifest.append(name).append("\ndescription=").append(description).append("\nauthor=Imported from ").append(result.installPath).append("\n");
		if (!sink.Write(pack, "manifest.ini", std::vector<uint8_t>(manifest.begin(), manifest.end()))) {
			result.error = D2SoundImportError::WriteFailed;
			result.errorPath = sink.Describe(pack, "manifest.ini");
		}
	}
	if (result.imported == 0 && result.importedAdvanced == 0 && result.error == D2SoundImportError::None)
		result.error = D2SoundImportError::NoSoundsFound;
	result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
	char seconds[32];
	std::snprintf(seconds, sizeof(seconds), "%.2f", result.seconds);
	Log(D2SoundLogLevel::Info, "Imported ", std::to_string(result.imported), " sounds (", std::to_string(result.importedAdvanced), " advanced, ",
	    std::to_string(result.remastered), " remastered) from ", result.installPath, " in ", seconds, "s, ", std::to_string(result.missing.size()), " not found");
}

D2SoundImportJob::D2SoundImportJob(D2SoundSource source, std::string installPath, std::vector<D2SoundMapEntry> entries, D2SoundSink &sink)
    : impl_(std::make_unique<Impl>(sink))
{
	impl_->result.source = source;
	impl_->result.installPath = std::move(installPath);
	impl_->entries = std::move(entries);
}

D2SoundImportJob::~D2SoundImportJob() = default;

int D2SoundImportJob::Step(std::chrono::steady_clock::duration budget)
{
	Impl &impl = *impl_;
	if (impl.state == Impl::State::NotStarted) {
		if (!impl.Begin())
			return 100;
		// Opening the storage can take a while, so we return to update the progress.
		return 1;
	}
	if (impl.state == Impl::State::Done)
		return 100;

	const auto deadline = std::chrono::steady_clock::now() + budget;
	do {
		if (impl.nextEntry >= impl.entries.size()) {
			impl.Finish();
			return 100;
		}
		impl.ImportEntry(impl.entries[impl.nextEntry++]);
	} while (std::chrono::steady_clock::now() < deadline);
	return std::clamp(static_cast<int>(impl.nextEntry * 99 / impl.entries.size()), 1, 99);
}

void D2SoundImportJob::Cancel()
{
	if (impl_->state == Impl::State::Done)
		return;
	impl_->state = Impl::State::Done;
	impl_->reader = nullptr;
	impl_->result.cancelled = true;
}

const D2SoundImportResult &D2SoundImportJob::Result() const
{
	return impl_->result;
}

} // namespace devilution
