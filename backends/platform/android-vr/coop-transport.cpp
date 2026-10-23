/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Needs POSIX sockets and C++ threads, and the system headers both pull collide
// with ScummVM's forbidden-symbol macros - same reason the other android-vr files
// open this way.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-transport.h"

#include "common/debug.h"
#include "common/textconsole.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace Coop {

Transport::Transport()
	: _fd(-1),
	  _stopping(false),
	  _ready(false),
	  _closed(false),
	  _peerId(0),
	  _roleTag('g'),
	  _lastPongMs(0),
	  _keyInstalled(false),
	  _plaintextAllowed(false),
	  _everJoined(false),
	  _warnedNoKey(false),
	  _queueHead(0),
	  _queueCount(0),
	  _droppedEvents(0),
	  _badPackets(0),
	  _cryptoDrops(0) {
	_code[0] = 0;
	_secret[0] = 0;
	_status[0] = 0;
}

Transport::~Transport() {
	close();
}

int64 Transport::monotonicMs() {
	return (int64)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Transport::open(const char *host, int port) {
	if (_fd >= 0)
		return true;
	if (!host || !host[0] || port <= 0 || port > 0xffff)
		return false;

	char portText[8];
	int n = 0;
	int value = port;
	char digits[8];
	int dn = 0;
	do {
		digits[dn++] = char('0' + (value % 10));
		value /= 10;
	} while (value);
	while (dn > 0)
		portText[n++] = digits[--dn];
	portText[n] = 0;

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	struct addrinfo *addresses = nullptr;
	const int rc = getaddrinfo(host, portText, &hints, &addresses);
	if (rc != 0) {
		warning("coop: cannot resolve relay host %s: %s", host, gai_strerror(rc));
		return false;
	}

	for (struct addrinfo *ai = addresses; ai; ai = ai->ai_next) {
		const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		// Connected rather than unbound: HELLO carries the session secret, and a
		// connected socket means only the relay can feed this parser.
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
			::close(fd);
			continue;
		}
		_fd = fd;
		break;
	}
	freeaddrinfo(addresses);

	if (_fd < 0) {
		warning("coop: could not connect a UDP socket to %s:%d", host, port);
		return false;
	}
	return true;
}

void Transport::close() {
	// Say goodbye while the socket is still usable, otherwise the room keeps this
	// peer's binding until the relay's expiry timer runs.
	if (_code[0] && _ready && !_closed) {
		char line[Wire::kMaxHeaderBytes];
		std::lock_guard<std::mutex> lock(_sendMutex);
		const int len = Wire::formatBye(line, sizeof(line), _code, _peerId);
		if (len > 0 && _fd >= 0)
			send(_fd, line, len, 0);
	}

	_stopping = true;
	if (_thread.joinable())
		_thread.join();

	if (_fd >= 0) {
		::close(_fd);
		_fd = -1;
	}

	_ready = false;
	_closed = false;
	_code[0] = 0;
	_secret[0] = 0;

	{
		// Leaving the room drops the key: a re-join has to fetch the new room's key
		// over the control plane, and an old key must not leak into a new session.
		// The receive thread is already joined here, so nothing can be reading the
		// context at this point. Note what does NOT get reset: _everJoined stays
		// true, which is what keeps this instance from being used for a second
		// session under a restarted sequence counter.
		std::lock_guard<std::mutex> lock(_sendMutex);
		_seal.clear();
		_keyInstalled = false;
		_warnedNoKey = false;
	}

	std::lock_guard<std::mutex> lock(_queueMutex);
	_queueHead = 0;
	_queueCount = 0;
}

bool Transport::isOpen() const {
	return _fd >= 0;
}

