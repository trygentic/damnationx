#pragma once
/**
 * @file clx_sprite.hpp
 *
 * @brief CLX format sprites.
 *
 * CLX is a format used for DevilutionX graphics at runtime.
 * CLX encodes pixel in the same way CL2 but encodes metadata differently.
 *
 * Unlike CL2:
 *
 * 1. CLX frame header stores frame width and height.
 * 2. CLX frame header does not store 32-pixel block offsets.
 *
 * CLX frame header is 6 bytes:
 *
 *  Bytes |   Type   | Value
 * :-----:|:--------:|-------------
 *  0..2  | uint16_t | header size
 *  2..4  | uint16_t | width
 *  4..6  | uint16_t | height
 *
 * CL2 reference: https://github.com/savagesteel/d1-file-formats/blob/master/PC-Mac/CL2.md#2-file-structure
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

#include "appfat.h"
#include "utils/endian_read.hpp"
#include "utils/intrusive_optional.hpp"

namespace devilution {

namespace clx_normal_map {
/** Evict-on-free hook for the sprite normal-plane cache (clx_normal_map.hpp); called by the Owned* sprite destructors. */
void EvictNormalPlanes(const void *dataBegin, size_t dataSize);
} // namespace clx_normal_map

/** Asset/WebGPU metadata contracts; classic CPU palette compositing remains unchanged. */
enum class ClxAtlasPageKind : uint8_t { Indexed, Rgba, Material };
struct ClxAtlasCoverage { uint32_t x = 0, y = 0, width = 0, height = 0; };
struct ClxFrameUv { float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };
struct ClxLinearDataDescriptor { uint32_t bytesPerPixel = 0; bool linear = true; bool nearest = true; };
struct ClxAssetCacheKey {
	uint64_t contentVersion = 0, materialVersion = 0, paletteVersion = 0;
	constexpr bool operator==(const ClxAssetCacheKey &o) const { return contentVersion == o.contentVersion && materialVersion == o.materialVersion && paletteVersion == o.paletteVersion; }
	constexpr bool operator!=(const ClxAssetCacheKey &o) const { return !(*this == o); }
};
struct ClxResourceHandle {
	static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();
	uint32_t index = InvalidIndex, generation = 0;
	constexpr bool isValid() const { return index != InvalidIndex && generation != 0; }
	constexpr bool operator==(const ClxResourceHandle &o) const { return index == o.index && generation == o.generation; }
};
struct ClxFrameMetadata {
	ClxResourceHandle page;
	ClxAtlasPageKind pageKind = ClxAtlasPageKind::Indexed;
	ClxAtlasCoverage coverage;
	ClxFrameUv uv;
	ClxLinearDataDescriptor data;
	ClxAssetCacheKey cacheKey;
};

class OptionalClxSprite;

/** Renderer-independent metadata carried alongside an animation frame. */
enum class SpriteMaterialFlags : uint32_t {
	None = 0,
	Indexed = 1u << 0,
	Rgba = 1u << 1,
	Normal = 1u << 2,
	Emissive = 1u << 3,
	Height = 1u << 4,
};

