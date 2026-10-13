#include "dvlnet/session.h"

#include <climits>

namespace devilution::net::session {
namespace {
struct Writer {
	std::vector<uint8_t> data;
	void u8(uint8_t v) { data.push_back(v); }
	void u16(uint16_t v) { data.push_back(v >> 8); data.push_back(v); }
	void u32(uint32_t v) { for (int i = 3; i >= 0; --i) data.push_back(static_cast<uint8_t>(v >> (i * 8))); }
	void u64(uint64_t v) { for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(v >> (i * 8))); }
	bool text(std::string_view s, size_t max) { if (s.size() > max || s.size() > UINT16_MAX) return false; u16(static_cast<uint16_t>(s.size())); data.insert(data.end(), s.begin(), s.end()); return true; }
};
struct Reader {
	std::span<const uint8_t> data; size_t pos = 0;
	bool take(size_t n, const uint8_t *&p) { if (n > data.size() - pos) return false; p = data.data() + pos; pos += n; return true; }
	bool u8(uint8_t &v) { const uint8_t *p; if (!take(1, p)) return false; v = *p; return true; }
	bool u16(uint16_t &v) { const uint8_t *p; if (!take(2, p)) return false; v = (p[0] << 8) | p[1]; return true; }
	bool u32(uint32_t &v) { const uint8_t *p; if (!take(4, p)) return false; v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; return true; }
	bool u64(uint64_t &v) { const uint8_t *p; if (!take(8, p)) return false; v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return true; }
	bool text(std::string &s, size_t max) { uint16_t n; const uint8_t *p; if (!u16(n) || n > max || !take(n, p)) return false; s.assign(reinterpret_cast<const char *>(p), n); return true; }
};
void WriteIdentity(Writer &w, const Identity &i) { w.text(i.gameVersion, MaxIdentity); w.text(i.contentId, MaxIdentity); w.text(i.modId, MaxIdentity); w.u32(i.modAbi); }
bool ReadIdentity(Reader &r, Identity &i) { return r.text(i.gameVersion, MaxIdentity) && r.text(i.contentId, MaxIdentity) && r.text(i.modId, MaxIdentity) && r.u32(i.modAbi); }
}
std::expected<std::vector<uint8_t>, RejectReason> Encode(const Message &m) { Writer w; w.u32(WireMagic); w.u16(WireVersion); w.u8(static_cast<uint8_t>(m.type)); w.u32(m.sequence); w.u8(static_cast<uint8_t>(m.rejection)); switch (m.type) { case MessageType::Hello: w.u16(m.hello.version); w.u32(static_cast<uint32_t>(m.hello.capabilities)); WriteIdentity(w, m.hello.identity); break; case MessageType::RoomJoin: if (!w.text(m.room.room, MaxRoomName)) return std::unexpected(RejectReason::Malformed); w.u32(m.room.peerId); break; case MessageType::StateHash: w.u32(m.state.tick); w.u64(m.state.hash); break; default: break; } if (w.data.size() > MaxWireMessage) return std::unexpected(RejectReason::Malformed); return w.data; }
std::expected<Message, RejectReason> Decode(std::span<const uint8_t> bytes) { if (bytes.size() > MaxWireMessage) return std::unexpected(RejectReason::Malformed); Reader r{bytes}; uint32_t magic; uint16_t wire; uint8_t type, reject; Message m; if (!r.u32(magic) || magic != WireMagic || !r.u16(wire) || wire != WireVersion || !r.u8(type) || !r.u32(m.sequence) || !r.u8(reject) || type < 1 || type > 9) return std::unexpected(RejectReason::Malformed); m.type = static_cast<MessageType>(type); m.rejection = static_cast<RejectReason>(reject); switch (m.type) { case MessageType::Hello: { uint32_t caps; if (!r.u16(m.hello.version) || !r.u32(caps) || !ReadIdentity(r, m.hello.identity)) return std::unexpected(RejectReason::Malformed); m.hello.capabilities = static_cast<Capability>(caps); break; } case MessageType::RoomJoin: if (!r.text(m.room.room, MaxRoomName) || !r.u32(m.room.peerId)) return std::unexpected(RejectReason::Malformed); break; case MessageType::StateHash: if (!r.u32(m.state.tick) || !r.u64(m.state.hash)) return std::unexpected(RejectReason::Malformed); break; default: break; } if (r.pos != bytes.size()) return std::unexpected(RejectReason::Malformed); return m; }
std::expected<NegotiatedSession, RejectReason> Negotiate(const Hello &a, const Hello &b) { if (a.version != b.version || a.version != WireVersion) return std::unexpected(RejectReason::UnsupportedVersion); if (a.identity.gameVersion != b.identity.gameVersion || a.identity.contentId != b.identity.contentId) return std::unexpected(RejectReason::ContentMismatch); if (a.identity.modId != b.identity.modId || a.identity.modAbi != b.identity.modAbi) return std::unexpected(RejectReason::ModMismatch); auto caps = static_cast<Capability>(static_cast<uint32_t>(a.capabilities) & static_cast<uint32_t>(b.capabilities)); if (!(caps & Capability::StateHash)) return std::unexpected(RejectReason::IncompatibleCapabilities); return NegotiatedSession{a.version, caps, a.identity}; }
std::expected<void, RejectReason> ValidateSequence(uint32_t last, uint32_t next) { if (next != last + 1 && !(last == UINT32_MAX && next == 0)) return std::unexpected(RejectReason::SequenceViolation); return {}; }
std::string_view RejectReasonName(RejectReason r) { switch (r) { case RejectReason::None: return "none"; case RejectReason::Malformed: return "malformed"; case RejectReason::UnsupportedVersion: return "unsupported-version"; case RejectReason::IncompatibleCapabilities: return "incompatible-capabilities"; case RejectReason::ModMismatch: return "mod-mismatch"; case RejectReason::ContentMismatch: return "content-mismatch"; case RejectReason::SequenceViolation: return "sequence-violation"; case RejectReason::QueueOverflow: return "queue-overflow"; case RejectReason::Timeout: return "timeout"; default: return "protocol-error"; } }
}
