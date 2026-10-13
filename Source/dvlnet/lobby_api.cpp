#include "dvlnet/lobby_api.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include "config.h"
#include "utils/log.hpp"

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define DEVILUTIONX_MAC_URL_HANDLER
#include "platform/macos/url_handler.h"
#endif
#endif

namespace devilution::net::lobby {
namespace {

std::string HostCharacterName;
bool DeepLinkTaken = false;
std::string NativeDeepLink;

bool IsGameId(std::string_view value)
{
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch); });
}
#ifdef __EMSCRIPTEN__
Transport HostTransportChoice = Transport::WebRtc;
#else
Transport HostTransportChoice = Transport::Relay;
#endif

const char *TransportName(Transport transport) { return transport == Transport::WebRtc ? "webrtc" : "relay"; }
Transport ParseTransport(std::string_view name) { return name == "webrtc" ? Transport::WebRtc : Transport::Relay; }

struct Reply {
	int status = 0;
	std::string body;
};

#ifdef __EMSCRIPTEN__
// The page's own origin when it is served locally (the lobby serves the game in development),
// otherwise the deployed lobby: the public page and the lobby are on different hosts.
EM_JS(char *, BrowserOrigin, (const char *defaultOrigin), {
	const host = window.location.hostname;
	const local = host === 'localhost' || host === '127.0.0.1' || host === '[::1]' || host.endsWith('.localhost');
	const fallback = UTF8ToString(defaultOrigin);
	return stringToNewUTF8(local || !fallback ? window.location.origin : fallback);
});

EM_JS(char *, BrowserDeepLink, (), {
	return stringToNewUTF8(new URLSearchParams(window.location.search).get('join') || "");
});

EM_ASYNC_JS(char *, BrowserRequest, (const char *method, const char *path, const char *body, int *status), {
	try {
		const options = { method: UTF8ToString(method), headers: { 'Content-Type': 'application/json' } };
		if (options.method !== 'GET') options.body = UTF8ToString(body);
		const response = await fetch(UTF8ToString(path), options);
		HEAP32[status >> 2] = response.status;
		return stringToNewUTF8(await response.text());
	} catch (error) {
		HEAP32[status >> 2] = 0;
		return stringToNewUTF8("");
	}
});
#else
struct Address {
	std::string host;
	std::string port;
	bool tls;
};

bool ParseOrigin(const std::string &origin, Address &address)
{
	const auto schemeEnd = origin.find("://");
	if (schemeEnd == std::string::npos || origin.find('/', schemeEnd + 3) != std::string::npos)
		return false;
	const std::string scheme = origin.substr(0, schemeEnd);
	if (scheme != "http" && scheme != "https")
		return false;
	address.tls = scheme == "https";
	const std::string authority = origin.substr(schemeEnd + 3);
	const auto colon = authority.rfind(':');
	address.host = colon == std::string::npos ? authority : authority.substr(0, colon);
	address.port = colon == std::string::npos ? (address.tls ? "443" : "80") : authority.substr(colon + 1);
	return !address.host.empty() && !address.port.empty();
}
#endif

