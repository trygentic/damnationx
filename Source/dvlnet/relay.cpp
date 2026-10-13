#include "dvlnet/relay.h"
#include <algorithm>

namespace devilution::net::relay {
namespace {
uint32_t Read32(std::span<const uint8_t> bytes, size_t offset)
{
	uint32_t value = 0;
	for (size_t i = 0; i < 4; ++i)
		value |= uint32_t { bytes[offset + i] } << (8 * i);
	return value;
}
void Write32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i)
		bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
} // namespace

std::optional<Envelope> Decode(std::span<const uint8_t> bytes)
{
	if (bytes.size() < HeaderSize || bytes.size() - HeaderSize > MaxPayload)
		return std::nullopt;
	if (Read32(bytes, 0) != bytes.size() || bytes[4] != 1 || bytes[5] != 0 || bytes[7] != 0 || bytes[6] < 1 || bytes[6] > 5)
		return std::nullopt;
	const auto op = static_cast<Operation>(bytes[6]);
	if ((op == Operation::Data) != (bytes.size() > HeaderSize))
		return std::nullopt;
	return Envelope { op, Read32(bytes, 8), Read32(bytes, 12), bytes.subspan(HeaderSize) };
}

std::vector<uint8_t> Encode(const Envelope &envelope)
{
	if (envelope.payload.size() > MaxPayload)
		return {};
	std::vector<uint8_t> bytes(HeaderSize + envelope.payload.size());
	Write32(bytes, 0, static_cast<uint32_t>(bytes.size()));
	bytes[4] = 1;
	bytes[6] = static_cast<uint8_t>(envelope.operation);
	Write32(bytes, 8, envelope.sender);
	Write32(bytes, 12, envelope.destination);
	std::copy(envelope.payload.begin(), envelope.payload.end(), bytes.begin() + HeaderSize);
	if (!Decode(bytes))
		return {};
	return bytes;
}
} // namespace devilution::net::relay