bool Transport::join(const char *code, uint16 peerId, const char *sessionSecret, char roleTag) {
	if (_fd < 0 || _thread.joinable())
		return false;
	if (_everJoined) {
		// The relay gives every session a fresh peerId, so a re-join is a different
		// identity and belongs on a fresh instance with fresh counters. Continuing
		// here would either restart the counters under an identity this instance has
		// already sealed frames as, or keep a stale identity alive; both are nonce
		// hazards, so this is refused rather than papered over.
		warning("coop: this Transport already joined a session; make a new one with a new control-plane session");
		return false;
	}
	if (!code || !sessionSecret)
		return false;
	const int codeLen = (int)strlen(code);
	const int secretLen = (int)strlen(sessionSecret);
	if (codeLen < 4 || codeLen > Wire::kMaxRoomCodeLen)
		return false;
	if (secretLen < 1 || secretLen > kMaxSecretLen)
		return false;
	if (roleTag != 'h' && roleTag != 'g')
		return false;

	{
		std::lock_guard<std::mutex> lock(_sendMutex);
		// Fail closed. The control plane advertises its sealing scheme and hands over
		// the key before anybody joins, so arriving here without one means the key
		// was dropped on the floor somewhere - and video of a game being played is
		// not something to send in the clear by accident.
		if (!_keyInstalled && !_plaintextAllowed) {
			warning("coop: refusing to join %s: no media key. Call setMediaKey() first, or "
				"enablePlaintextMode() if this is a development room that really has no key",
				code);
			return false;
		}
		if (_keyInstalled && _plaintextAllowed) {
			// Both is a configuration mistake, and the safe reading of a mistake in
			// this area is to stop.
			warning("coop: refusing to join %s: a media key and plaintext mode are both set", code);
			return false;
		}

		memcpy(_code, code, codeLen + 1);
		memcpy(_secret, sessionSecret, secretLen + 1);
		_peerId = peerId;
		_roleTag = roleTag;
		_everJoined = true;
		// The SEQ counters are deliberately not reset here. They belong to the
		// (media key, peerId) namespace, and on this instance that namespace has
		// exactly one owner: the constructor's zeros are the only reset there is.
	}

	_ready = false;
	_closed = false;
	_lastPongMs = monotonicMs();
	noteStatus("");

	// The receive thread starts here rather than in open() so that the
	// credentials are settled before anything is sent or parsed, and so a
	// half-configured socket never has a thread reading from it.
	_stopping = false;
	_thread = std::thread(&Transport::receiveLoop, this);
	return true;
}

bool Transport::setMediaKey(const char *mediaKeyHex) {
	std::lock_guard<std::mutex> lock(_sendMutex);

	if (_keyInstalled) {
		// No rotation, by design: the SEQ counters on this instance belong to the key
		// that is already installed, so swapping the key out from under them would
		// repeat nonces. A different room means a new Transport.
		warning("coop: a media key is already installed on this Transport; join a new room with a new one");
		return false;
	}
	if (_everJoined) {
		// Frames may already have gone out unsealed, and a key installed mid-session
		// means two different envelope regimes on one identity.
		warning("coop: setMediaKey() must come before join()");
		return false;
	}
	if (_plaintextAllowed) {
		warning("coop: this Transport was set to plaintext mode; make a new one to use a key");
		return false;
	}

	if (!_seal.setFromHex(mediaKeyHex)) {
		warning("coop: control plane media key is not %d hex characters; nothing will be sealed",
			Seal::kKeyHexChars);
		_seal.clear();
		return false;
	}

	// Hand-written field arithmetic gets checked before it gets used. This is a few
	// milliseconds once per room, and it is what turns a bit flip in the table build
	// into a refused session instead of media nobody can decrypt.
	if (!_seal.selfCheck()) {
		warning("coop: sealing context failed its self-check; refusing to seal with it");
		_seal.clear();
		return false;
	}

	_keyInstalled = true;
	_warnedNoKey = false;
	return true;
}

bool Transport::enablePlaintextMode() {
	std::lock_guard<std::mutex> lock(_sendMutex);
	if (_keyInstalled || _everJoined)
		return false;
	_plaintextAllowed = true;
	warning("coop: plaintext mode enabled for this Transport: payloads go out unencrypted and "
		"unauthenticated, and the relay can read and rewrite them. Development use only");
	return true;
}

bool Transport::isSealed() const {
	std::lock_guard<std::mutex> lock(_sendMutex);
	return _seal.hasKey();
}

int Transport::maxAppPayloadBytes() const {
	std::lock_guard<std::mutex> lock(_sendMutex);
	// Sealing costs the envelope byte and the tag; plaintext only costs the envelope.
	return _seal.hasKey() ? Seal::kMaxPlaintextBytes : Wire::kMaxPayloadBytes - Wire::kEnvelopeBytes;
}