constexpr SpriteMaterialFlags operator|(SpriteMaterialFlags lhs, SpriteMaterialFlags rhs)
{
	return static_cast<SpriteMaterialFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

enum class SpriteVisibilityFlags : uint32_t {
	None = 0,
	Covered = 1u << 0,
	Outline = 1u << 1,
	Petrified = 1u << 2,
	Infravision = 1u << 3,
};

constexpr SpriteVisibilityFlags operator|(SpriteVisibilityFlags lhs, SpriteVisibilityFlags rhs)
{
	return static_cast<SpriteVisibilityFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

/** Logical, source-independent bounds in animation space. */
struct SpriteLogicalBounds {
	int16_t left = 0;
	int16_t top = 0;
	int16_t right = 0;
	int16_t bottom = 0;
};

/** Metadata required to render one frame without inspecting its asset source. */
struct SpriteFrameMetadata {
	uint8_t direction = 0;
	uint32_t frameId = 0;
	int16_t pivotX = 0;
	int16_t pivotY = 0;
	SpriteLogicalBounds logicalBounds;
	bool mirrored = false;
	SpriteMaterialFlags materialFlags = SpriteMaterialFlags::Indexed;
	SpriteVisibilityFlags visibilityFlags = SpriteVisibilityFlags::None;
};

/** Stable identity shared by original and replacement animation sources. */
struct SpriteAnimationMetadata {
	uint8_t direction = 0;
	uint32_t animationId = 0;
	uint32_t frameCount = 0;
};

/**
 * @brief A single CLX sprite.
 */
class ClxSprite {
public:
	explicit constexpr ClxSprite(const uint8_t *data, uint32_t dataSize)
	    : data_(data)
	    , pixel_data_size_(dataSize - LoadLE16(data))
	{
		assert(data != nullptr);
	}

	[[nodiscard]] constexpr uint16_t width() const
	{
		return LoadLE16(&data_[2]);
	}

	[[nodiscard]] constexpr uint16_t height() const
	{
		return LoadLE16(&data_[4]);
	}

	/**
	 * @brief The raw pixel data (CL2 frame data).
	 *
	 * Format: https://github.com/savagesteel/d1-file-formats/blob/master/PC-Mac/CL2.md#42-cl2-frame-data
	 */
	[[nodiscard]] constexpr const uint8_t *pixelData() const
	{
		return &data_[LoadLE16(data_)];
	}

	[[nodiscard]] constexpr uint32_t pixelDataSize() const
	{
		return pixel_data_size_;
	}

	/** The whole frame (header + pixel data). */
	[[nodiscard]] constexpr const uint8_t *frameData() const
	{
		return data_;
	}

	[[nodiscard]] constexpr uint32_t frameSize() const
	{
		return pixel_data_size_ + LoadLE16(data_);
	}

	constexpr bool operator==(const ClxSprite &other) const
	{
		return data_ == other.data_;
	}

	constexpr bool operator!=(const ClxSprite &other) const
	{
		return !(*this == other);
	}

private:
	// For OptionalClxSprite.
	constexpr ClxSprite() = default;

	const uint8_t *data_ = nullptr;
	uint32_t pixel_data_size_ = 0;

	friend class OptionalClxSprite;
};

class OwnedClxSpriteList;
class OptionalClxSpriteList;
class ClxSpriteListIterator;

/**
 * @brief A list of `ClxSprite`s.
 */
class ClxSpriteList {
public:
	explicit constexpr ClxSpriteList(const uint8_t *data)
	    : data_(data)
	{
		assert(data != nullptr);
	}

	ClxSpriteList(const OwnedClxSpriteList &owned);

	[[nodiscard]] OwnedClxSpriteList clone() const;

	[[nodiscard]] constexpr uint32_t numSprites() const
	{
		return LoadLE32(data_);
	}

	[[nodiscard]] constexpr ClxSprite operator[](size_t spriteIndex) const
	{
		assert(spriteIndex < numSprites());
		const uint32_t begin = spriteOffset(spriteIndex);
		const uint32_t end = spriteOffset(spriteIndex + 1);
		return ClxSprite { &data_[begin], end - begin };
	}

	[[nodiscard]] constexpr uint32_t spriteOffset(size_t spriteIndex) const
	{
		return LoadLE32(&data_[4 + spriteIndex * 4]);
	}

	/** @brief The offset to the next sprite sheet, or file size if this is the last sprite sheet. */
	[[nodiscard]] constexpr uint32_t dataSize() const
	{
		return LoadLE32(&data_[4 + numSprites() * 4]);
	}

	[[nodiscard]] constexpr const uint8_t *data() const
	{
		return data_;
	}

	[[nodiscard]] constexpr ClxSpriteListIterator begin() const;
	[[nodiscard]] constexpr ClxSpriteListIterator end() const;

private:
	// For OptionalClxSpriteList.
	constexpr ClxSpriteList() = default;

	const uint8_t *data_ = nullptr;

	friend class OptionalClxSpriteList;
};

class ClxSpriteListIterator {
public:
	using iterator_category = std::forward_iterator_tag;
	using difference_type = int;
	using value_type = ClxSprite;
	using pointer = void;
	using reference = value_type &;

	constexpr ClxSpriteListIterator(ClxSpriteList list, size_t index)
	    : list_(list)
	    , index_(index)
	{
	}

	constexpr ClxSprite operator*()
	{
		return list_[index_];
	}

	constexpr ClxSpriteListIterator &operator++()
	{
		++index_;
		return *this;
	}

	constexpr ClxSpriteListIterator operator++(int)
	{
		auto copy = *this;
		++(*this);
		return copy;
	}

	constexpr bool operator==(const ClxSpriteListIterator &other) const
	{
		return index_ == other.index_;
	}

	constexpr bool operator!=(const ClxSpriteListIterator &other) const
	{
		return !(*this == other);
	}

private:
	ClxSpriteList list_;
	size_t index_;
};

inline constexpr ClxSpriteListIterator ClxSpriteList::begin() const
{
	return { *this, 0 };
}

inline constexpr ClxSpriteListIterator ClxSpriteList::end() const
{
	return { *this, numSprites() };
}

class OwnedClxSpriteSheet;
class OptionalClxSpriteSheet;
class ClxSpriteSheetIterator;

/**
 * @brief A sprite sheet is a list of `ClxSpriteList`s.
 */
class ClxSpriteSheet {
public:
	explicit constexpr ClxSpriteSheet(const uint8_t *data, uint16_t numLists)
	    : data_(data)
	    , num_lists_(numLists)
	{
		assert(data != nullptr);
		assert(num_lists_ > 0);
	}

	ClxSpriteSheet(const OwnedClxSpriteSheet &owned);

	[[nodiscard]] constexpr uint16_t numLists() const
	{
		return num_lists_;
	}

	[[nodiscard]] constexpr ClxSpriteList operator[](size_t sheetIndex) const
	{
		return ClxSpriteList { &data_[sheetOffset(sheetIndex)] };
	}

	[[nodiscard]] constexpr uint32_t sheetOffset(size_t sheetIndex) const
	{
		assert(sheetIndex < num_lists_);
		return LoadLE32(&data_[4 * sheetIndex]);
	}

	[[nodiscard]] constexpr const uint8_t *data() const
	{
		return data_;
	}

	[[nodiscard]] constexpr ClxSpriteSheetIterator begin() const;
	[[nodiscard]] constexpr ClxSpriteSheetIterator end() const;

	[[nodiscard]] size_t dataSize() const
	{
		return static_cast<size_t>(&data_[sheetOffset(num_lists_ - 1)] + (*this)[num_lists_ - 1].dataSize() - &data_[0]);
	}

private:
	// For OptionalClxSpriteSheet.
	constexpr ClxSpriteSheet() = default;

	const uint8_t *data_ = nullptr;
	uint16_t num_lists_ = 0;

	friend class OptionalClxSpriteSheet;
};

class ClxSpriteSheetIterator {
public:
	using iterator_category = std::forward_iterator_tag;
	using difference_type = int;
	using value_type = ClxSpriteList;
	using pointer = void;
	using reference = value_type &;

	constexpr ClxSpriteSheetIterator(ClxSpriteSheet sheet, size_t index)
	    : sheet_(sheet)
	    , index_(index)
	{
	}

	constexpr ClxSpriteList operator*()
	{
		return sheet_[index_];
	}

	constexpr ClxSpriteSheetIterator &operator++()
	{
		++index_;
		return *this;
	}

	constexpr ClxSpriteSheetIterator operator++(int)
	{
		auto copy = *this;
		++(*this);
		return copy;
	}

	constexpr bool operator==(const ClxSpriteSheetIterator &other) const
	{
		return index_ == other.index_;
	}

	constexpr bool operator!=(const ClxSpriteSheetIterator &other) const
	{
		return !(*this == other);
	}

private:
	ClxSpriteSheet sheet_;
	size_t index_;
};

inline constexpr ClxSpriteSheetIterator ClxSpriteSheet::begin() const
{
	return { *this, 0 };
}

inline constexpr ClxSpriteSheetIterator ClxSpriteSheet::end() const
{
	return { *this, num_lists_ };
}

class OptionalOwnedClxSpriteList;
class OwnedClxSpriteListOrSheet;

/**
 * @brief Implicitly convertible to `ClxSpriteList` and owns its data.
 */
class OwnedClxSpriteList {
public:
	explicit OwnedClxSpriteList(std::unique_ptr<uint8_t[]> &&data)
	    : data_(std::move(data))
	{
		assert(data_ != nullptr);
	}

	~OwnedClxSpriteList()
	{
		if (data_ != nullptr)
			clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
	}

	OwnedClxSpriteList(OwnedClxSpriteList &&) noexcept = default;
	OwnedClxSpriteList &operator=(OwnedClxSpriteList &&other) noexcept
	{
		if (this != &other) {
			// Evict before the old buffer is freed so a reused address cannot alias stale normal planes.
			if (data_ != nullptr)
				clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
			data_ = std::move(other.data_);
		}
		return *this;
	}

	[[nodiscard]] OwnedClxSpriteList clone() const
	{
		return ClxSpriteList { *this }.clone();
	}

	[[nodiscard]] ClxSprite operator[](size_t spriteIndex) const
	{
		return ClxSpriteList { *this }[spriteIndex];
	}

	[[nodiscard]] uint32_t numSprites() const
	{
		return ClxSpriteList { *this }.numSprites();
	}

	[[nodiscard]] size_t dataSize() const
	{
		return ClxSpriteList { *this }.dataSize();
	}

private:
	// For OptionalOwnedClxSpriteList.
	OwnedClxSpriteList() = default;

	std::unique_ptr<uint8_t[]> data_;

	friend class ClxSpriteList; // for implicit conversion
	friend class OptionalOwnedClxSpriteList;
	friend class OwnedClxSpriteListOrSheet;
};

inline ClxSpriteList::ClxSpriteList(const OwnedClxSpriteList &owned)
    : data_(owned.data_.get())
{
}

inline OwnedClxSpriteList ClxSpriteList::clone() const
{
	const size_t size = dataSize();
	std::unique_ptr<uint8_t[]> data { new uint8_t[size] };
	memcpy(data.get(), data_, size);
	return OwnedClxSpriteList { std::move(data) };
}

/**
 * @brief Implicitly convertible to `ClxSpriteSheet` and owns its data.
 */
class OwnedClxSpriteSheet {
public:
	OwnedClxSpriteSheet(std::unique_ptr<uint8_t[]> &&data, uint16_t numLists)
	    : data_(std::move(data))
	    , num_lists_(numLists)
	{
		assert(data_ != nullptr);
		assert(numLists > 0);
	}

	~OwnedClxSpriteSheet()
	{
		if (data_ != nullptr)
			clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
	}

	OwnedClxSpriteSheet(OwnedClxSpriteSheet &&) noexcept = default;
	OwnedClxSpriteSheet &operator=(OwnedClxSpriteSheet &&other) noexcept
	{
		if (this != &other) {
			// Evict before the old buffer is freed so a reused address cannot alias stale normal planes.
			if (data_ != nullptr)
				clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
			data_ = std::move(other.data_);
			num_lists_ = other.num_lists_;
		}
		return *this;
	}

	[[nodiscard]] ClxSpriteList operator[](size_t sheetIndex) const
	{
		return ClxSpriteSheet { *this }[sheetIndex];
	}

	[[nodiscard]] ClxSpriteSheetIterator begin() const
	{
		return ClxSpriteSheet { *this }.begin();
	}

	[[nodiscard]] ClxSpriteSheetIterator end() const
	{
		return ClxSpriteSheet { *this }.end();
	}

	[[nodiscard]] size_t dataSize() const
	{
		return ClxSpriteSheet { *this }.dataSize();
	}

private:
	// For OptionalOwnedClxSpriteList.
	OwnedClxSpriteSheet() = default;

	std::unique_ptr<uint8_t[]> data_;
	uint16_t num_lists_ = 0;

	friend class ClxSpriteSheet; // for implicit conversion.
	friend class OptionalOwnedClxSpriteSheet;
	friend class OwnedClxSpriteListOrSheet;
};

inline ClxSpriteSheet::ClxSpriteSheet(const OwnedClxSpriteSheet &owned)
    : data_(owned.data_.get())
    , num_lists_(owned.num_lists_)
{
}

class OwnedClxSpriteListOrSheet;
class OptionalClxSpriteListOrSheet;

inline bool IsValidClxSpriteList(const uint8_t *data, size_t size)
{
	if (data == nullptr || size < sizeof(uint32_t) * 2)
		return false;
	const uint32_t count = LoadLE32(data);
	if (count == 0 || count > (size - sizeof(uint32_t)) / sizeof(uint32_t) - 1)
		return false;
	const size_t tableSize = sizeof(uint32_t) + (static_cast<size_t>(count) + 1) * sizeof(uint32_t);
	if (tableSize > size)
		return false;
	uint32_t previous = static_cast<uint32_t>(tableSize);
	for (uint32_t i = 0; i <= count; ++i) {
		const uint32_t offset = LoadLE32(&data[sizeof(uint32_t) + static_cast<size_t>(i) * sizeof(uint32_t)]);
		if (offset < previous || offset > size)
			return false;
		previous = offset;
	}
	return true;
}

inline uint16_t GetNumListsFromClxListOrSheetBuffer(const uint8_t *data, size_t size)
{
	if (IsValidClxSpriteList(data, size))
		return 0;
	if (data == nullptr || size < sizeof(uint32_t) * 2)
		return 0;

	// A sheet begins with one offset per sprite list. The first offset is
	// therefore both the end of the sheet table and four times the list count.
	const uint32_t firstListOffset = LoadLE32(data);
	if (firstListOffset < sizeof(uint32_t) * 2 || firstListOffset % sizeof(uint32_t) != 0 || firstListOffset > size)
		return 0;
	const size_t numLists = firstListOffset / sizeof(uint32_t);
	if (numLists > std::numeric_limits<uint16_t>::max())
		return 0;

	for (size_t i = 0; i < numLists; ++i) {
		const uint32_t begin = LoadLE32(&data[i * sizeof(uint32_t)]);
		const uint32_t end = i + 1 < numLists ? LoadLE32(&data[(i + 1) * sizeof(uint32_t)]) : static_cast<uint32_t>(size);
		if (begin < firstListOffset || begin > end || end > size || !IsValidClxSpriteList(&data[begin], end - begin))
			return 0;
	}
	return static_cast<uint16_t>(numLists);
}

/**
 * @brief A CLX sprite list or a sprite sheet (list of lists).
 */
class ClxSpriteListOrSheet {
public:
	constexpr ClxSpriteListOrSheet(const uint8_t *data, uint16_t numLists)
	    : data_(data)
	    , num_lists_(numLists)
	{
	}

	ClxSpriteListOrSheet(const OwnedClxSpriteListOrSheet &listOrSheet);

	[[nodiscard]] constexpr ClxSpriteList list() const
	{
		assert(num_lists_ == 0);
		return ClxSpriteList { data_ };
	}

	[[nodiscard]] constexpr ClxSpriteSheet sheet() const
	{
		assert(num_lists_ != 0);
		return ClxSpriteSheet { data_, num_lists_ };
	}

	[[nodiscard]] constexpr bool isSheet() const
	{
		return num_lists_ != 0;
	}

	[[nodiscard]] size_t dataSize() const
	{
		return isSheet() ? sheet().dataSize() : list().dataSize();
	}

private:
	// For OptionalClxSpriteListOrSheet.
	constexpr ClxSpriteListOrSheet() = default;

	const uint8_t *data_ = nullptr;
	uint16_t num_lists_ = 0;

	friend class OptionalClxSpriteListOrSheet;
};

class OptionalOwnedClxSpriteListOrSheet;

/**
 * @brief A CLX sprite list or a sprite sheet (list of lists).
 */
class OwnedClxSpriteListOrSheet {
public:
	static OwnedClxSpriteListOrSheet FromBuffer(std::unique_ptr<uint8_t[]> &&data, size_t size)
	{
		const uint16_t numLists = GetNumListsFromClxListOrSheetBuffer(data.get(), size);
		return OwnedClxSpriteListOrSheet { std::move(data), numLists };
	}

	explicit OwnedClxSpriteListOrSheet(std::unique_ptr<uint8_t[]> &&data, uint16_t numLists)
	    : data_(std::move(data))
	    , num_lists_(numLists)
	{
	}

	explicit OwnedClxSpriteListOrSheet(OwnedClxSpriteSheet &&sheet)
	    : data_(std::move(sheet.data_))
	    , num_lists_(sheet.num_lists_)
	{
	}

	explicit OwnedClxSpriteListOrSheet(OwnedClxSpriteList &&list)
	    : data_(std::move(list.data_))
	    , num_lists_(0)
	{
	}

	~OwnedClxSpriteListOrSheet()
	{
		if (data_ != nullptr)
			clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
	}

	OwnedClxSpriteListOrSheet(OwnedClxSpriteListOrSheet &&) noexcept = default;
	OwnedClxSpriteListOrSheet &operator=(OwnedClxSpriteListOrSheet &&other) noexcept
	{
		if (this != &other) {
			// Evict before the old buffer is freed so a reused address cannot alias stale normal planes.
			if (data_ != nullptr)
				clx_normal_map::EvictNormalPlanes(data_.get(), dataSize());
			data_ = std::move(other.data_);
			num_lists_ = other.num_lists_;
		}
		return *this;
	}

	[[nodiscard]] ClxSpriteList list() const &
	{
		assert(num_lists_ == 0);
		return ClxSpriteList { data_.get() };
	}

	[[nodiscard]] OwnedClxSpriteList list() &&
	{
		assert(num_lists_ == 0);
		return OwnedClxSpriteList { std::move(data_) };
	}

	[[nodiscard]] ClxSpriteSheet sheet() const &
	{
		assert(num_lists_ != 0);
		return ClxSpriteSheet { data_.get(), num_lists_ };
	}

	[[nodiscard]] OwnedClxSpriteSheet sheet() &&
	{
		assert(num_lists_ != 0);
		return OwnedClxSpriteSheet { std::move(data_), num_lists_ };
	}

	[[nodiscard]] bool isSheet() const
	{
		return num_lists_ != 0;
	}

	[[nodiscard]] uint16_t numLists() const { return num_lists_; }

	[[nodiscard]] size_t dataSize() const
	{
		return ClxSpriteListOrSheet { *this }.dataSize();
	}

private:
	// For OptionalOwnedClxSpriteListOrSheet.
	OwnedClxSpriteListOrSheet() = default;

	std::unique_ptr<uint8_t[]> data_;
	uint16_t num_lists_ = 0;

	friend class ClxSpriteListOrSheet;
	friend class OptionalOwnedClxSpriteListOrSheet;
};

inline ClxSpriteListOrSheet::ClxSpriteListOrSheet(const OwnedClxSpriteListOrSheet &listOrSheet)
    : data_(listOrSheet.data_.get())
    , num_lists_(listOrSheet.num_lists_)
{
}

/**
 * @brief Equivalent to `std::optional<ClxSprite>` but smaller.
 */
class OptionalClxSprite {
	DEFINE_CONSTEXPR_INTRUSIVE_OPTIONAL(OptionalClxSprite, ClxSprite, data_, nullptr)
};

/**
 * @brief Equivalent to `std::optional<ClxSpriteList>` but smaller.
 */
class OptionalClxSpriteList {
	DEFINE_CONSTEXPR_INTRUSIVE_OPTIONAL(OptionalClxSpriteList, ClxSpriteList, data_, nullptr)
};

/**
 * @brief Equivalent to `std::optional<ClxSpriteSheet>` but smaller.
 */
class OptionalClxSpriteSheet {
	DEFINE_CONSTEXPR_INTRUSIVE_OPTIONAL(OptionalClxSpriteSheet, ClxSpriteSheet, data_, nullptr)
};

/**
 * @brief Equivalent to `std::optional<ClxSpriteListOrSheet>` but smaller.
 */
class OptionalClxSpriteListOrSheet {
public:
	DEFINE_INTRUSIVE_OPTIONAL(OptionalClxSpriteListOrSheet, ClxSpriteListOrSheet, data_, nullptr);
};

/**
 * @brief Equivalent to `std::optional<OwnedClxSpriteList>` but smaller.
 */
class OptionalOwnedClxSpriteList {
public:
	DEFINE_INTRUSIVE_OPTIONAL(OptionalOwnedClxSpriteList, OwnedClxSpriteList, data_, nullptr)
};

/**
 * @brief Equivalent to `std::optional<OwnedClxSpriteSheet>` but smaller.
 */
class OptionalOwnedClxSpriteSheet {
public:
	DEFINE_INTRUSIVE_OPTIONAL(OptionalOwnedClxSpriteSheet, OwnedClxSpriteSheet, data_, nullptr)
};

class OptionalOwnedClxSpriteListOrSheet {
public:
	DEFINE_INTRUSIVE_OPTIONAL(OptionalOwnedClxSpriteListOrSheet, OwnedClxSpriteListOrSheet, data_, nullptr);
};

} // namespace devilution
