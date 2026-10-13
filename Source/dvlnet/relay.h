#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace devilution::net::relay {

constexpr size_t HeaderSize = 16;
constexpr size_t MaxPayload = 65536;
enum class Operation : uint8_t { Join = 1, Leave, Data, Joined, Departed };

// A borrowed view. Decode never allocates; ownership stays with the caller.
struct Envelope {
	Operation operation;
	uint32_t sender;
	uint32_t destination;
	std::span<const uint8_t> payload;
};

std::optional<Envelope> Decode(std::span<const uint8_t> bytes);
std::vector<uint8_t> Encode(const Envelope &envelope);

} // namespace devilution::net::relay