bool Transport::sendFrame(char type, const byte *payload, int payloadLen) {
	if (!payload || payloadLen < 0)
		return false;
	if (!Wire::maySendAs(_roleTag, type))
		return false;
	// The size limit is enforced where the envelope is applied, because what fits
	// depends on whether this frame is being sealed.
	return sendEnclosed(type, payload, payloadLen);
}

bool Transport::sendMedia(char type, uint32 frameId, const byte *data, int dataLen) {
	if (!data || dataLen <= 0)
		return false;
	if (!Wire::maySendAs(_roleTag, type))
		return false;

	const int count = Wire::fragmentCount(dataLen);
	// The reassembler on the other end tracks at most 64 fragments per frame.
	if (count < 1 || count > 64)
		return false;

	for (int index = 0; index < count; ++index) {
		const int offset = index * Wire::kMaxFragmentBytes;
		int chunk = dataLen - offset;
		if (chunk > Wire::kMaxFragmentBytes)
			chunk = Wire::kMaxFragmentBytes;

		// Plain fragment = fragment header + media bytes, and the fragment header
		// is inside the sealed region, not outside it. Loop arithmetic guarantees
		// the uniform-chunk rule: only the final fragment can be short.
		byte plain[Wire::kFragHeaderBytes + Wire::kMaxFragmentBytes];
		Wire::writeFragmentHeader(plain, frameId, (uint16)index, (uint16)count);
		memcpy(plain + Wire::kFragHeaderBytes, data + offset, chunk);

		// Only the bytes actually filled in travel: taking sizeof(plain) would send
		// the tail of this stack buffer and make the final fragment look like a
		// full-size one, which silently changes the reassembled length.
		const int plainLen = Wire::kFragHeaderBytes + chunk;
		// Each fragment is its own datagram with its own SEQ: the sealing nonce is
		// (type, peerId, SEQ), so it must differ per fragment.
		if (!sendEnclosed(type, plain, plainLen))
			return false;
	}
	return true;
}

bool Transport::sendEnclosed(char type, const byte *plain, int plainLen) {
	if (_fd < 0 || !plain || plainLen < 0)
		return false;

	byte wire[Wire::kMaxPayloadBytes];
	char datagram[Wire::kMaxDatagramBytes];
	std::lock_guard<std::mutex> lock(_sendMutex);
	if (_fd < 0)
		return false;

	uint64 seq = 0;
	if (!_seq.next(type, &seq)) {
		// Do not send: continuing past the ceiling would repeat a nonce under the
		// same media key, which is worse than losing this frame.
		warning("coop: sequence exhausted for frame type %c; the room must be re-joined", type);
		return false;
	}

	// The envelope is applied here rather than by the caller because the sealing
	// nonce and the AAD both need this datagram's SEQ.
	int wireLen = 0;
	if (_seal.hasKey()) {
		Wire::HeaderLine ref;
		memset(&ref, 0, sizeof(ref));
		memcpy(ref.code, _code, sizeof(ref.code));
		// The recipient authenticates the *forwarded* line, where the relay has
		// stamped our role and peerId, so the reference carries our own identity -
		// not the "D" line we are about to write.
		ref.roleTag = _roleTag;
		ref.peerId = _peerId;
		ref.type = type;
		ref.seq = seq;
		wireLen = Seal::sealPayload(_seal, wire, sizeof(wire), ref, plain, plainLen);
	} else {
		wireLen = Wire::wrapPlaintext(wire, sizeof(wire), plain, plainLen);
	}
	if (wireLen == 0) {
		// Over budget for this mode. The SEQ is spent regardless, which is safe:
		// skipping a nonce cannot cause a reuse.
		debug("coop: %d bytes do not fit a %c datagram", plainLen, type);
		return false;
	}

	const int headLen = Wire::formatOutboundData(datagram, sizeof(datagram), _code, _peerId, type, seq);
	if (headLen == 0)
		return false;
	if (headLen + wireLen > (int)sizeof(datagram))
		return false;
	memcpy(datagram + headLen, wire, wireLen);

	// One sendto per frame: the header and the payload must not become two
	// datagrams.
	if (send(_fd, datagram, headLen + wireLen, 0) < 0) {
		debug("coop: UDP send failed: %s", strerror(errno));
		return false;
	}
	return true;
}

