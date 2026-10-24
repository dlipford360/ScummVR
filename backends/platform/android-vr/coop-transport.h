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

// UDP side of the coop relay: one connected socket, one receive thread that keeps
// the room alive (HELLO retry, PING) and queues what arrives for the game thread.
//
// The socket is connect()ed to the relay, not bound loosely: HELLO carries the
// session secret, and a connected socket means only the relay's replies can reach
// this parser.
//
// Media payloads are sealed end to end once setMediaKey() has been called, so the
// relay forwards bytes it can neither read nor rewrite: its identity stamping stops
// being the only thing a participant has to trust. What is still plain text is the
// control traffic on this socket (HELLO with the session secret, PING, the relay's
// WELCOME/ROSTER/DENY pushes) and the HTTP control plane unless it runs behind TLS,
// so the room code and session secret remain network-sensitive until that changes.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_TRANSPORT_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_TRANSPORT_H

#include "common/types.h"
#include "backends/platform/android-vr/coop-seal.h"
#include "backends/platform/android-vr/coop-wire.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Coop {

class Transport {
public:
	// The relay expires a UDP binding after 30 s of silence, so pings go out well
	// inside that window. HELLO is retried until the relay answers.
	static const int kPingIntervalMs = 5000;
	static const int kHelloRetryMs = 2000;
	static const int kLivenessMs = 15000;
	static const int kMaxSecretLen = 128;

	// One inbound datagram, already parsed. Payloads are copied in, so events own
	// their bytes and nothing points into the socket buffer. For kRecvData the
	// envelope has already been authenticated and stripped: `payload` is plaintext
	// and `envelope` records what was on the wire.
	struct Event {
		Wire::ReceivedKind kind;
		Wire::HeaderLine header;
		char text[Wire::kMaxHeaderBytes + 1];
		byte payload[Wire::kMaxDatagramBytes]; // plaintext after opening, or the roster JSON
		int payloadLen;
		uint8 envelope; // kRecvData: Wire::kEnvelopePlaintext or kEnvelopeSealed
	};

	Transport();
	~Transport();

	// Resolves `host` (literal address or name) and connects a UDP socket. Does
	// not start the thread; call join() for that, so the credentials are in place
	// before the first HELLO leaves.
	bool open(const char *host, int port);

	// Sends BYE, then stops the receive thread and closes the socket. Safe to call
	// more than once, and the destructor calls it.
	void close();

	bool isOpen() const;

	// Room credentials from the control plane. roleTag is 'h' or 'g': the relay
	// stamps identity itself, but knowing our role locally means we never spend a
	// SEQ value on a frame the relay would drop anyway.
	//
	// One Transport instance joins exactly one session. That is a cryptographic
	// rule, not tidiness: the sealing nonce is (frame type, peerId, SEQ) under the
	// room's key, so re-joining with an identity that already sent frames on this
	// instance would either reuse nonces or keep counting under an identity the
	// relay no longer recognises. Reconnecting means a new control-plane session
	// (the relay hands out a fresh peerId every time) and a new Transport. A host
	// that has lost its ticket cannot re-issue one at all: the relay refuses, and
	// it hosts a new room instead.
	//
	// Joining also refuses unless sealing is configured, because a session that
	// silently runs in the clear is worse than one that fails to start. Call
	// enablePlaintextMode() first when you really want an unsealed dev room.
	bool join(const char *code, uint16 peerId, const char *sessionSecret, char roleTag);

	// Turns sealing on with the room's `mediaKey` from the control plane (64 hex
	// characters). Call it before join(); it expands the key schedule and the GHASH
	// tables once, and the result is what every later frame is sealed and opened
	// with.
	//
	// There is no rotation. One key per room, for the life of the instance: the
	// nonce is (type, peerId, SEQ) and SEQ is never reset while a key is installed,
	// so a new key means a new room and a new Transport. A second call with a
	// different key is refused, and the installed context is checked against a slow
	// reference path before it is used.
	bool setMediaKey(const char *mediaKeyHex);