Reply Request(const char *method, const std::string &path, const std::string &body)
{
#ifdef __EMSCRIPTEN__
	Reply result;
	const std::string url = Origin() + path;
	char *text = BrowserRequest(method, url.c_str(), body.c_str(), &result.status);
	if (text != nullptr) {
		result.body = text;
		free(text);
	}
	return result;
#else
	Reply result;
	Address address;
	if (!ParseOrigin(Origin(), address))
		return result;
	try {
		namespace asio = boost::asio;
		namespace beast = boost::beast;
		namespace http = beast::http;
		asio::io_context io;
		asio::ip::tcp::resolver resolver(io);
		const auto endpoints = resolver.resolve(address.host, address.port);
		http::request<http::string_body> request { method[0] == 'G' ? http::verb::get : http::verb::post, path, 11 };
		request.set(http::field::host, address.host);
		request.set(http::field::user_agent, "DamnationX Lobby");
		request.set(http::field::content_type, "application/json");
		request.body() = body;
		request.prepare_payload();
		beast::flat_buffer buffer;
		http::response<http::string_body> response;
		if (address.tls) {
			asio::ssl::context context(asio::ssl::context::tls_client);
			context.set_default_verify_paths();
			if (const char *ca = std::getenv("DAMNATIONX_LOBBY_CA")) context.load_verify_file(ca);
			beast::ssl_stream<beast::tcp_stream> stream(io, context);
			stream.set_verify_mode(asio::ssl::verify_peer);
			stream.set_verify_callback(asio::ssl::host_name_verification(address.host));
			SSL_set_tlsext_host_name(stream.native_handle(), address.host.c_str());
			beast::get_lowest_layer(stream).connect(endpoints);
			stream.handshake(asio::ssl::stream_base::client);
			http::write(stream, request);
			http::read(stream, buffer, response);
		} else {
			beast::tcp_stream stream(io);
			stream.connect(endpoints);
			http::write(stream, request);
			http::read(stream, buffer, response);
		}
		result.status = response.result_int();
		result.body = std::move(response.body());
	} catch (...) {
		result.status = 0;
	}
	return result;
#endif
}

std::string JsonString(std::string_view value)
{
	std::string result = "\"";
	for (char ch : value) {
		if (ch == '"' || ch == '\\') result += '\\';
		if (static_cast<unsigned char>(ch) < 32) continue;
		result += ch;
	}
	return result + '"';
}

std::string Field(std::string_view json, std::string_view key)
{
	const std::string needle = JsonString(key);
	auto pos = json.find(needle);
	if (pos == std::string_view::npos) return {};
	pos = json.find(':', pos + needle.size());
	if (pos == std::string_view::npos) return {};
	while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) { }
	if (pos >= json.size()) return {};
	if (json[pos] != '"') {
		const auto end = json.find_first_of(",}]", pos);
		return std::string(json.substr(pos, end - pos));
	}
	std::string result;
	for (++pos; pos < json.size(); ++pos) {
		const char ch = json[pos];
		if (ch == '"') break;
		if (ch == '\\' && ++pos < json.size()) {
			const char escaped = json[pos];
			if (escaped == 'n') result += '\n';
			else if (escaped == 't') result += '\t';
			else result += escaped;
		} else result += ch;
	}
	return result;
}

std::string Error(const Reply &reply)
{
	if (reply.status == 0) return "Could not reach the lobby service.";
	std::string message = Field(reply.body, "error");
	if (message.empty()) message = "Lobby request failed.";
	const std::string expiry = Field(reply.body, "expiresAt");
	if (reply.status == 403 && !expiry.empty() && expiry != "null") message += " Ban expires at Unix time " + expiry + '.';
	return message;
}

} // namespace

std::string Origin()
{
#ifdef __EMSCRIPTEN__
	char *origin = BrowserOrigin(DEVILUTIONX_DEFAULT_LOBBY_ORIGIN);
	if (origin == nullptr) return {};
	std::string result(origin);
	free(origin);
	return result;
#else
	const char *origin = std::getenv("DAMNATIONX_LOBBY_ORIGIN");
	return origin != nullptr ? origin : DEVILUTIONX_DEFAULT_LOBBY_ORIGIN;
#endif
}

std::string WebSocketUrl(const std::string &path)
{
	std::string origin = Origin();
	if (origin.starts_with("https://")) return "wss://" + origin.substr(8) + path;
	if (origin.starts_with("http://")) return "ws://" + origin.substr(7) + path;
	return {};
}

