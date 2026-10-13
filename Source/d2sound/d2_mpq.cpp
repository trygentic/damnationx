#include "d2sound/d2_mpq.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

#include <adpcm.h>
#include <huff.h>
#include <pklib.h>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace devilution {

namespace {

constexpr uint32_t MpqSignature = 0x1A51504D;         // "MPQ\x1A"
constexpr uint32_t MpqUserDataSignature = 0x1B51504D; // "MPQ\x1B"

constexpr uint32_t MpqFileImplode = 0x00000100;
constexpr uint32_t MpqFileCompress = 0x00000200;
constexpr uint32_t MpqFileEncrypted = 0x00010000;
constexpr uint32_t MpqFileFixKey = 0x00020000;
constexpr uint32_t MpqFileSingleUnit = 0x01000000;
constexpr uint32_t MpqFileExists = 0x80000000;

constexpr uint8_t MpqCompressionHuffman = 0x01;
constexpr uint8_t MpqCompressionZlib = 0x02;
constexpr uint8_t MpqCompressionPkware = 0x08;
constexpr uint8_t MpqCompressionAdpcmMono = 0x40;
constexpr uint8_t MpqCompressionAdpcmStereo = 0x80;

constexpr uint32_t HashTableEmpty = 0xFFFFFFFF;
constexpr uint32_t HashTableDeleted = 0xFFFFFFFE;

enum HashType : uint32_t {
	HashTableOffset = 0,
	HashNameA = 1,
	HashNameB = 2,
	HashFileKey = 3,
};

const std::array<uint32_t, 0x500> &CryptTable()
{
	static const std::array<uint32_t, 0x500> table = [] {
		std::array<uint32_t, 0x500> result {};
		uint32_t seed = 0x00100001;
		for (uint32_t index1 = 0; index1 < 0x100; index1++) {
			for (uint32_t i = 0, index2 = index1; i < 5; i++, index2 += 0x100) {
				seed = (seed * 125 + 3) % 0x2AAAAB;
				const uint32_t temp1 = (seed & 0xFFFF) << 0x10;
				seed = (seed * 125 + 3) % 0x2AAAAB;
				const uint32_t temp2 = (seed & 0xFFFF);
				result[index2] = temp1 | temp2;
			}
		}
		return result;
	}();
	return table;
}

uint32_t HashString(std::string_view str, HashType hashType)
{
	const std::array<uint32_t, 0x500> &cryptTable = CryptTable();
	uint32_t seed1 = 0x7FED7FED;
	uint32_t seed2 = 0xEEEEEEEE;
	for (char c : str) {
		auto ch = static_cast<uint8_t>(c);
		if (ch == '/') ch = '\\';
		if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
		seed1 = cryptTable[(hashType << 8) + ch] ^ (seed1 + seed2);
		seed2 = ch + seed1 + seed2 + (seed2 << 5) + 3;
	}
	return seed1;
}

uint32_t LoadLE32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t LoadLE16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void StoreLE32(uint8_t *p, uint32_t value)
{
	p[0] = static_cast<uint8_t>(value);
	p[1] = static_cast<uint8_t>(value >> 8);
	p[2] = static_cast<uint8_t>(value >> 16);
	p[3] = static_cast<uint8_t>(value >> 24);
}

/** @brief Decrypts the whole 32-bit words of `data` in place. */
void DecryptBlock(uint8_t *data, size_t size, uint32_t key)
{
	const std::array<uint32_t, 0x500> &cryptTable = CryptTable();
	uint32_t seed = 0xEEEEEEEE;
	for (size_t i = 0; i + 4 <= size; i += 4) {
		seed += cryptTable[0x400 + (key & 0xFF)];
		const uint32_t ch = LoadLE32(data + i) ^ (key + seed);
		key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
		seed = ch + seed + (seed << 5) + 3;
		StoreLE32(data + i, ch);
	}
}

struct PkwareBuffers {
	const uint8_t *in;
	const uint8_t *inEnd;
	uint8_t *out;
	uint8_t *outEnd;
};

unsigned int PkwareRead(char *buf, unsigned int *size, void *param)
{
	auto *info = static_cast<PkwareBuffers *>(param);
	const auto toRead = static_cast<unsigned int>(std::min<size_t>(*size, info->inEnd - info->in));
	std::memcpy(buf, info->in, toRead);
	info->in += toRead;
	return toRead;
}

void PkwareWrite(char *buf, unsigned int *size, void *param)
{
	auto *info = static_cast<PkwareBuffers *>(param);
	const auto toWrite = static_cast<unsigned int>(std::min<size_t>(*size, info->outEnd - info->out));
	std::memcpy(info->out, buf, toWrite);
	info->out += toWrite;
}

bool Explode(const uint8_t *in, size_t inSize, uint8_t *out, size_t &outSize)
{
	PkwareBuffers info { in, in + inSize, out, out + outSize };
	const std::unique_ptr<char[]> workBuffer { new char[EXP_BUFFER_SIZE]() };
	const unsigned int result = explode(&PkwareRead, &PkwareWrite, workBuffer.get(), &info);
	outSize = static_cast<size_t>(info.out - out);
	return result == CMP_NO_ERROR;
}

bool DecompressStep(uint8_t compression, const uint8_t *in, size_t inSize, uint8_t *out, size_t &outSize)
{
	switch (compression) {
	case MpqCompressionPkware:
		return Explode(in, inSize, out, outSize);
	case MpqCompressionZlib: {
		auto destLen = static_cast<uLongf>(outSize);
		if (uncompress(out, &destLen, in, static_cast<uLong>(inSize)) != Z_OK)
			return false;
		outSize = destLen;
		return true;
	}
	case MpqCompressionHuffman: {
		THuffmannTree tree(/*bCompression=*/false);
		TInputStream inputStream(const_cast<uint8_t *>(in), inSize);
		outSize = tree.Decompress(out, static_cast<unsigned int>(outSize), &inputStream);
		return outSize != 0;
	}
	case MpqCompressionAdpcmMono:
	case MpqCompressionAdpcmStereo: {
		const int channels = compression == MpqCompressionAdpcmMono ? 1 : 2;
		const int result = DecompressADPCM(out, static_cast<int>(outSize), const_cast<uint8_t *>(in), static_cast<int>(inSize), channels);
		if (result <= 0)
			return false;
		outSize = static_cast<size_t>(result);
		return true;
	}
	default:
		return false;
	}
}

/**
 * @brief Decompresses a sector compressed with one or more methods (MPQ_FILE_COMPRESS).
 * The first byte is a mask of the compressions that were applied.
 */
bool DecompressMulti(const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize)
{
	if (inSize < 1)
		return false;
	const uint8_t mask = in[0];
	++in;
	--inSize;

	// The order in which the decompressions are applied (the reverse of the compression order).
	constexpr std::array<uint8_t, 5> Order = { MpqCompressionPkware, MpqCompressionZlib, MpqCompressionHuffman, MpqCompressionAdpcmStereo, MpqCompressionAdpcmMono };
	uint8_t supported = 0;
	for (const uint8_t compression : Order)
		supported |= compression;
	if (mask == 0 || (mask & ~supported) != 0)
		return false;

	size_t numSteps = 0;
	for (const uint8_t compression : Order) {
		if ((mask & compression) != 0)
			++numSteps;
	}

	std::vector<uint8_t> buffers[2];
	const uint8_t *current = in;
	size_t currentSize = inSize;
	size_t step = 0;
	for (const uint8_t compression : Order) {
		if ((mask & compression) == 0)
			continue;
		++step;
		uint8_t *target;
		if (step == numSteps) {
			target = out;
		} else {
			std::vector<uint8_t> &buffer = buffers[step % 2];
			buffer.resize(outSize);
			target = buffer.data();
		}
		size_t targetSize = outSize;
		if (!DecompressStep(compression, current, currentSize, target, targetSize))
			return false;
		current = target;
		currentSize = targetSize;
	}
	return currentSize == outSize;
}

bool DecompressSector(uint32_t flags, const uint8_t *in, size_t inSize, uint8_t *out, size_t outSize)
{
	if ((flags & MpqFileImplode) != 0) {
		size_t written = outSize;
		return Explode(in, inSize, out, written) && written == outSize;
	}
	return DecompressMulti(in, inSize, out, outSize);
}

std::string_view Basename(std::string_view filename)
{
	const size_t pos = filename.find_last_of("\\/");
	return pos == std::string_view::npos ? filename : filename.substr(pos + 1);
}

/**
 * @brief Opens a file for reading. The path is UTF-8.
 *
 * This file does not use the engine's file utilities, so that it can also be built into the
 * browser's import worker (see d2sound_web_worker.cpp).
 */
FILE *OpenFileForReading(const std::string &path)
{
#ifdef _WIN32
	const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (length <= 0)
		return nullptr;
	std::wstring widePath(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), length);
	return _wfopen(widePath.c_str(), L"rb");
#else
	return std::fopen(path.c_str(), "rb");
#endif
}

} // namespace

