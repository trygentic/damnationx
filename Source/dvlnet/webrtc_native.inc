// Native browser-compatible WebRTC; only the game thread touches
// peers/signaling.
#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <rtc/rtc.hpp>
#include <set>
#include <sstream>

namespace devilution::net
{
struct WebRtcSocket::Native : std::enable_shared_from_this<Native> {
	using Json = boost::property_tree::ptree;
	struct Peer {
		std::shared_ptr<rtc::PeerConnection> pc;
		std::shared_ptr<rtc::DataChannel> channel;
		bool remote = false;
		std::vector<rtc::Candidate> candidates;
	};
	RelaySocket signaling;
	std::map<uint32_t, Peer> peers;
	std::mutex mutex;
	std::deque<std::vector<uint8_t>> incoming, outgoing;
	std::deque<std::pair<uint32_t, std::shared_ptr<rtc::DataChannel>>> channels;
	std::set<uint32_t> ready;
	std::atomic<bool> alive{true};
	std::atomic<uint32_t> self{0};
	static uint32_t Read(const std::vector<uint8_t> &b, size_t p)
	{
		return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 |
			   uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24;
	}
	static std::vector<uint8_t>
	Envelope(uint8_t op, uint32_t sender, uint32_t dest,
			 const std::vector<uint8_t> &payload = {})
	{
		std::vector<uint8_t> b(16 + payload.size());
		for (size_t i = 0; i < 4; ++i) {
			b[i] = b.size() >> (i * 8);
			b[8 + i] = sender >> (i * 8);
			b[12 + i] = dest >> (i * 8);
		}
		b[4] = 1;
		b[6] = op;
		std::copy(payload.begin(), payload.end(), b.begin() + 16);
		return b;
	}
	void Queue(std::vector<uint8_t> bytes, bool signal = false)
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto &q = signal ? outgoing : incoming;
		if (!alive)
			return;
		if (q.size() >= 256) {
			alive = false;
			return;
		} // Fail closed; never drop reliable game packets silently.
		q.push_back(std::move(bytes));
	}
	void Signal(uint32_t id, const Json &j)
	{
		std::ostringstream out;
		boost::property_tree::write_json(out, j, false);
		const auto s = out.str();
		if (s.size() > 8176) {
			alive = false;
			return;
		}
		Queue(Envelope(3, 0, id, std::vector<uint8_t>(s.begin(), s.end())),
			  true);
	}
	bool Open(const std::string &url, std::string &error)
	{
		return signaling.Open(url, error) &&
			   signaling.SendBinary(Envelope(1, 0, 0));
	}
	void Ready(uint32_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (alive && ready.insert(id).second) {
			if (incoming.size() >= 256) {
				alive = false;
				return;
			}
			incoming.push_back(Envelope(4, id, self));
		}
	}
	void Bind(uint32_t id, const std::shared_ptr<rtc::DataChannel> &dc)
	{
		if (!alive || dc->label() != "game") {
			dc->close();
			return;
		}
		auto weak = weak_from_this();
		dc->onOpen([weak, id] {
			if (auto n = weak.lock())
				n->Ready(id);
		});
		dc->onClosed([weak, id] {
			if (auto n = weak.lock())
				n->Queue(Envelope(5, id, n->self));
		});
		dc->onMessage([weak, id](rtc::message_variant message) {
			if (auto n = weak.lock())
				if (auto b = std::get_if<rtc::binary>(&message)) {
					if (b->size() > 65536) {
						n->alive = false;
						return;
					}
					std::vector<uint8_t> data(b->size());
					std::memcpy(data.data(), b->data(), b->size());
					n->Queue(Envelope(3, id, n->self, data));
				}
		});
		std::lock_guard<std::mutex> lock(mutex);
		if (channels.size() >= 16) {
			alive = false;
			return;
		}
		channels.emplace_back(id, dc);
		if (dc->isOpen() && ready.insert(id).second) {
			if (incoming.size() >= 256)
				alive = false;
			else
				incoming.push_back(Envelope(4, id, self));
		}
	}
	Peer &Create(uint32_t id, bool offer)
	{
		if (auto it = peers.find(id); it != peers.end())
			return it->second;
		if (peers.size() >= 3)
			throw std::runtime_error("WebRTC peer limit");
		rtc::Configuration config;
		config.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
		auto pc = std::make_shared<rtc::PeerConnection>(config);
		auto &p = peers[id];
		p.pc = pc;
		auto weak = weak_from_this();
		pc->onLocalDescription([weak, id](rtc::Description desc) {
			if (auto n = weak.lock()) {
				Json j;
				j.put("type", desc.typeString());
				j.put("sdp", std::string(desc));
				n->Signal(id, j);
			}
		});
		pc->onLocalCandidate([weak, id](rtc::Candidate c) {
			if (auto n = weak.lock()) {
				Json j;
				j.put("type", "candidate");
				j.put("candidate.candidate", std::string(c));
				j.put("candidate.sdpMid", c.mid());
				n->Signal(id, j);
			}
		});
		pc->onDataChannel([weak, id](std::shared_ptr<rtc::DataChannel> dc) {
			if (auto n = weak.lock())
				n->Bind(id, dc);
		});
		pc->onStateChange([weak, id](rtc::PeerConnection::State s) {
			if (s == rtc::PeerConnection::State::Failed)
				if (auto n = weak.lock())
					n->Queue(Envelope(5, id, n->self));
		});
		if (offer)
			Bind(id, pc->createDataChannel("game"));
		return p;
	}
	void Poll(std::vector<RelaySocket::Frame> &frames)
	{
		if (!alive) {
			signaling.Close();
			return;
		}
		std::vector<RelaySocket::Frame> wire;
		signaling.Poll(wire);
		for (auto &f : wire) {
			auto &b = f.data;
			if (f.text || b.size() < 16 || Read(b, 0) != b.size() || b[4] != 1)
				continue;
			const auto id = Read(b, 8);
			try {
				if (b[6] == 4) {
					if (!self) {
						self = id;
						frames.push_back(std::move(f));
					} else if (id != self)
						Create(id, self > id);
				} else if (b[6] == 5) {
					auto it = peers.find(id);
					if (it != peers.end()) {
						it->second.pc->close();
						peers.erase(it);
						Queue(Envelope(5, id, self));
					}
				} else if (b[6] == 3 && id && id != self && b.size() <= 8192) {
					Json j;
					std::istringstream in(std::string(b.begin() + 16, b.end()));
					boost::property_tree::read_json(in, j);
					auto &p = Create(id, false);
					const auto type = j.get<std::string>("type");
					if (type == "offer" || type == "answer") {
						p.pc->setRemoteDescription(
							rtc::Description(j.get<std::string>("sdp"), type));
						p.remote = true;
						for (const auto &c : p.candidates)
							p.pc->addRemoteCandidate(c);
						p.candidates.clear();
					} else if (type == "candidate") {
						rtc::Candidate c(
							j.get<std::string>("candidate.candidate"),
							j.get<std::string>("candidate.sdpMid", "0"));
						if (p.remote)
							p.pc->addRemoteCandidate(c);
						else if (p.candidates.size() < 64)
							p.candidates.push_back(c);
						else
							alive = false;
					}
				}
			} catch (const std::exception &) {
				alive = false;
			}
		}
		std::deque<std::vector<uint8_t>> sends;
		{
			std::lock_guard<std::mutex> lock(mutex);
			sends.swap(outgoing);
			while (!channels.empty()) {
				auto [id, dc] = std::move(channels.front());
				channels.pop_front();
				if (auto it = peers.find(id); it != peers.end())
					it->second.channel = std::move(dc);
			}
			while (!incoming.empty()) {
				frames.push_back({false, std::move(incoming.front())});
				incoming.pop_front();
			}
		}
		for (auto &frame : frames) {
			if (frame.data.size() >= 16 && frame.data[6] == 5) {
				const auto id = Read(frame.data, 8);
				if (auto it = peers.find(id); it != peers.end()) {
					it->second.pc->close();
					peers.erase(it);
				}
				std::lock_guard<std::mutex> lock(mutex);
				ready.erase(id);
			}
		}
		for (const auto &b : sends)
			if (!signaling.SendBinary(b))
				alive = false;
	}
	bool Send(const std::vector<uint8_t> &b)
	{
		if (!alive || b.size() < 16 || b.size() > 65552)
			return false;
		if (b[6] == 1)
			return true;
		if (b[6] != 3)
			return false;
		const auto dest = Read(b, 12);
		bool sent = dest == 0;
		for (auto &[id, p] : peers)
			if (!dest || id == dest) {
				if (!p.channel || !p.channel->isOpen() ||
					p.channel->bufferedAmount() + b.size() - 16 > 262144)
					return false;
				try {
					p.channel->send(
						reinterpret_cast<const std::byte *>(b.data() + 16),
						b.size() - 16);
					sent = true;
				} catch (const std::exception &) {
					return false;
				}
			}
		return sent;
	}
	void Close()
	{
		alive = false;
		signaling.Close();
		for (auto &[id, p] : peers)
			p.pc->close();
		peers.clear();
		std::lock_guard<std::mutex> lock(mutex);
		incoming.clear();
		outgoing.clear();
		channels.clear();
		ready.clear();
	}
};
} // namespace devilution::net
