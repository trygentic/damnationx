/**
 * @file d2sound/d2_mpq.hpp
 *
 * A minimal read-only reader for the MPQ (v1) archives of Diablo II.
 *
 * The engine's own MPQ reader (mpqfs) only supports what Diablo uses. Diablo II stores most of
 * its sounds with the "wave" compression (IMA ADPCM + Huffman), so the sound importer uses this
 * reader with StormLib's decompressors instead.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

class D2MpqArchive {
public:
	/** @brief Opens an MPQ archive. Returns nullptr if the file is not a supported MPQ archive. */
	static std::unique_ptr<D2MpqArchive> Open(const std::string &path);

	~D2MpqArchive();
	D2MpqArchive(const D2MpqArchive &) = delete;
	D2MpqArchive &operator=(const D2MpqArchive &) = delete;

	/** @brief Whether the archive contains a file with this name (case-insensitive, `\` separators). */
	[[nodiscard]] bool HasFile(std::string_view filename) const;

	/** @brief Reads and decompresses a file. Returns std::nullopt if it is missing or can't be decoded. */
	[[nodiscard]] std::optional<std::vector<uint8_t>> ReadFile(std::string_view filename) const;

	struct HashEntry {
		uint32_t nameA;
		uint32_t nameB;
		uint16_t locale;
		uint16_t platform;
		uint32_t blockIndex;
	};

	struct BlockEntry {
		uint32_t filePos;
		uint32_t compressedSize;
		uint32_t fileSize;
		uint32_t flags;
	};

private:
	D2MpqArchive() = default;

	[[nodiscard]] const BlockEntry *FindBlock(std::string_view filename) const;
	bool ReadAt(uint64_t offset, void *dst, size_t size) const;

	FILE *file_ = nullptr;
	uint64_t archiveOffset_ = 0;
	uint32_t sectorSize_ = 0;
	std::vector<HashEntry> hashTable_;
	std::vector<BlockEntry> blockTable_;
};

} // namespace devilution