	// Opt in to sending and accepting envelope 0x00. Dev/test only: it exists so a
	// harness or a laptop-on-laptop test can run without the control plane handing
	// out a key, and it is deliberately an explicit, loud step rather than a
	// default. join() refuses without this or a media key.
	bool enablePlaintextMode();

	// Sealing is on for this transport.
	bool isSealed() const;

	// Largest application ("X"/"K"/"I") payload that fits one datagram right now.
	// Sealing costs envelope plus tag, so the number shrinks when the key is set.
	int maxAppPayloadBytes() const;

	// Application/control frames and input: one datagram, envelope applied by this
	// class (sealed when a media key is set). `payload` is the plaintext.
	bool sendFrame(char type, const byte *payload, int payloadLen);

	// Video and anything else larger than the cap: split into fragments with the
	// fragment header, each fragment sealed-and-enveloped separately.
	bool sendMedia(char type, uint32 frameId, const byte *data, int dataLen);

	// Retransmit support. A receiver that detects a missing 'V' frame asks for it
	// by frameId (the X kind 9 request in the client guide); the sender can only
	// answer if it still has the AU, so a host that wants to answer requests
	// enables this first: sendMedia then keeps each AU for a short window, and
	// resendMedia re-sends the stored bytes under the same frameId. The other
	// end's reassembler ignores duplicate fragments, so a frame that was only
	// partially lost simply completes.
	bool enableMediaHistory();
	bool resendMedia(uint32 frameId);

	// Non-blocking. Returns false when the queue is empty.
	bool pollEvent(Event *out);

	// Relay answered HELLO with WELCOME.
	bool isReady() const;
	// Room is gone (CLOSED/KICKED), or the relay stopped answering.
	bool isClosed() const;
	// Last control line seen, for reporting: DENY reasons, CLOSED, KICKED.
	void statusText(char *out, int capacity) const;

	// Relay-clock synchronisation, measured from the keepalive it already does:
	// the PONG carries the relay's wall clock (protocol.ts), so a PING/PONG
	// exchange yields offset = relayEpoch - (t1 + RTT/2). The median of the
	// recent samples converts this instance's monotonic clock to the relay's -
	// which is how two headsets' timestamps become comparable without either
	// trusting its own wall clock.
	bool clockSynced() const { return _clockSamples >= kClockMinSamples; }
	int64 clockEpochOffsetUs() const;

	// Largest observed RTT/2 across the kept samples: the honest error bar on
	// every offset-derived comparison.
	int64 clockUncertaintyUs() const;

	uint32 droppedEvents() const { return _droppedEvents; }
	uint32 badPackets() const { return _badPackets; }
	// Inbound frames this transport could not authenticate: wrong key, rewritten
	// header, truncated payload, or a sealed frame while no key was configured.
	uint32 cryptoDrops() const { return _cryptoDrops; }
	// Datagrams read from the socket, counted whether or not they parsed. Paired with
	// hellosSent(), this is the difference between "nothing came back" and "something
	// came back that we could not read" - which a client that never sees WELCOME
	// cannot otherwise tell.
	uint32 datagramsIn() const { return _datagramsIn; }
	// HELLO datagrams written to the socket. hellosSent() > 0 with datagramsIn() == 0
	// says the outbound direction is the broken one, which is a different diagnosis
	// (and a different fix) than a parse or key failure.
	uint32 hellosSent() const { return _hellosSent; }

private:
	// Absorb two maximum-size media frames while the consumer services the
	// decoder, plus room for control traffic. This is burst capacity, not a
	// playout delay; the consumer drains it immediately.
	static const int kQueueDepth = 2 * Wire::FrameReassembler::kMaxFragments + 32;

	void receiveLoop();
	void handleDatagram(const byte *packet, int len);
	void pushEvent(const Wire::Received &received, int envelope);

	// Header line plus payload in a single sendto; SEQ is stamped here and the
	// envelope is applied here, because the sealing nonce needs that SEQ.
	bool sendEnclosed(char type, const byte *plain, int plainLen);
	bool sendTextFrame(const char *line, int len);

