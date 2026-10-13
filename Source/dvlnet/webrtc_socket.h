#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dvlnet/relay_socket.h"

namespace devilution::net {

// Uses the lobby room WebSocket for signaling and WebRTC data channels for game packets.
class WebRtcSocket {
public:
	bool Open(const std::string &url, std::string &error);
	bool SendBinary(const std::vector<uint8_t> &data);
	void Poll(std::vector<RelaySocket::Frame> &frames);
	bool IsOpen() const;
	void Close();
	~WebRtcSocket() { Close(); }

private:
#ifdef DAMNATION_NATIVE_WEBRTC
	struct Native;
	std::shared_ptr<Native> native_;
#endif
	int handle_ = 0;
};

} // namespace devilution::net