D2MpqArchive::~D2MpqArchive()
{
	if (file_ != nullptr)
		std::fclose(file_);
}

bool D2MpqArchive::ReadAt(uint64_t offset, void *dst, size_t size) const
{
#ifdef _WIN32
	if (_fseeki64(file_, static_cast<int64_t>(offset), SEEK_SET) != 0)
		return false;
#else
	if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0)
		return false;
#endif
	return size == 0 || std::fread(dst, size, 1, file_) == 1;
}

std::unique_ptr<D2MpqArchive> D2MpqArchive::Open(const std::string &path)
{
	FILE *file = OpenFileForReading(path);
	if (file == nullptr)
		return nullptr;
	std::unique_ptr<D2MpqArchive> archive { new D2MpqArchive() };
	archive->file_ = file;

	// The header is located at a multiple of 512 bytes (usually at the start of the file).
	constexpr uint64_t MaxHeaderSearchOffset = 64 * 1024 * 1024;
	std::array<uint8_t, 32> header;
	bool found = false;
	for (uint64_t offset = 0; offset < MaxHeaderSearchOffset; offset += 0x200) {
		if (!archive->ReadAt(offset, header.data(), header.size()))
			return nullptr;
		const uint32_t signature = LoadLE32(header.data());
		if (signature == MpqUserDataSignature) {
			const uint32_t headerOffset = LoadLE32(&header[8]);
			if (!archive->ReadAt(offset + headerOffset, header.data(), header.size()))
				return nullptr;
			if (LoadLE32(header.data()) != MpqSignature)
				continue;
			offset += headerOffset;
		} else if (signature != MpqSignature) {
			continue;
		}
		archive->archiveOffset_ = offset;
		found = true;
		break;
	}
	if (!found)
		return nullptr;

	const uint16_t sectorSizeShift = LoadLE16(&header[14]);
	const uint32_t hashTablePos = LoadLE32(&header[16]);
	const uint32_t blockTablePos = LoadLE32(&header[20]);
	const uint32_t hashTableSize = LoadLE32(&header[24]);
	const uint32_t blockTableSize = LoadLE32(&header[28]);
	// Only the v1 tables are used: Diablo II archives don't have the v2+ extensions.
	if (sectorSizeShift > 20 || hashTableSize == 0 || (hashTableSize & (hashTableSize - 1)) != 0 || hashTableSize > (1U << 20) || blockTableSize > (1U << 20))
		return nullptr;
	archive->sectorSize_ = 512U << sectorSizeShift;

	std::vector<uint8_t> buffer(static_cast<size_t>(hashTableSize) * 16);
	if (!archive->ReadAt(archive->archiveOffset_ + hashTablePos, buffer.data(), buffer.size()))
		return nullptr;
	DecryptBlock(buffer.data(), buffer.size(), HashString("(hash table)", HashFileKey));
	archive->hashTable_.resize(hashTableSize);
	for (size_t i = 0; i < hashTableSize; i++) {
		const uint8_t *p = &buffer[i * 16];
		archive->hashTable_[i] = HashEntry { LoadLE32(p), LoadLE32(p + 4), LoadLE16(p + 8), LoadLE16(p + 10), LoadLE32(p + 12) };
	}

	buffer.resize(static_cast<size_t>(blockTableSize) * 16);
	if (!archive->ReadAt(archive->archiveOffset_ + blockTablePos, buffer.data(), buffer.size()))
		return nullptr;
	DecryptBlock(buffer.data(), buffer.size(), HashString("(block table)", HashFileKey));
	archive->blockTable_.resize(blockTableSize);
	for (size_t i = 0; i < blockTableSize; i++) {
		const uint8_t *p = &buffer[i * 16];
		archive->blockTable_[i] = BlockEntry { LoadLE32(p), LoadLE32(p + 4), LoadLE32(p + 8), LoadLE32(p + 12) };
	}

	return archive;
}