	// Counterpart of the envelope step in sendEnclosed: authenticates and opens a
	// received payload, so no consumer ever sees bytes that failed the tag.
	// `envelopeOut` reports what was on the wire, for the event record.
	bool openEnvelope(const Wire::Received &received, byte *plain, int capacity, int *plainLen, int *envelopeOut);

	void noteStatus(const char *text);
	static int64 monotonicMs();
	static int64 monotonicUs();

	// Feed one PONG's relay stamp into the clock estimator. `sentMonoUs` is
	// when the matching PING left, `recvMonoUs` when the PONG arrived.
	void noteClockSample(int64 relayEpochUs, int64 sentMonoUs, int64 recvMonoUs);

	int _fd;
	std::thread _thread;
	std::atomic<bool> _stopping;
	std::atomic<bool> _ready;
	std::atomic<bool> _closed;

	char _code[Wire::kMaxRoomCodeBytes];
	uint16 _peerId;
	char _roleTag;
	char _secret[kMaxSecretLen + 1];
	std::atomic<int64> _lastPongMs;

	// Key-dependent material for the one session this instance serves: the AES
	// schedule, the GHASH subkey and its tables. Installed before join() and read by
	// the send path and the receive thread afterwards; the GCM operations through it
	// are const, so no lock is needed once the thread exists. Cleared in close(),
	// after the thread has been joined.
	Seal::Context _seal;
	// A media key has been installed on this instance. join() requires it, unless
	// plaintext mode was asked for instead.
	bool _keyInstalled;
	bool _plaintextAllowed;
	// join() is one-shot per instance. See the join() comment for why.
	bool _everJoined;
	bool _warnedNoKey;

	mutable std::mutex _sendMutex; // credentials, SEQ counters, and socket writes
	Wire::SeqCounter _seq;

	// PING/PONG-derived relay clock offset (see clockEpochOffsetUs). Samples
	// are kept on the receive thread that produces them; readers take a
	// median of the small window, so a single spike on either leg cannot move
	// the reported offset.
	static const int kClockWindow = 8;
	static const int kClockMinSamples = 3;
	mutable std::mutex _clockMutex; // estimator is written by the receive thread, read by const getters
	int64 _clockOffsets[kClockWindow];
	int64 _clockHalves[kClockWindow]; // RTT/2 per sample, for the error bar
	std::atomic<int> _clockSamples; // total ever produced; window is ring-filled
	int64 _pingSentMonoUs;

	// Media sends are serialized whole-AU: the pacing in sendMedia releases
	// _sendMutex between fragments, and a retransmit from the room thread must
	// not interleave its fragments into a live frame's burst. One counter
	// ('V') namespace, and SEQ stays monotonic per frame type either way.
	std::mutex _mediaSendMutex;

	// Retransmit window: recent AUs kept for resendMedia. Sized in bytes and
	// age, not frame count: at the 4 Mbit/s cap 1 MB is ~2 s of video, which
	// covers the RTT plus the guest's detection delay with room to spare.
	static const int kHistoryMaxBytes = 1024 * 1024;
	static const int64 kHistoryMaxAgeMs = 2000;
	struct MediaHistoryEntry {
		uint32 frameId;
		int64 sentMs;
		std::vector<byte> data;
	};
	bool _mediaHistory;
	int _historyBytes;
	std::mutex _historyMutex;
	std::deque<MediaHistoryEntry> _history;

	mutable std::mutex _stateMutex;
	char _status[Wire::kMaxHeaderBytes + 1];

	std::mutex _queueMutex;
	Event _queue[kQueueDepth];
	int _queueHead;
	int _queueCount;

	std::atomic<uint32> _droppedEvents;
	std::atomic<uint32> _badPackets;
	std::atomic<uint32> _cryptoDrops;
	std::atomic<uint32> _datagramsIn;
	std::atomic<uint32> _hellosSent;
};

} // namespace Coop

#endif
