#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace devilution::net::lobby {

enum class Transport { WebRtc, Relay };

void SetHostTransport(Transport transport);
Transport HostTransport();

struct Game {
	std::string id;
	std::string name;
	std::string host;
	int players = 1;
	Transport transport = Transport::Relay;
};

struct Created {
	std::string id;
	std::string hostToken;
	std::string roomToken;
};

std::string Origin();
std::string WebSocketUrl(const std::string &path);
std::string DeepLinkGameId();
std::string TakeDeepLinkGameId();
// Accepts damnationx://join/<gameId> (desktop URL scheme); returns false for anything else.
bool SetDeepLinkUrl(std::string_view url);
// macOS: feed damnationx:// URLs opened by the system into SetDeepLinkUrl. Call before and after SDL_Init.
void ListenForDeepLinkUrls();
void SetHostCharacter(std::string name);
std::string HostCharacter();
bool Create(const std::string &name, const std::string &host, bool isPublic, Created &created, std::string &error);
bool Join(const std::string &id, std::string &roomToken, Transport &transport, std::string &error);
bool List(std::vector<Game> &games, std::string &error);

} // namespace devilution::net::lobby
