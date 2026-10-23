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

// Wire layer for the coop relay: budgets, frame headers, sequence counters,
// video fragments. Deliberately pure bytes - no Common::String, no allocation
// in the formatting path - so it can run per frame from the encoder thread and
// be exercised on its own without a configured ScummVM build.
//
// The byte-level authority is the relay's README ("Wire protocol v2"); this
// header mirrors it and must not drift from it. Note in particular
// FrameReassembler below: the fragment header carries no offsets, so the
// sender must keep every fragment but the last at exactly kMaxFragmentBytes.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_WIRE_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_WIRE_H

#include "common/types.h"

namespace Coop {
namespace Wire {

// ------------------------------------------------------------------ budgets
// Mirrors MAX_DATAGRAM_BYTES / MAX_PAYLOAD_BYTES in the relay's protocol.ts.
const int kMaxDatagramBytes = 1232;
const int kMaxHeaderBytes = 128;
const int kMaxPayloadBytes = 1100;

// Payload sealing overhead ("Media sealing"): a sealed payload is envelope
// byte, ciphertext, then a 16-byte GCM tag.
const uint8 kEnvelopePlaintext = 0x00;
const uint8 kEnvelopeSealed = 0x01;
const int kEnvelopeBytes = 1;
const int kTagBytes = 16;

// Fragment header, "Video and audio":
//   0  uint32le frameId | 4  uint16le index | 6  uint16le count
const int kFragHeaderBytes = 8;

// The relay's cap applies to the *sealed* payload, so the media budget is what
// is left after the envelope and the tag, and then after the fragment header.
// Derived here so the arithmetic lives in one place.
const int kMaxFragmentBytes = kMaxPayloadBytes - kEnvelopeBytes - kTagBytes - kFragHeaderBytes;

const int kMaxRoomCodeLen = 8;
const int kMaxRoomCodeBytes = kMaxRoomCodeLen + 1;

// Recipient marker inside application ("X") payloads. Fan-out on the relay is
// broadcast-only, so a message aimed at one peer names that peer in the payload.
const uint16 kTargetAll = 0xFFFF;

// Frame types, kept in step with the relay's ROUTING table.
bool isKnownFrameType(char type);

// 'h' = host, 'g' = guest. Guests may not inject video/audio/acks and the host
// may not inject input; voice is guest->host and host->guests.
bool maySendAs(char roleTag, char type);

// ------------------------------------------------------------------- header
// A forwarded ("F") header line as received. This is also the AEAD associated
// data for sealed payloads: sealing binds to these exact bytes, so the only
// correct AAD is what formatForwardedHeader() produces.
struct HeaderLine {
	char code[kMaxRoomCodeBytes];
	char roleTag; // 'h' or 'g'
	uint16 peerId;
	char type;
	uint64 seq;
};

// Writes "F <code> <roleTag> <peerId> <type> <seq>\n". Returns the byte count,
// or 0 when the buffer was too small - a truncated header must never be sent.
int formatForwardedHeader(char *out, int capacity, const char *code, char roleTag, uint16 peerId, char type, uint64 seq);

// What a client puts on the wire ("D <code> <peerId> <type> <seq>\n"). The
// relay rewrites this into the "F" form and stamps the sender's identity, so a
// participant cannot claim somebody else's peerId.
int formatOutboundData(char *out, int capacity, const char *code, uint16 peerId, char type, uint64 seq);

// Control frames, text without a payload.
int formatHello(char *out, int capacity, const char *code, uint16 peerId, const char *sessionSecret);
int formatPing(char *out, int capacity, const char *code, uint16 peerId);
int formatBye(char *out, int capacity, const char *code, uint16 peerId);

// Reads the text part of a received datagram. Data frames and the relay's own
// pushes (WELCOME, ROSTER, PONG, BYE-OK, DENY, KICKED, CLOSED) share this one
// socket, so all of them are recognised here.
enum ReceivedKind {
	kRecvData,    // "F ..." - header parsed into `header`, payload follows
	kRecvRoster,  // "R <code> <n> ..." followed by a JSON payload
	kRecvControl, // any other recognised text verb
	kRecvForeign, // well formed but for another room
	kRecvBad      // malformed or oversized
};

struct Received {
	ReceivedKind kind;
	HeaderLine header;   // kRecvData
	char text[kMaxHeaderBytes + 1]; // kRecvControl/kRecvRoster: the whole line, NUL terminated
	const byte *payload; // kRecvData: payload; kRecvRoster: JSON body
	int payloadLen;
};

// `datagram` is the whole UDP packet. Anything larger than kMaxDatagramBytes is
// rejected, matching the relay, because a peer that sends us an oversized packet
// is either broken or hostile.
ReceivedKind parseDatagram(const byte *datagram, int datagramLen, const char *ourCode, Received *out);

// ---------------------------------------------------------------- sequence
// SEQ travels as decimal text and the relay keeps it in a JS number, so the
// largest value that survives a round trip is 2^53-1 (Number.MAX_SAFE_INTEGER).
// Both the counter and the sealing layer stop here rather than emitting a SEQ the
// relay would reject or round. Public because Seal refuses beyond it too.
const uint64 kMaxSeq = 9007199254740991ULL;

// One counter per frame type. The sealing nonce is (type, peerId, SEQ), so a
// repeated (type, SEQ) pair under one key is a cryptographic break, not a
// glitch: a counter must never be reset while its media key stays in use.
//
// Slots are indexed by SeqCounter::slotFor(), which mirrors the relay's ROUTING
// table order. kSeqSlots must stay at least as large as the number of known
// frame types; adding a type to isKnownFrameType without adding a slot is a bug
// the parity harness catches.
class SeqCounter {
public:
	SeqCounter();

