#include "dvlnet/protocol_relay.h"

#include <algorithm>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifdef USE_SDL3
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "config.h"
#include "multi.h"
#include "player.h"
#include "storm/storm_net.hpp"

namespace devilution::net {
namespace {

uint32_t Read32(const uint8_t *data)
{
	return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

void Write32(uint8_t *data, uint32_t value)
{
	for (int i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (8 * i));
}

buffer_t Envelope(uint8_t operation, uint32_t destination, const buffer_t &payload)
{
	buffer_t result(16 + payload.size());
	Write32(result.data(), static_cast<uint32_t>(result.size()));
	result[4] = 1;
	result[6] = operation;
	Write32(result.data() + 12, destination);
	std::copy(payload.begin(), payload.end(), result.begin() + 16);
	return result;
}

void WaitStep()
{
#ifdef __EMSCRIPTEN__
	emscripten_sleep(1);
#else
	SDL_Delay(1);
#endif
}

} // namespace

buffer_t protocol_relay::endpoint::serialize() const
{
	buffer_t result(4);
	Write32(result.data(), id);
	return result;
}

std::expected<void, PacketError> protocol_relay::endpoint::unserialize(const buffer_t &data)
{
	if (data.size() != 4 || Read32(data.data()) == 0) return std::unexpected("Invalid relay endpoint");
	id = Read32(data.data());
	return {};
}

void protocol_relay::Configure(std::string room, std::string token, lobby::Transport transport)
{
	room_ = std::move(room);
	token_ = std::move(token);
	transport_ = transport;
	self_ = 0;
	peers_.clear();
}

std::expected<bool, PacketError> protocol_relay::network_online()
{
	if (self_ != 0) return true;
	if (room_.empty() || token_.empty()) return std::unexpected("No lobby room token");
	std::string error;
	const std::string url = lobby::WebSocketUrl("/ws/relay/" + room_ + "?token=" + token_);
	if (!(transport_ == lobby::Transport::WebRtc ? webRtc_.Open(url, error) : socket_.Open(url, error)))
		return std::unexpected(PacketError(std::move(error)));
	if (!(transport_ == lobby::Transport::WebRtc ? webRtc_.SendBinary(Envelope(1, 0, {})) : socket_.SendBinary(Envelope(1, 0, {})))) return std::unexpected("Could not join lobby room");
	for (int i = 0; i < 5000 && self_ == 0 && (transport_ == lobby::Transport::WebRtc ? webRtc_.IsOpen() : socket_.IsOpen()); ++i) {
		Drain();
		WaitStep();
	}
	if (self_ == 0) return std::unexpected("Lobby room join timed out");
	return true;
}

std::expected<bool, PacketError> protocol_relay::peers_ready() { return self_ != 0; }

std::expected<void, PacketError> protocol_relay::send(const endpoint &peer, const buffer_t &data)
{
	if (!peer || data.empty() || data.size() > 65536) return std::unexpected("Invalid relay packet");
	const buffer_t frame = Envelope(3, peer.id, data);
	if (!(transport_ == lobby::Transport::WebRtc ? webRtc_.SendBinary(frame) : socket_.SendBinary(frame))) return std::unexpected("Lobby send failed");
	return {};
}

bool protocol_relay::send_oob(const endpoint &peer, const buffer_t &data) { return send(peer, data).has_value(); }
bool protocol_relay::send_oob_mc(const buffer_t &data)
{
	if (data.empty()) return false;
	const buffer_t frame = Envelope(3, 0, data);
	return transport_ == lobby::Transport::WebRtc ? webRtc_.SendBinary(frame) : socket_.SendBinary(frame);
}

void protocol_relay::Drain()
{
	std::vector<RelaySocket::Frame> frames;
	if (transport_ == lobby::Transport::WebRtc) webRtc_.Poll(frames);
	else socket_.Poll(frames);
	for (const auto &frame : frames) {
		const buffer_t &bytes = frame.data;
		if (frame.text || bytes.size() < 16 || bytes.size() > 65552 || Read32(bytes.data()) != bytes.size() || bytes[4] != 1 || bytes[5] != 0 || bytes[7] != 0) continue;
		const uint8_t op = bytes[6];
		const uint32_t sender = Read32(bytes.data() + 8);
		if (op == 4) {
			if (self_ == 0) self_ = sender;
			else if (sender != self_) peers_.insert(sender);
		} else if (op == 5) {
			peers_.erase(sender);
			departed_.push_back({ sender });
		} else if (op == 3 && sender != 0 && bytes.size() > 16) {
			received_.emplace_back(endpoint { sender }, buffer_t(bytes.begin() + 16, bytes.end()));
		}
	}
}

bool protocol_relay::recv(endpoint &peer, buffer_t &data)
{
	Drain();
	if (received_.empty()) return false;
	peer = received_.front().first;
	data = std::move(received_.front().second);
	received_.pop_front();
	return true;
}

bool protocol_relay::get_disconnected(endpoint &peer)
{
	Drain();
	if (departed_.empty()) return false;
	peer = departed_.front();
	departed_.pop_front();
	return true;
}

void protocol_relay::disconnect(const endpoint &peer) { peers_.erase(peer.id); }
bool protocol_relay::is_peer_connected(endpoint &peer) { return peers_.contains(peer.id); }

void relay_net::SNetGetProviderCaps(struct _SNETCAPS *caps)
{
	base::SNetGetProviderCaps(caps);
	// Four 100 ms turns cover the measured intercontinental round trip without repeated stalls.
	caps->defaultturnsintransit = 4;
}

int relay_net::create(std::string_view name)
{
	lobby::Created created;
	std::string error;
	const std::string host = lobby::HostCharacter();
	if (!lobby::Create(std::string(name), host.empty() ? "Hero" : host, DvlNet_IsPublicGame(), created, error)) {
		SDL_SetError("%s", error.c_str());
		return -1;
	}
	proto.Configure(created.id, created.roomToken, lobby::HostTransport());
	hostControlUrl_ = lobby::WebSocketUrl("/ws/games/" + created.id + "?token=" + created.hostToken);
	if (!hostControl_.Open(hostControlUrl_, error)) {
		SDL_SetError("%s", error.c_str());
		return -1;
	}
	return base_protocol<protocol_relay>::create(created.id);
}

int relay_net::join(std::string_view id)
{
	std::string token;
	lobby::Transport transport;
	std::string error;
	if (!lobby::Join(std::string(id), token, transport, error)) {
		SDL_SetError("%s", error.c_str());
		return -1;
	}
	proto.Configure(std::string(id), token, transport);
	const int player = base_protocol<protocol_relay>::join(id);
	if (player < 0 && transport == lobby::Transport::WebRtc) {
		const std::string reason = SDL_GetError();
		if (reason.find("Timeout waiting") != std::string::npos)
			SDL_SetError("%s Direct WebRTC may have failed. Ask the host to recreate the game with Relay.", reason.c_str());
	}
	return player;
}

std::vector<GameInfo> relay_net::get_gamelist()
{
	std::vector<lobby::Game> games;
	std::string error;
	if (!lobby::List(games, error)) return {};
	std::vector<GameInfo> result;
	for (const lobby::Game &game : games) {
#if !defined(__EMSCRIPTEN__) && !defined(DAMNATION_NATIVE_WEBRTC)
		if (game.transport == lobby::Transport::WebRtc) continue;
#endif
		GameInfo info { game.id, {}, {}, std::nullopt, game.transport == lobby::Transport::Relay, game.name, game.host, game.players };
		result.push_back(std::move(info));
	}
	return result;
}

std::expected<void, PacketError> relay_net::poll()
{
	std::vector<RelaySocket::Frame> ignored;
	hostControl_.Poll(ignored);
	const uint32_t now = SDL_GetTicks();
	if (!hostControlUrl_.empty() && !hostControl_.IsOpen() && now - lastHostReconnect_ >= 5000) {
		lastHostReconnect_ = now;
		std::string error;
		if (hostControl_.Open(hostControlUrl_, error)) lastHeartbeat_ = now - 20001;
	}
	if (hostControl_.IsOpen() && SDL_GetTicks() - lastHeartbeat_ > 20000) {
		int count = 0;
		for (const Player &player : Players) if (player.plractive) ++count;
		if (count == 0) count = 1;
		hostControl_.SendText("{\"type\":\"heartbeat\",\"playerCount\":" + std::to_string(count) + "}");
		lastHeartbeat_ = SDL_GetTicks();
	}
	return base_protocol<protocol_relay>::poll();
}

bool relay_net::SNetLeaveGame(net::leaveinfo_t type)
{
	const bool left = base_protocol<protocol_relay>::SNetLeaveGame(type);
	if (hostControl_.IsOpen()) hostControl_.SendText("{\"type\":\"end\"}");
	hostControl_.Close();
	hostControlUrl_.clear();
	return left;
}

} // namespace devilution::net
