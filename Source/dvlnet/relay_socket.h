#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace devilution::net {

class RelaySocket {
public:
	struct Frame {
		bool text;
		std::vector<uint8_t> data;
	};

	RelaySocket();
	~RelaySocket();
	RelaySocket(const RelaySocket &) = delete;
	RelaySocket &operator=(const RelaySocket &) = delete;
	bool Open(const std::string &url, std::string &error);
	bool SendBinary(const std::vector<uint8_t> &data);
	bool SendText(const std::string &text);
	void Poll(std::vector<Frame> &frames);
	bool IsOpen() const;
	void Close();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace devilution::net