const D2MpqArchive::BlockEntry *D2MpqArchive::FindBlock(std::string_view filename) const
{
	const auto mask = static_cast<uint32_t>(hashTable_.size() - 1);
	const uint32_t start = HashString(filename, HashTableOffset) & mask;
	const uint32_t nameA = HashString(filename, HashNameA);
	const uint32_t nameB = HashString(filename, HashNameB);
	const BlockEntry *result = nullptr;
	for (uint32_t i = start;;) {
		const HashEntry &entry = hashTable_[i];
		if (entry.blockIndex == HashTableEmpty)
			break;
		if (entry.blockIndex != HashTableDeleted && entry.nameA == nameA && entry.nameB == nameB && entry.blockIndex < blockTable_.size()) {
			const BlockEntry &block = blockTable_[entry.blockIndex];
			if ((block.flags & MpqFileExists) != 0) {
				// Prefer the language-neutral version of a file.
				if (entry.locale == 0)
					return &block;
				if (result == nullptr)
					result = &block;
			}
		}
		i = (i + 1) & mask;
		if (i == start)
			break;
	}
	return result;
}

bool D2MpqArchive::HasFile(std::string_view filename) const
{
	return FindBlock(filename) != nullptr;
}

std::optional<std::vector<uint8_t>> D2MpqArchive::ReadFile(std::string_view filename) const
{
	const BlockEntry *block = FindBlock(filename);
	if (block == nullptr)
		return std::nullopt;

	const uint32_t flags = block->flags;
	const bool isCompressed = (flags & (MpqFileImplode | MpqFileCompress)) != 0;
	uint32_t key = 0;
	if ((flags & MpqFileEncrypted) != 0) {
		key = HashString(Basename(filename), HashFileKey);
		if ((flags & MpqFileFixKey) != 0)
			key = (key + block->filePos) ^ block->fileSize;
	}

	std::vector<uint8_t> result(block->fileSize);
	if (block->fileSize == 0)
		return result;
	const uint64_t basePos = archiveOffset_ + block->filePos;

	if ((flags & MpqFileSingleUnit) != 0) {
		std::vector<uint8_t> raw(block->compressedSize);
		if (!ReadAt(basePos, raw.data(), raw.size()))
			return std::nullopt;
		if ((flags & MpqFileEncrypted) != 0)
			DecryptBlock(raw.data(), raw.size(), key);
		if (isCompressed && raw.size() < result.size()) {
			if (!DecompressSector(flags, raw.data(), raw.size(), result.data(), result.size()))
				return std::nullopt;
		} else {
			if (raw.size() < result.size())
				return std::nullopt;
			std::memcpy(result.data(), raw.data(), result.size());
		}
		return result;
	}

	const size_t numSectors = (block->fileSize + sectorSize_ - 1) / sectorSize_;
	std::vector<uint32_t> sectorOffsets(numSectors + 1);
	if (isCompressed) {
		std::vector<uint8_t> raw(sectorOffsets.size() * 4);
		if (!ReadAt(basePos, raw.data(), raw.size()))
			return std::nullopt;
		if ((flags & MpqFileEncrypted) != 0)
			DecryptBlock(raw.data(), raw.size(), key - 1);
		for (size_t i = 0; i < sectorOffsets.size(); i++) {
			sectorOffsets[i] = LoadLE32(&raw[i * 4]);
			if (sectorOffsets[i] > block->compressedSize || (i > 0 && sectorOffsets[i] < sectorOffsets[i - 1]))
				return std::nullopt;
		}
	} else {
		for (size_t i = 0; i < numSectors; i++)
			sectorOffsets[i] = static_cast<uint32_t>(i * sectorSize_);
		sectorOffsets[numSectors] = block->fileSize;
	}

	std::vector<uint8_t> raw;
	for (size_t i = 0; i < numSectors; i++) {
		const size_t outOffset = i * sectorSize_;
		const size_t expected = std::min<size_t>(sectorSize_, block->fileSize - outOffset);
		raw.resize(sectorOffsets[i + 1] - sectorOffsets[i]);
		if (!ReadAt(basePos + sectorOffsets[i], raw.data(), raw.size()))
			return std::nullopt;
		if ((flags & MpqFileEncrypted) != 0)
			DecryptBlock(raw.data(), raw.size(), key + static_cast<uint32_t>(i));
		if (isCompressed && raw.size() < expected) {
			if (!DecompressSector(flags, raw.data(), raw.size(), &result[outOffset], expected))
				return std::nullopt;
		} else {
			if (raw.size() < expected)
				return std::nullopt;
			std::memcpy(&result[outOffset], raw.data(), expected);
		}
	}
	return result;
}

} // namespace devilution