bool Transport::sendTextFrame(const char *line, int len) {
	if (_fd < 0 || !line || len <= 0)
		return false;
	std::lock_guard<std::mutex> lock(_sendMutex);
	if (send(_fd, line, len, 0) < 0) {
		debug("coop: UDP send failed: %s", strerror(errno));
		return false;
	}
	return true;
}

bool Transport::pollEvent(Event *out) {
	if (!out)
		return false;
	std::lock_guard<std::mutex> lock(_queueMutex);
	if (_queueCount == 0)
		return false;
	*out = _queue[_queueHead];
	_queueHead = (_queueHead + 1) % kQueueDepth;
	--_queueCount;
	return true;
}

bool Transport::isReady() const {
	return _ready;
}

bool Transport::isClosed() const {
	return _closed;
}

void Transport::statusText(char *out, int capacity) const {
	if (!out || capacity <= 0)
		return;
	std::lock_guard<std::mutex> lock(_stateMutex);
	int i = 0;
	for (; i < capacity - 1 && _status[i]; ++i)
		out[i] = _status[i];
	out[i] = 0;
}

void Transport::noteStatus(const char *text) {
	std::lock_guard<std::mutex> lock(_stateMutex);
	int i = 0;
	for (; i < (int)sizeof(_status) - 1 && text && text[i]; ++i)
		_status[i] = text[i];
	_status[i] = 0;
}

namespace {

bool startsWithWord(const char *text, const char *word) {
	const int len = (int)strlen(word);
	if (strncmp(text, word, len) != 0)
		return false;
	return text[len] == 0 || text[len] == ' ';
}

} // namespace

void Transport::handleDatagram(const byte *packet, int len) {
	char code[Wire::kMaxRoomCodeBytes];
	{
		std::lock_guard<std::mutex> lock(_sendMutex);
		memcpy(code, _code, sizeof(code));
	}
	if (!code[0])
		return;

	Wire::Received received;
	const Wire::ReceivedKind kind = Wire::parseDatagram(packet, len, code, &received);

	switch (kind) {
	case Wire::kRecvForeign:
		// Not our room: silently ignored, because a code we did not ask for is not
		// something to react to or echo.
		return;
	case Wire::kRecvBad:
		++_badPackets;
		return;
	case Wire::kRecvControl:
		if (startsWithWord(received.text, "WELCOME")) {
			_ready = true;
			_lastPongMs = monotonicMs();
		} else if (startsWithWord(received.text, "PONG")) {
			_lastPongMs = monotonicMs();
		} else if (startsWithWord(received.text, "CLOSED") || startsWithWord(received.text, "KICKED")) {
			_closed = true;
			_ready = false;
		} else if (startsWithWord(received.text, "DENY")) {
			_closed = true;
			_ready = false;
		}
		noteStatus(received.text);
		pushEvent(received, 0);
		break;
	case Wire::kRecvData: {
		// Open the envelope here rather than in the consumer: a payload that fails
		// the tag, or that arrived in an envelope this transport does not accept,
		// must never reach a video decoder or a chat string.
		byte plain[Wire::kMaxPayloadBytes];
		int plainLen = 0;
		int envelope = 0;
		if (!openEnvelope(received, plain, sizeof(plain), &plainLen, &envelope)) {
			++_cryptoDrops;
			return;
		}
		received.payload = plain;
		received.payloadLen = plainLen;
		pushEvent(received, envelope);
		break;
	}
	case Wire::kRecvRoster:
		// The roster is written by the relay itself and travels unsealed, like the
		// other control pushes.
		pushEvent(received, 0);
		break;
	}
}

