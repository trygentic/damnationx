#include "dvlnet/webrtc_socket.h"
#ifdef DAMNATION_NATIVE_WEBRTC
#include "dvlnet/webrtc_native.inc"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

EM_JS(int, WebRtcOpen, (const char *url), {
	if (!window.RTCPeerConnection) return 0;
	const registry = Module.damnationRtc ||= { next: 1, sockets: new Map() };
	const handle = registry.next++;
	const socket = { ws: null, self: 0, peers: new Map(), events: [], open: true };
	registry.sockets.set(handle, socket);
	const envelope = (op, sender, destination, payload = new Uint8Array()) => {
		const bytes = new Uint8Array(16 + payload.length);
		const view = new DataView(bytes.buffer);
		view.setUint32(0, bytes.length, true);
		bytes[4] = 1;
		bytes[6] = op;
		view.setUint32(8, sender, true);
		view.setUint32(12, destination, true);
		bytes.set(payload, 16);
		return bytes;
	};
	const signal = (destination, message) => {
		if (socket.ws.readyState === WebSocket.OPEN)
			socket.ws.send(envelope(3, 0, destination, new TextEncoder().encode(JSON.stringify(message))));
	};
	const removePeer = (id) => {
		const peer = socket.peers.get(id);
		if (!peer) return;
		socket.peers.delete(id);
		socket.events.push(envelope(5, id, socket.self));
		peer.pc.close();
	};
	const bindChannel = (peer, channel) => {
		peer.channel = channel;
		channel.binaryType = 'arraybuffer';
		channel.onopen = () => {
			if (!peer.ready) {
				peer.ready = true;
				socket.events.push(envelope(4, peer.id, socket.self));
			}
		};
		channel.onmessage = event => {
			if (event.data instanceof ArrayBuffer && event.data.byteLength <= 65536)
				socket.events.push(envelope(3, peer.id, socket.self, new Uint8Array(event.data)));
		};
		channel.onclose = () => removePeer(peer.id);
	};
	const createPeer = (id, offerer) => {
		if (socket.peers.has(id)) return socket.peers.get(id);
		const pc = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.cloudflare.com:3478' }] });
		const peer = { id, pc, channel: null, ready: false, candidates: [] };
		socket.peers.set(id, peer);
		pc.onicecandidate = event => {
			if (event.candidate) signal(id, { type: 'candidate', candidate: event.candidate });
		};
		pc.onconnectionstatechange = () => {
			if (pc.connectionState === 'failed' || pc.connectionState === 'closed') removePeer(id);
		};
		pc.ondatachannel = event => bindChannel(peer, event.channel);
		if (offerer) {
			bindChannel(peer, pc.createDataChannel('game', { ordered: true }));
			pc.createOffer().then(offer => pc.setLocalDescription(offer).then(() => signal(id, pc.localDescription)))
				.catch(error => { console.warn('WebRTC offer failed', error); removePeer(id); });
		}
		return peer;
	};
	const receivedSignal = async (id, message) => {
		const peer = createPeer(id, false);
		try {
			if (message.type === 'offer') {
				await peer.pc.setRemoteDescription(message);
				for (const candidate of peer.candidates.splice(0)) await peer.pc.addIceCandidate(candidate);
				const answer = await peer.pc.createAnswer();
				await peer.pc.setLocalDescription(answer);
				signal(id, peer.pc.localDescription);
			} else if (message.type === 'answer') {
				await peer.pc.setRemoteDescription(message);
				for (const candidate of peer.candidates.splice(0)) await peer.pc.addIceCandidate(candidate);
			} else if (message.type === 'candidate') {
				if (peer.pc.remoteDescription) await peer.pc.addIceCandidate(message.candidate);
				else peer.candidates.push(message.candidate);
			}
		} catch (error) { console.warn('WebRTC signaling failed', error); removePeer(id); }
	};
	try {
		const ws = new WebSocket(UTF8ToString(url));
		socket.ws = ws;
		ws.binaryType = 'arraybuffer';
		ws.onopen = () => ws.send(envelope(1, 0, 0));
		ws.onmessage = event => {
			if (!(event.data instanceof ArrayBuffer)) return;
			const bytes = new Uint8Array(event.data);
			if (bytes.length < 16 || new DataView(bytes.buffer).getUint32(0, true) !== bytes.length || bytes[4] !== 1) return;
			const view = new DataView(bytes.buffer);
			const op = bytes[6], sender = view.getUint32(8, true);
			if (op === 4) {
				if (!socket.self) { socket.self = sender; socket.events.push(bytes); }
				else if (sender !== socket.self) createPeer(sender, socket.self > sender);
			} else if (op === 5) removePeer(sender);
			else if (op === 3 && sender && bytes.length <= 8192) {
				try { receivedSignal(sender, JSON.parse(new TextDecoder().decode(bytes.subarray(16)))); }
				catch (error) { console.warn('Invalid WebRTC signal', error); }
			}
		};
		ws.onerror = () => { socket.open = false; };
		ws.onclose = () => { socket.open = false; };
		return handle;
	} catch (error) {
		registry.sockets.delete(handle);
		return 0;
	}
});

