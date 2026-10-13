#include "dvlnet/relay_socket.h"

#include <deque>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#else
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cstdlib>
#endif

namespace devilution::net {

#ifdef __EMSCRIPTEN__
struct RelaySocket::Impl {
	EMSCRIPTEN_WEBSOCKET_T socket = 0;
	bool open = false;
	bool failed = false;
	std::deque<Frame> incoming;

	static bool OnOpen(int, const EmscriptenWebSocketOpenEvent *, void *context)
	{
		static_cast<Impl *>(context)->open = true;
		return true;
	}
	static bool OnError(int, const EmscriptenWebSocketErrorEvent *, void *context)
	{
		static_cast<Impl *>(context)->failed = true;
		return true;
	}
	static bool OnClose(int, const EmscriptenWebSocketCloseEvent *, void *context)
	{
		static_cast<Impl *>(context)->open = false;
		static_cast<Impl *>(context)->failed = true;
		return true;
	}
	static bool OnMessage(int, const EmscriptenWebSocketMessageEvent *event, void *context)
	{
		if (event->numBytes > 65552) return true;
		auto *self = static_cast<Impl *>(context);
		self->incoming.push_back({ event->isText, { event->data, event->data + event->numBytes } });
		return true;
	}
};
#else
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct RelaySocket::Impl {
	struct StreamBase {
		virtual ~StreamBase() = default;
		virtual bool Open(asio::io_context &io, const std::string &host, const std::string &port, const std::string &path, std::string &error) = 0;
		virtual void Send(const std::vector<uint8_t> &data, bool text) = 0;
		virtual void Poll() = 0;
		virtual bool IsOpen() const = 0;
		virtual void Close() = 0;
		std::deque<Frame> incoming;
	};

	template <class Stream>
	struct StreamImpl : StreamBase {
		asio::io_context &io;
		Stream stream;
		beast::flat_buffer readBuffer;
		std::deque<std::pair<bool, std::vector<uint8_t>>> outgoing;
		bool open = false;
		bool writing = false;

		template <class... Args>
		StreamImpl(asio::io_context &io, Args &&...args) : io(io), stream(std::forward<Args>(args)...) { }

		bool Open(asio::io_context &, const std::string &host, const std::string &port, const std::string &path, std::string &error) override
		{
			try {
				tcp::resolver resolver(io);
				auto endpoints = resolver.resolve(host, port);
				if constexpr (std::is_same_v<Stream, websocket::stream<asio::ssl::stream<tcp::socket>>>) {
					asio::connect(stream.next_layer().next_layer(), endpoints);
					SSL_set_tlsext_host_name(stream.next_layer().native_handle(), host.c_str());
					stream.next_layer().set_verify_mode(asio::ssl::verify_peer);
					stream.next_layer().set_verify_callback(asio::ssl::host_name_verification(host));
					stream.next_layer().handshake(asio::ssl::stream_base::client);
				} else {
					asio::connect(stream.next_layer(), endpoints);
				}
				stream.binary(true);
				constexpr bool IsTls = std::is_same_v<Stream, websocket::stream<asio::ssl::stream<tcp::socket>>>;
				const std::string origin = std::string(IsTls ? "https://" : "http://") + host + (port == (IsTls ? "443" : "80") ? "" : ":" + port);
				stream.set_option(websocket::stream_base::decorator([origin](websocket::request_type &request) {
					request.set(boost::beast::http::field::origin, origin);
				}));
				stream.handshake(host, path);
				open = true;
				Read();
				return true;
			} catch (const std::exception &exception) {
				error = exception.what();
				return false;
			}
		}

		void Read()
		{
			stream.async_read(readBuffer, [this](boost::system::error_code error, size_t) {
				if (error) { open = false; return; }
				if (readBuffer.size() <= 65552)
					incoming.push_back({ stream.got_text(), { static_cast<const uint8_t *>(readBuffer.data().data()), static_cast<const uint8_t *>(readBuffer.data().data()) + readBuffer.size() } });
				readBuffer.consume(readBuffer.size());
				Read();
			});
		}

		void Write()
		{
			if (writing || outgoing.empty() || !open) return;
			writing = true;
			stream.text(outgoing.front().first);
			stream.async_write(asio::buffer(outgoing.front().second), [this](boost::system::error_code error, size_t) {
				writing = false;
				if (error) { open = false; return; }
				outgoing.pop_front();
				Write();
			});
		}

		void Send(const std::vector<uint8_t> &data, bool text) override
		{
			if (outgoing.size() >= 256) { open = false; return; }
			outgoing.emplace_back(text, data);
			Write();
		}
		void Poll() override { io.restart(); io.poll(); }
		bool IsOpen() const override { return open; }
		void Close() override {
			open = false;
			boost::system::error_code ignored;
			if constexpr (std::is_same_v<Stream, websocket::stream<asio::ssl::stream<tcp::socket>>>)
				stream.next_layer().next_layer().close(ignored);
			else stream.next_layer().close(ignored);
		}
	};

	asio::io_context io;
	asio::ssl::context tls { asio::ssl::context::tls_client };
	std::unique_ptr<StreamBase> stream;
};
#endif

RelaySocket::RelaySocket() : impl_(std::make_unique<Impl>()) { }
RelaySocket::~RelaySocket() { Close(); }

bool RelaySocket::Open(const std::string &url, std::string &error)
{
	Close();
#ifdef __EMSCRIPTEN__
	if (!emscripten_websocket_is_supported()) { error = "WebSockets unavailable"; return false; }
	EmscriptenWebSocketCreateAttributes attributes;
	emscripten_websocket_init_create_attributes(&attributes);
	attributes.url = url.c_str();
	impl_->failed = false;
	impl_->socket = emscripten_websocket_new(&attributes);
	if (impl_->socket <= 0) { error = "Could not open WebSocket"; return false; }
	emscripten_websocket_set_onopen_callback(impl_->socket, impl_.get(), Impl::OnOpen);
	emscripten_websocket_set_onerror_callback(impl_->socket, impl_.get(), Impl::OnError);
	emscripten_websocket_set_onclose_callback(impl_->socket, impl_.get(), Impl::OnClose);
	emscripten_websocket_set_onmessage_callback(impl_->socket, impl_.get(), Impl::OnMessage);
	for (int i = 0; i < 5000 && !impl_->open && !impl_->failed; ++i) emscripten_sleep(1);
	if (!impl_->open) { error = "WebSocket connection timed out"; Close(); return false; }
	return true;
#else
	const auto schemeEnd = url.find("://");
	const auto pathStart = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
	if (schemeEnd == std::string::npos || pathStart == std::string::npos) { error = "Invalid WebSocket URL"; return false; }
	const std::string scheme = url.substr(0, schemeEnd);
	const bool secure = scheme == "wss";
	if (!secure && scheme != "ws") { error = "Invalid WebSocket URL"; return false; }
	const std::string authority = url.substr(schemeEnd + 3, pathStart - schemeEnd - 3);
	const auto colon = authority.rfind(':');
	const std::string host = colon == std::string::npos ? authority : authority.substr(0, colon);
	const std::string port = colon == std::string::npos ? (secure ? "443" : "80") : authority.substr(colon + 1);
	if (secure) {
		impl_->tls.set_default_verify_paths();
		if (const char *ca = std::getenv("DAMNATIONX_LOBBY_CA")) impl_->tls.load_verify_file(ca);
		impl_->stream = std::make_unique<Impl::StreamImpl<websocket::stream<asio::ssl::stream<tcp::socket>>>>(impl_->io, impl_->io, impl_->tls);
	} else {
		impl_->stream = std::make_unique<Impl::StreamImpl<websocket::stream<tcp::socket>>>(impl_->io, impl_->io);
	}
	return impl_->stream->Open(impl_->io, host, port, url.substr(pathStart), error);
#endif
}

bool RelaySocket::SendBinary(const std::vector<uint8_t> &data)
{
	if (!IsOpen() || data.size() > 65552) return false;
#ifdef __EMSCRIPTEN__
	size_t buffered = 0;
	emscripten_websocket_get_buffered_amount(impl_->socket, &buffered);
	if (buffered + data.size() > 256 * 1024) return false;
	return emscripten_websocket_send_binary(impl_->socket, const_cast<uint8_t *>(data.data()), data.size()) == EMSCRIPTEN_RESULT_SUCCESS;
#else
	impl_->stream->Send(data, false);
	return impl_->stream->IsOpen();
#endif
}

bool RelaySocket::SendText(const std::string &text)
{
	if (!IsOpen()) return false;
#ifdef __EMSCRIPTEN__
	return emscripten_websocket_send_utf8_text(impl_->socket, text.c_str()) == EMSCRIPTEN_RESULT_SUCCESS;
#else
	impl_->stream->Send({ text.begin(), text.end() }, true);
	return impl_->stream->IsOpen();
#endif
}

void RelaySocket::Poll(std::vector<Frame> &frames)
{
#ifdef __EMSCRIPTEN__
	while (!impl_->incoming.empty()) { frames.push_back(std::move(impl_->incoming.front())); impl_->incoming.pop_front(); }
#else
	if (!impl_->stream) return;
	impl_->stream->Poll();
	while (!impl_->stream->incoming.empty()) { frames.push_back(std::move(impl_->stream->incoming.front())); impl_->stream->incoming.pop_front(); }
#endif
}

bool RelaySocket::IsOpen() const
{
#ifdef __EMSCRIPTEN__
	return impl_->open;
#else
	return impl_->stream && impl_->stream->IsOpen();
#endif
}

void RelaySocket::Close()
{
#ifdef __EMSCRIPTEN__
	if (impl_->socket > 0) {
		emscripten_websocket_close(impl_->socket, 1000, "");
		emscripten_websocket_delete(impl_->socket);
		impl_->socket = 0;
	}
	impl_->open = false;
	impl_->incoming.clear();
#else
		if (impl_->stream) { impl_->stream->Close(); impl_->stream->Poll(); impl_->stream.reset(); }
#endif
}

} // namespace devilution::net
