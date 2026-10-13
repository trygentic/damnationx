#include "engine/render/hd_floor.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>

namespace devilution {

namespace {

std::string_view Trim(std::string_view text)
{
	constexpr std::string_view Whitespace = " \t\r\n\v\f";
	const size_t begin = text.find_first_not_of(Whitespace);
	if (begin == std::string_view::npos)
		return {};
	const size_t end = text.find_last_not_of(Whitespace);
	return text.substr(begin, end - begin + 1);
}

std::vector<std::string_view> SplitFields(std::string_view line)
{
	constexpr std::string_view Whitespace = " \t\r\v\f";
	std::vector<std::string_view> fields;
	size_t position = 0;
	while (true) {
		position = line.find_first_not_of(Whitespace, position);
		if (position == std::string_view::npos)
			break;
		const size_t end = std::min(line.find_first_of(Whitespace, position), line.size());
		fields.push_back(line.substr(position, end - position));
		position = end;
	}
	return fields;
}

std::optional<int> ParseNonNegative(std::string_view field)
{
	int value = 0;
	const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
	if (ec != std::errc() || ptr != field.data() + field.size() || value < 0)
		return std::nullopt;
	return value;
}

std::string ResolveAtlasPath(std::string_view file)
{
	std::string path(file);
	std::replace(path.begin(), path.end(), '/', '\\');
	if (!path.starts_with("hd\\"))
		path.insert(0, "hd\\");
	return path;
}

std::unexpected<std::string> LineError(size_t lineNumber, std::string_view message)
{
	return std::unexpected("town-floor.txt line " + std::to_string(lineNumber) + ": " + std::string(message));
}

struct PendingPiece {
	size_t lineNumber;
	int piece;
	int page;
	int cell;
};

} // namespace

int HdFloorManifest::MaxPageHeight() const
{
	int height = 0;
	for (const HdFloorPage &page : pages)
		height = std::max(height, page.height);
	return height;
}

std::expected<HdFloorManifest, std::string> ParseHdFloorManifest(std::string_view text)
{
	HdFloorManifest manifest;
	std::vector<std::optional<HdFloorPage>> pages;
	std::vector<PendingPiece> pendingPieces;

	size_t lineNumber = 0;
	while (!text.empty()) {
		++lineNumber;
		const size_t newline = text.find('\n');
		std::string_view line = text.substr(0, newline);
		text = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);
		if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF"))
			line.remove_prefix(3);
		line = Trim(line.substr(0, line.find('#')));
		if (line.empty())
			continue;

		const std::vector<std::string_view> fields = SplitFields(line);
		if (fields[0] == "atlas") {
			if (fields.size() != 5)
				return LineError(lineNumber, "expected 'atlas <N> <file> <width> <height>'");
			const std::optional<int> index = ParseNonNegative(fields[1]);
			const std::optional<int> width = ParseNonNegative(fields[3]);
			const std::optional<int> height = ParseNonNegative(fields[4]);
			if (!index || !width || !height)
				return LineError(lineNumber, "atlas index, width and height must be non-negative integers");
			if (*index >= HdFloorMaxPages)
				return LineError(lineNumber, "atlas index must be below " + std::to_string(HdFloorMaxPages));
			if (*width != HdFloorPageWidth)
				return LineError(lineNumber, "atlas width must be " + std::to_string(HdFloorPageWidth));
			if (*height <= 0 || *height > HdFloorMaxPageHeight || *height % HdFloorCellHeight != 0)
				return LineError(lineNumber, "atlas height must be a multiple of 128 between 128 and 4096");
			if (static_cast<size_t>(*index) >= pages.size())
				pages.resize(static_cast<size_t>(*index) + 1);
			if (pages[*index].has_value())
				return LineError(lineNumber, "atlas " + std::to_string(*index) + " is defined twice");
			pages[*index] = HdFloorPage { ResolveAtlasPath(fields[2]), *width, *height };
		} else if (fields[0] == "piece") {
			if (fields.size() != 4)
				return LineError(lineNumber, "expected 'piece <piece_index> <page> <cell>'");
			const std::optional<int> piece = ParseNonNegative(fields[1]);
			const std::optional<int> page = ParseNonNegative(fields[2]);
			const std::optional<int> cell = ParseNonNegative(fields[3]);
			if (!piece || !page || !cell)
				return LineError(lineNumber, "piece index, page and cell must be non-negative integers");
			if (*piece > std::numeric_limits<uint16_t>::max())
				return LineError(lineNumber, "piece index out of range");
			pendingPieces.push_back({ lineNumber, *piece, *page, *cell });
		} else {
			return LineError(lineNumber, "unknown directive '" + std::string(fields[0]) + "'");
		}
	}

	if (pages.empty())
		return std::unexpected("town-floor.txt defines no atlas pages");
	for (size_t i = 0; i < pages.size(); ++i) {
		if (!pages[i].has_value())
			return std::unexpected("town-floor.txt is missing atlas page " + std::to_string(i));
		manifest.pages.push_back(*pages[i]);
	}

	for (const PendingPiece &pending : pendingPieces) {
		if (static_cast<size_t>(pending.page) >= manifest.pages.size())
			return LineError(pending.lineNumber, "page " + std::to_string(pending.page) + " is not defined");
		const HdFloorPage &page = manifest.pages[pending.page];
		const int cellsOnPage = HdFloorCellsPerRow * (page.height / HdFloorCellHeight);
		if (pending.cell >= cellsOnPage)
			return LineError(pending.lineNumber, "cell " + std::to_string(pending.cell) + " is outside page " + std::to_string(pending.page));
		if (static_cast<size_t>(pending.piece) >= manifest.pieces.size())
			manifest.pieces.resize(static_cast<size_t>(pending.piece) + 1);
		std::optional<HdFloorCell> &entry = manifest.pieces[pending.piece];
		if (entry.has_value())
			return LineError(pending.lineNumber, "piece " + std::to_string(pending.piece) + " is listed twice");
		entry = HdFloorCell { static_cast<uint8_t>(pending.page), static_cast<uint16_t>(pending.cell) };
	}
	return manifest;
}

} // namespace devilution