EM_JS(int, WebRtcSend, (int handle, const uint8_t *data, int length), {
	const socket = Module.damnationRtc?.sockets.get(handle);
	if (!socket || !socket.open || length < 16) return 0;
	const bytes = HEAPU8.slice(data, data + length);
	if (bytes[6] === 1) return 1; // Join is sent when signaling opens.
	if (bytes[6] !== 3) return 0;
	const destination = new DataView(bytes.buffer).getUint32(12, true);
	const payload = bytes.subarray(16);
	let sent = destination === 0;
	for (const peer of socket.peers.values()) {
		if (destination && peer.id !== destination) continue;
		if (!peer.ready || peer.channel.readyState !== 'open' || peer.channel.bufferedAmount + payload.length > 256 * 1024) continue;
		try { peer.channel.send(payload); sent = true; } catch (error) { sent = false; }
	}
	return sent ? 1 : 0;
});

EM_JS(int, WebRtcNext, (int handle, uint8_t *data, int capacity), {
	const event = Module.damnationRtc?.sockets.get(handle)?.events.shift();
	if (!event) return 0;
	if (event.length > capacity) return -1;
	HEAPU8.set(event, data);
	return event.length;
});

EM_JS(int, WebRtcAlive, (int handle), {
	return Module.damnationRtc?.sockets.get(handle)?.open ? 1 : 0;
});

EM_JS(void, WebRtcClose, (int handle), {
	const socket = Module.damnationRtc?.sockets.get(handle);
	if (!socket) return;
	socket.open = false;
	for (const peer of socket.peers.values()) peer.pc.close();
	socket.ws.close();
	Module.damnationRtc.sockets.delete(handle);
});
#endif

namespace devilution::net {

bool WebRtcSocket::Open(const std::string &url, std::string &error)
{
	Close();
#ifdef __EMSCRIPTEN__
	handle_ = WebRtcOpen(url.c_str());
	if (handle_ != 0) return true;
	error = "WebRTC is unavailable in this browser.";
#elif defined(DAMNATION_NATIVE_WEBRTC)
	native_ = std::make_shared<Native>();
	return native_->Open(url, error);
#else
	(void)url;
	error = "WebRTC games require a browser client.";
#endif
	return false;
}

bool WebRtcSocket::SendBinary(const std::vector<uint8_t> &data)
{
#ifdef __EMSCRIPTEN__
	return handle_ != 0 && WebRtcSend(handle_, data.data(), data.size()) != 0;
#elif defined(DAMNATION_NATIVE_WEBRTC)
	return native_ && native_->Send(data);
#else
	(void)data;
	return false;
#endif
}

void WebRtcSocket::Poll(std::vector<RelaySocket::Frame> &frames)
{
#ifdef __EMSCRIPTEN__
	if (handle_ == 0) return;
	for (;;) {
		std::vector<uint8_t> bytes(65552);
		const int length = WebRtcNext(handle_, bytes.data(), bytes.size());
		if (length <= 0) break;
		bytes.resize(length);
		frames.push_back({ false, std::move(bytes) });
	}
#elif defined(DAMNATION_NATIVE_WEBRTC)
	if (native_) native_->Poll(frames);
#else
	(void)frames;
#endif
}

bool WebRtcSocket::IsOpen() const
{
#ifdef __EMSCRIPTEN__
	return handle_ != 0 && WebRtcAlive(handle_) != 0;
#elif defined(DAMNATION_NATIVE_WEBRTC)
	return native_ && native_->alive && native_->signaling.IsOpen();
#else
	return false;
#endif
}

void WebRtcSocket::Close()
{
#ifdef __EMSCRIPTEN__
	if (handle_ != 0) WebRtcClose(handle_);
#endif
	handle_ = 0;
#ifdef DAMNATION_NATIVE_WEBRTC
	if (native_) native_->Close();
	native_.reset();
#endif
}

} // namespace devilution::net