std::string DeepLinkGameId()
{
	if (DeepLinkTaken) return {};
#ifdef __EMSCRIPTEN__
	static const std::string id = [] {
		char *raw = BrowserDeepLink();
		if (raw == nullptr) return std::string {};
		std::string value(raw);
		free(raw);
		if (!IsGameId(value)) return std::string {};
		return value;
	}();
	return id;
#else
	return NativeDeepLink;
#endif
}

bool SetDeepLinkUrl(std::string_view url)
{
	constexpr std::string_view Prefix = "damnationx://join/";
	if (url.size() < Prefix.size() || !std::equal(Prefix.begin(), Prefix.end(), url.begin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
		return false;
	url.remove_prefix(Prefix.size());
	if (url.ends_with('/')) url.remove_suffix(1);
	if (!IsGameId(url)) return false;
#ifndef __EMSCRIPTEN__
	NativeDeepLink = std::string(url);
	DeepLinkTaken = false;
#endif
	return true;
}

void ListenForDeepLinkUrls()
{
#ifdef DEVILUTIONX_MAC_URL_HANDLER
	MacInstallUrlHandler([](const char *url) {
		if (SetDeepLinkUrl(url)) Log("Opening lobby game from {}", url);
		else LogWarn("Ignoring URL {}", url);
	});
#endif
}

std::string TakeDeepLinkGameId()
{
	const std::string id = DeepLinkGameId();
	DeepLinkTaken = true;
#ifndef __EMSCRIPTEN__
	NativeDeepLink.clear();
#endif
	return id;
}

void SetHostCharacter(std::string name) { HostCharacterName = std::move(name); }
std::string HostCharacter() { return HostCharacterName; }
void SetHostTransport(Transport transport) { HostTransportChoice = transport; }
Transport HostTransport() { return HostTransportChoice; }

bool Create(const std::string &name, const std::string &host, bool isPublic, Created &created, std::string &error)
{
	const std::string body = "{\"public\":" + std::string(isPublic ? "true" : "false") +
	    ",\"lobbyName\":" + JsonString(name) + ",\"hostCharacter\":" + JsonString(host) +
	    ",\"version\":" + JsonString(PROJECT_VERSION) + ",\"modIdentity\":\"vanilla\",\"transport\":" + JsonString(TransportName(HostTransportChoice)) + "}";
	const Reply reply = Request("POST", "/api/v1/games", body);
	if (reply.status != 201) { error = Error(reply); return false; }
	created = { Field(reply.body, "gameId"), Field(reply.body, "hostToken"), Field(reply.body, "roomToken") };
	if (created.id.empty() || created.hostToken.empty() || created.roomToken.empty()) { error = "Invalid lobby response."; return false; }
	return true;
}

bool Join(const std::string &id, std::string &roomToken, Transport &transport, std::string &error)
{
	const Reply reply = Request("POST", "/api/v1/games/" + id + "/join", "{}");
	if (reply.status != 200) { error = Error(reply); return false; }
	roomToken = Field(reply.body, "roomToken");
	transport = ParseTransport(Field(reply.body, "transport"));
	if (roomToken.empty()) { error = "Invalid lobby response."; return false; }
	return true;
}

bool List(std::vector<Game> &games, std::string &error)
{
	const Reply reply = Request("GET", "/api/v1/games", "");
	if (reply.status != 200) { error = Error(reply); return false; }
	games.clear();
	for (size_t pos = 0; (pos = reply.body.find('{', pos)) != std::string::npos; ) {
		const auto end = reply.body.find('}', pos);
		if (end == std::string::npos) break;
		const std::string_view object(reply.body.data() + pos, end - pos + 1);
		Game game { Field(object, "gameId"), Field(object, "lobbyName"), Field(object, "hostCharacter"), 1 };
		game.transport = ParseTransport(Field(object, "transport"));
		const std::string count = Field(object, "playerCount");
		if (!count.empty()) game.players = std::atoi(count.c_str());
		if (!game.id.empty()) games.push_back(std::move(game));
		pos = end + 1;
	}
	return true;
}

} // namespace devilution::net::lobby