bool Transport::openEnvelope(const Wire::Received &received, byte *plain, int capacity, int *plainLen,
	int *envelopeOut) {
	// The sealing context is read-only from here on: a key is installed before the
	// receive thread starts and is never replaced while that thread runs, so the GCM
	// calls below need no lock. close() only clears it after the thread is joined.
	const bool sealedMode = _seal.hasKey();

	const int envelope = Seal::envelopeKind(received.payload, received.payloadLen);
	if (envelope < 0)
		return false;
	if (envelopeOut)
		*envelopeOut = envelope;

	// Envelope and key must agree. A sealed frame we cannot read is simply lost; a
	// plaintext frame once we hold the room's key is a downgrade attempt, and there
	// is no reason to accept one, because the control plane gates on
	// media.encryption before anybody joins.
	if (sealedMode != (envelope == Wire::kEnvelopeSealed)) {
		if (envelope == Wire::kEnvelopeSealed && !sealedMode) {
			std::lock_guard<std::mutex> lock(_sendMutex);
			if (!_warnedNoKey) {
				_warnedNoKey = true;
				warning("coop: room is sealing but no media key was set; inbound media is being dropped");
			}
		}
		return false;
	}

	const int n = Seal::openPayload(_seal, plain, capacity, received.header, received.payload,
		received.payloadLen, !sealedMode);
	if (n < 0)
		return false;
	*plainLen = n;
	return true;
}

void Transport::pushEvent(const Wire::Received &received, int envelope) {
	std::lock_guard<std::mutex> lock(_queueMutex);
	if (_queueCount == 0) {
		_queueHead = 0;
	} else if (_queueCount >= kQueueDepth) {
		// Full queue: drop the oldest. A stalled consumer should see the newest
		// frames, and video that is a second late is already not worth decoding.
		const int oldest = _queueHead;
		_queueHead = (_queueHead + 1) % kQueueDepth;
		--_queueCount;
		if (_queue[oldest].kind == Wire::kRecvControl || _queue[oldest].kind == Wire::kRecvRoster)
			warning("coop: dropping relay control message: %s", _queue[oldest].text);
		++_droppedEvents;
	}

	const int slot = (_queueHead + _queueCount) % kQueueDepth;
	Event &e = _queue[slot];
	e.kind = received.kind;
	e.header = received.header;
	e.payloadLen = 0;
	e.envelope = (uint8)envelope;
	memset(e.text, 0, sizeof(e.text));
	memcpy(e.text, received.text, sizeof(e.text) - 1);
	if (received.payload && received.payloadLen > 0) {
		e.payloadLen = received.payloadLen;
		if (e.payloadLen > (int)sizeof(e.payload))
			e.payloadLen = (int)sizeof(e.payload);
		memcpy(e.payload, received.payload, e.payloadLen);
	}
	++_queueCount;
}

void Transport::receiveLoop() {
	int64 lastHelloMs = 0;
	int64 lastPingMs = 0;

	while (!_stopping) {
		const int64 now = monotonicMs();
		char line[Wire::kMaxHeaderBytes];
		int len = 0;
		bool sendControl = false;

		{
			std::lock_guard<std::mutex> lock(_sendMutex);
			if (_code[0] && !_ready && !_closed && now - lastHelloMs >= kHelloRetryMs) {
				lastHelloMs = now;
				len = Wire::formatHello(line, sizeof(line), _code, _peerId, _secret);
				sendControl = len > 0;
			} else if (_code[0] && _ready && !_closed && now - lastPingMs >= kPingIntervalMs) {
				lastPingMs = now;
				len = Wire::formatPing(line, sizeof(line), _code, _peerId);
				sendControl = len > 0;
			}
		}
		if (sendControl)
			sendTextFrame(line, len);

		if (_ready && !_closed && now - _lastPongMs > kLivenessMs) {
			// The relay drops a binding after 30 s of silence; if we have not seen
			// anything for twice our ping interval, treat the room as gone rather
			// than encoding into the void.
			_closed = true;
			noteStatus("liveness lost");
			debug("coop: no answer from the relay for %d ms", kLivenessMs);
		}

		struct pollfd pfd;
		pfd.fd = _fd;
		pfd.events = POLLIN;
		const int ready = poll(&pfd, 1, 200);
		if (_stopping)
			break;
		if (ready > 0 && (pfd.revents & POLLIN)) {
			byte packet[Wire::kMaxDatagramBytes];
			const int received = (int)recv(_fd, packet, sizeof(packet), 0);
			if (received > 0)
				handleDatagram(packet, received);
		} else if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			_closed = true;
			noteStatus("socket error");
			break;
		}
	}
}

} // namespace Coop