	// Only ever called before any frame has been sent. Resetting a counter that
	// has already sealed frames under the current key repeats an AEAD nonce, so
	// Transport never calls this once a media key is installed.
	void reset();

	// Returns false once a counter can no longer advance safely, which means the
	// room has to be re-joined to get a fresh key.
	bool next(char type, uint64 *seqOut);

private:
	static int slotFor(char type);

	static const int kSeqSlots = 9;
	uint64 _values[kSeqSlots];
};

// --------------------------------------------------------------- fragments
void writeFragmentHeader(byte *payload, uint32 frameId, uint16 index, uint16 count);
bool readFragmentHeader(const byte *payload, int payloadLen, uint32 *frameId, uint16 *index, uint16 *count);

// How many fragments a `plainLen` byte media payload needs at this budget.
int fragmentCount(int plainLen);

// --------------------------------------------------------------- reassembly
// Collects fragments of one media frame and hands back the complete payload.
//
// Offset convention the sender must honour: fragments 0..count-2 carry exactly
// kMaxFragmentBytes bytes and only the last may be shorter. The reassembler
// checks this and poisons the frame when it is violated.
//
// Any gap, mismatch or conflict drops the whole frame: for a video decoder a
// partial frame is worse than no frame, because it poisons the references.
class FrameReassembler {
public:
	FrameReassembler();
	~FrameReassembler();

	struct Complete {
		uint32 frameId;
		const byte *data;
		int len;
	};

	// Returns true when `out` describes a finished frame. The data is valid
	// until the next call, so decode or discard it before feeding again.
	// Use one reassembler per media kind (video, audio).
	bool feed(uint32 frameId, uint16 index, uint16 count, const byte *data, int dataLen, Complete *out);

	void reset();

private:
	static const int kSlots = 4;
	static const uint16 kMaxFragments = 64; // bounds the received bitmask

	struct Slot {
		bool inUse;
		bool poisoned;
		uint32 frameId;
		uint16 count;
		uint64 received; // one bit per index
		byte *buffer;
		int capacity;
		int lastLen; // payload length of the final fragment, which fixes the total
		int dataLen; // filled in once complete
	};

	Slot *slotFor(uint32 frameId);
	bool ensureCapacity(Slot *slot, int needed);

	Slot _slots[kSlots];
};

// ---------------------------------------------------------------- envelope
// Wraps a plaintext payload in the envelope the participants agreed on. The relay
// treats payloads as opaque; the leading byte says whether they are sealed. Returns
// the total length written, or 0 on overflow. This is the *plaintext* envelope -
// sealed payloads go through Seal::sealPayload, which also stamps the tag.
int wrapPlaintext(byte *out, int capacity, const byte *payload, int payloadLen);

// Counterpart of wrapPlaintext: splits a received payload into its envelope byte
// and the media bytes inside it, so the fragment header is read from the right
// offset. Returns the envelope byte, or -1 when the payload is empty or uses an
// envelope this helper cannot handle. Envelope 0x01 (AES-256-GCM) needs the key and
// the tag check, so it comes back as -1 here: use Seal::openPayload for that.
int unwrapEnvelope(const byte *payload, int payloadLen, const byte **inner, int *innerLen);

} // namespace Wire
} // namespace Coop

#endif
