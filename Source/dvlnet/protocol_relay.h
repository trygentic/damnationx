#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <set>
#include <string>

#include "dvlnet/base_protocol.h"
#include "dvlnet/lobby_api.h"
#include "dvlnet/relay_socket.h"
#include "dvlnet/webrtc_socket.h"

namespace devilution::net {

class protocol_relay {
public:
	struct endpoint {
		uint32_t id = 0;
		explicit operator bool() const { return id != 0; }
		bool operator==(const endpoint &) const = default;
		bool operator<(const endpoint &other) const { return id < other.id; }
		buffer_t serialize() const;
		std::expected<void, PacketError> unserialize(const buffer_t &data);
	};

	void Configure(std::string room, std::string token, lobby::Transport transport);
	std::expected<bool, PacketError> network_online();
	std::expected<bool, PacketError> peers_ready();
	std::expected<void, PacketError> send(const endpoint &peer, const buffer_t &data);
	bool send_oob(const endpoint &peer, const buffer_t &data);
	bool send_oob_mc(const buffer_t &data);
	bool recv(endpoint &peer, buffer_t &data);
	bool get_disconnected(endpoint &peer);
	void disconnect(const endpoint &peer);
	bool is_peer_connected(endpoint &peer);
	std::optional<bool> is_peer_relayed(const endpoint &) const { return transport_ == lobby::Transport::Relay; }
	std::optional<int> get_latency_to(const endpoint &) const { return std::nullopt; }
	static std::string make_default_gamename() { return "Lobby Game"; }

private:
	void Drain();
	std::string room_;
	std::string token_;
	RelaySocket socket_;
	WebRtcSocket webRtc_;
	lobby::Transport transport_ = lobby::Transport::Relay;
	uint32_t self_ = 0;
	std::set<uint32_t> peers_;
	std::deque<std::pair<endpoint, buffer_t>> received_;
	std::deque<endpoint> departed_;
};

class relay_net : public base_protocol<protocol_relay> {
public:
	void SNetGetProviderCaps(struct _SNETCAPS *caps) override;
	int create(std::string_view name) override;
	int join(std::string_view id) override;
	std::expected<void, PacketError> poll() override;
	void clear_gamelist() override { }
	std::vector<GameInfo> get_gamelist() override;
	bool SNetLeaveGame(net::leaveinfo_t type) override;

private:
	RelaySocket hostControl_;
	uint32_t lastHeartbeat_ = 0;
	uint32_t lastHostReconnect_ = 0;
	std::string hostControlUrl_;
};

} // namespace devilution::net
