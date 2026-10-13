#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::net::session {

inline constexpr uint32_t WireMagic = 0x44564C4E; // "DVLN"
inline constexpr uint16_t WireVersion = 1;
inline constexpr size_t MaxWireMessage = 64 * 1024;
inline constexpr size_t MaxRoomName = 64;
inline constexpr size_t MaxIdentity = 128;

enum class Capability : uint32_t {
	None = 0,
	WebSocket = 1u << 0,
	LegacyTcp = 1u << 1,
	ZeroTier = 1u << 2,
	StateHash = 1u << 3,
};

constexpr Capability operator|(Capability lhs, Capability rhs)
{
	return static_cast<Capability>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}
constexpr uint32_t operator&(Capability lhs, Capability rhs)
{
	return static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs);
}

enum class MessageType : uint8_t {
	Hello = 1,
	HelloReject = 2,
	RoomJoin = 3,
	RoomAccept = 4,
	RoomReject = 5,
	Ready = 6,
	StateHash = 7,
	Heartbeat = 8,
	Disconnect = 9,
};

enum class RejectReason : uint8_t {
	None = 0,
	Malformed,
	UnsupportedVersion,
	IncompatibleCapabilities,
	RoomFull,
	RoomNotFound,
	IdentityMismatch,
	ModMismatch,
	ContentMismatch,
	SequenceViolation,
	QueueOverflow,
	Timeout,
	ProtocolError,
};

struct Identity {
	std::string gameVersion;
	std::string contentId;
	std::string modId;
	uint32_t modAbi = 0;
};

struct Hello {
	uint16_t version = WireVersion;
	Capability capabilities = Capability::None;
	Identity identity;
};

struct RoomJoin {
	std::string room;
	uint32_t peerId = 0;
};

struct StateHash {
	uint32_t tick = 0;
	uint64_t hash = 0;
};

struct Message {
	MessageType type = MessageType::Heartbeat;
	uint32_t sequence = 0;
	RejectReason rejection = RejectReason::None;
	Hello hello;
	RoomJoin room;
	StateHash state;
};

struct NegotiatedSession {
	uint16_t version = 0;
	Capability capabilities = Capability::None;
	Identity identity;
};

std::expected<std::vector<uint8_t>, RejectReason> Encode(const Message &message);
std::expected<Message, RejectReason> Decode(std::span<const uint8_t> bytes);
std::expected<NegotiatedSession, RejectReason> Negotiate(const Hello &local, const Hello &remote);
std::expected<void, RejectReason> ValidateSequence(uint32_t last, uint32_t next);
std::string_view RejectReasonName(RejectReason reason);

} // namespace devilution::net::session

constexpr devilution::net::session::Capability operator|(
    devilution::net::session::Capability lhs,
    devilution::net::session::Capability rhs)
{
	return devilution::net::session::operator|(lhs, rhs);
}
