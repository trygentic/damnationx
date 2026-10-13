/**
 * @file hd_floor.hpp
 *
 * Parser and piece lookup for the opt-in HD town floor manifest (hd/town-floor.txt, DamnationX HD runtime
 * contract v1 section 1). Pure data: no GPU, asset or game-state access, so it can be unit tested natively.
 *
 *     atlas <N> <file> <width> <height>   page N, 4096 x up to 4096, cells 256x128 laid out 16 per row
 *     piece <piece_index> <page> <cell>   piece_index is the 0-based dPiece value DrawFloorTile draws
 */
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

inline constexpr int HdFloorPageWidth = 4096;
inline constexpr int HdFloorMaxPageHeight = 4096;
inline constexpr int HdFloorCellWidth = 256;
inline constexpr int HdFloorCellHeight = 128;
inline constexpr int HdFloorCellsPerRow = HdFloorPageWidth / HdFloorCellWidth; // 16
/** The presenter packs page and cell into 15 bits of the floor metadata: 6 bits page, 9 bits cell. */
inline constexpr int HdFloorMaxPages = 64;
inline constexpr int HdFloorMaxCellsPerPage = HdFloorCellsPerRow * (HdFloorMaxPageHeight / HdFloorCellHeight); // 512

struct HdFloorPage {
	/** Asset path for FindAsset/LoadPNG, e.g. "hd\\town-floor-atlas-0.png". */
	std::string path;
	int width = 0;
	int height = 0;
};

struct HdFloorCell {
	uint8_t page = 0;
	uint16_t cell = 0;
};

struct HdFloorManifest {
	/** Pages in index order; indices are contiguous from 0. */
	std::vector<HdFloorPage> pages;
	/** Indexed by piece id; pieces without an entry hold std::nullopt. */
	std::vector<std::optional<HdFloorCell>> pieces;

	[[nodiscard]] std::optional<HdFloorCell> Lookup(size_t piece) const
	{
		return piece < pieces.size() ? pieces[piece] : std::nullopt;
	}

	[[nodiscard]] int MaxPageHeight() const;
};

/**
 * @brief Parses hd/town-floor.txt.
 *
 * Validates the whole file: page indices contiguous from 0 (at most HdFloorMaxPages), width 4096, height a
 * positive multiple of 128 up to 4096, every piece referencing an existing page and a cell inside that page,
 * and no piece listed twice. Relative atlas file names resolve inside the "hd" asset directory.
 * @return The manifest, or a message naming the offending line.
 */
std::expected<HdFloorManifest, std::string> ParseHdFloorManifest(std::string_view text);

} // namespace devilution
