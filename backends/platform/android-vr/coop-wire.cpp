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

#include "common/types.h"
#include "backends/platform/android-vr/coop-wire.h"

#include <cstring>

namespace Coop {
namespace Wire {

namespace {

// Hand-built output buffer. snprintf is avoided on purpose: this runs for every
// fragment of every frame, the decimal conversion is a handful of instructions,
// and truncation is detected per field instead of by re-reading a return value.
struct Writer {
	char *out;
	int capacity;
	int pos;

	void put(char c) {
		if (pos < capacity)
			out[pos++] = c;
		else
			++pos; // still counts, so finish() reports overflow
	}

	void putStr(const char *s) {
		while (*s)
			put(*s++);
	}

	void putUint(uint64 value) {
		char digits[20];
		int n = 0;
		do {
			digits[n++] = char('0' + (value % 10));
			value /= 10;
		} while (value);
		while (n > 0)
			put(digits[--n]);
	}

	int finish() {
		if (pos > capacity)
			return 0;
		return pos;
	}
};

bool isCodeChar(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Mirrors CODE_RE in the relay's protocol.ts: 4..8 characters of [A-Z0-9].
bool isValidCode(const char *code, int len) {
	if (len < 4 || len > kMaxRoomCodeLen)
		return false;
	for (int i = 0; i < len; ++i) {
		if (!isCodeChar(code[i]))
			return false;
	}
	return true;
}

bool isValidCode(const char *code) {
	return isValidCode(code, (int)strlen(code));
}

// Mirrors parsePeerId() in the relay: 1..4 digits, never zero.
bool parsePeerId(const char *token, int len, uint16 *out) {
	if (len < 1 || len > 4)
		return false;
	uint32 value = 0;
	for (int i = 0; i < len; ++i) {
		if (token[i] < '0' || token[i] > '9')
			return false;
		value = value * 10 + (token[i] - '0');
	}
	if (value < 1 || value > 0xffff)
		return false;
	*out = (uint16)value;
	return true;
}

// kMaxSeq comes from the header: the counter and the sealing layer share one
// ceiling.

bool parseSeq(const char *token, int len, uint64 *out) {
	if (len < 1 || len > 16)
		return false;
	uint64 value = 0;
	for (int i = 0; i < len; ++i) {
		if (token[i] < '0' || token[i] > '9')
			return false;
		value = value * 10 + (token[i] - '0');
		if (value > kMaxSeq)
			return false;
	}
	*out = value;
	return true;
}

void copyCode(char *dst, const char *src, int len) {
	if (len > kMaxRoomCodeLen)
		len = kMaxRoomCodeLen;
	memcpy(dst, src, len);
	dst[len] = 0;
}

uint32 readLE32(const byte *p) {
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

uint16 readLE16(const byte *p) {
	return (uint16)((uint32)p[0] | ((uint32)p[1] << 8));
}

} // namespace

bool isKnownFrameType(char type) {
	switch (type) {
	case 'V': // video
	case 'A': // audio (unused by the host today, reserved)
	case 'K': // keyframe ack
	case 'C': // cursor
	case 'I': // input: pointer move
	case 'B': // input: button
	case 'M': // voice
	case 'X': // application control
	case 'P': // head pose (reserved in v3; routed to every other participant)
		return true;
	default:
		return false;
	}
}

bool maySendAs(char roleTag, char type) {
	if (!isKnownFrameType(type))
		return false;
	if (roleTag == 'h') {
		// The host never injects input: it is the only peer the game listens to.
		return type != 'I' && type != 'B';
	}
	if (roleTag == 'g') {
		// Guests cannot inject video, keyframe acks or the reserved audio type.
		// Pose ('P') is deliberately allowed either way: every participant
		// publishes their own, and the relay routes it to everyone else.
		return type != 'V' && type != 'K' && type != 'A';
	}
	return false;
}

// ------------------------------------------------------------------- header

int formatForwardedHeader(char *out, int capacity, const char *code, char roleTag, uint16 peerId, char type, uint64 seq) {
	if (!out || capacity <= 0 || !isValidCode(code) || !isKnownFrameType(type))
		return 0;
	Writer w = { out, capacity, 0 };
	w.put('F');
	w.put(' ');
	w.putStr(code);
	w.put(' ');
	w.put(roleTag);
	w.put(' ');
	w.putUint(peerId);
	w.put(' ');
	w.put(type);
	w.put(' ');
	w.putUint(seq);
	w.put('\n');
	return w.finish();
}

int formatOutboundData(char *out, int capacity, const char *code, uint16 peerId, char type, uint64 seq) {
	if (!out || capacity <= 0 || !isValidCode(code) || !isKnownFrameType(type))
		return 0;
	Writer w = { out, capacity, 0 };
	w.put('D');
	w.put(' ');
	w.putStr(code);
	w.put(' ');
	w.putUint(peerId);
	w.put(' ');
	w.put(type);
	w.put(' ');
	w.putUint(seq);
	w.put('\n');
	return w.finish();
}

int formatHello(char *out, int capacity, const char *code, uint16 peerId, const char *sessionSecret) {
	if (!out || capacity <= 0 || !isValidCode(code) || !sessionSecret || !sessionSecret[0])
		return 0;
	Writer w = { out, capacity, 0 };
	w.putStr("HELLO ");
	w.putStr(code);
	w.put(' ');
	w.putUint(peerId);
	w.put(' ');
	w.putStr(sessionSecret);
	w.put('\n');
	return w.finish();
}

int formatPing(char *out, int capacity, const char *code, uint16 peerId) {
	if (!out || capacity <= 0 || !isValidCode(code))
		return 0;
	Writer w = { out, capacity, 0 };
	w.putStr("PING ");
	w.putStr(code);
	w.put(' ');
	w.putUint(peerId);
	w.put('\n');
	return w.finish();
}

int formatBye(char *out, int capacity, const char *code, uint16 peerId) {
	if (!out || capacity <= 0 || !isValidCode(code))
		return 0;
	Writer w = { out, capacity, 0 };
	w.putStr("BYE ");
	w.putStr(code);
	w.put(' ');
	w.putUint(peerId);
	w.put('\n');
	return w.finish();
}

namespace {

// One token of the header line.
struct Token {
	const char *start;
	int len;
};

int tokenize(char *line, Token *tokens, int maxTokens) {
	int count = 0;
	char *p = line;
	while (count < maxTokens) {
		char *space = strchr(p, ' ');
		int len = space ? (int)(space - p) : (int)strlen(p);
		tokens[count].start = p;
		tokens[count].len = len;
		++count;
		if (!space)
			break;
		*space = 0;
		p = space + 1;
		if (*p == 0) // trailing separator means an empty final field
			break;
	}
	return count;
}

bool isControlVerb(const char *token, int len) {
	static const char *const kVerbs[] = { "WELCOME", "DENY", "PONG", "BYE-OK", "KICKED", "CLOSED" };
	for (int i = 0; i < 6; ++i) {
		const char *v = kVerbs[i];
		if ((int)strlen(v) == len && memcmp(token, v, len) == 0)
			return true;
	}
	return false;
}

bool tokenIs(const Token &t, const char *text) {
	return (int)strlen(text) == t.len && memcmp(t.start, text, t.len) == 0;
}

} // namespace

ReceivedKind parseDatagram(const byte *datagram, int datagramLen, const char *ourCode, Received *out) {
	if (!datagram || !out || !ourCode)
		return kRecvBad;
	memset(out, 0, sizeof(Received));
	out->kind = kRecvBad;

	// The relay refuses the same thing, so matching it here keeps a hostile peer
	// from making this client copy a packet the relay would have dropped anyway.
	if (datagramLen <= 0 || datagramLen > kMaxDatagramBytes)
		return kRecvBad;

	const char *newline = (const char *)memchr(datagram, '\n', datagramLen);
	const int headLen = newline ? (int)(newline - (const char *)datagram) : datagramLen;
	if (headLen <= 0 || headLen > kMaxHeaderBytes)
		return kRecvBad;

	// Work on a NUL-terminated copy: the packet is not NUL terminated and the
	// fields have to be inspected individually. tokenize() overwrites the
	// separators, so keep the verbatim line first.
	char line[kMaxHeaderBytes + 1];
	memcpy(line, datagram, headLen);
	line[headLen] = 0;
	const int textLen = headLen < kMaxHeaderBytes ? headLen : kMaxHeaderBytes;
	memcpy(out->text, line, textLen);
	out->text[textLen] = 0;

	Token tokens[6];
	const int count = tokenize(line, tokens, 6);
	const char *verb = tokens[0].start;
	const int verbLen = tokens[0].len;
	const int ourCodeLen = (int)strlen(ourCode);

	if (verbLen == 1 && verb[0] == 'F') {
		// "F <code> <role> <peerId> <type> <seq>" - six fields.
		if (count != 6 || !newline)
			return kRecvBad;
		if (!isValidCode(tokens[1].start, tokens[1].len))
			return kRecvBad;
		if (tokens[1].len != ourCodeLen || memcmp(tokens[1].start, ourCode, ourCodeLen) != 0)
			return kRecvForeign;
		if (tokens[2].len != 1 || (tokens[2].start[0] != 'h' && tokens[2].start[0] != 'g'))
			return kRecvBad;

		HeaderLine ref;
		copyCode(ref.code, tokens[1].start, tokens[1].len);
		ref.roleTag = tokens[2].start[0];
		if (!parsePeerId(tokens[3].start, tokens[3].len, &ref.peerId))
			return kRecvBad;
		if (tokens[4].len != 1 || !isKnownFrameType(tokens[4].start[0]))
			return kRecvBad;
		ref.type = tokens[4].start[0];
		if (!parseSeq(tokens[5].start, tokens[5].len, &ref.seq))
			return kRecvBad;
		// A frame the relay would never have routed to us is treated as malformed
		// instead of trusted: the routing table is not this client's to relax.
		if (!maySendAs(ref.roleTag, ref.type))
			return kRecvBad;

		out->kind = kRecvData;
		out->header = ref;
		out->payload = (const byte *)newline + 1;
		out->payloadLen = datagramLen - headLen - 1;
		return kRecvData;
	}

	if (verbLen == 1 && verb[0] == 'R') {
		// "R <code> roster\n<json>"
		if (count != 3 || !newline)
			return kRecvBad;
		if (!isValidCode(tokens[1].start, tokens[1].len))
			return kRecvBad;
		if (tokens[1].len != ourCodeLen || memcmp(tokens[1].start, ourCode, ourCodeLen) != 0)
			return kRecvForeign;
		if (!tokenIs(tokens[2], "roster"))
			return kRecvBad;
		out->kind = kRecvRoster;
		out->payload = (const byte *)newline + 1;
		out->payloadLen = datagramLen - headLen - 1;
		return kRecvRoster;
	}

	if (isControlVerb(verb, verbLen)) {
		// Relay pushes name our room in field 2, except DENY before a code is
		// known, which uses "-". A real code that is not ours means this packet is
		// for somebody else and must not affect our state at all.
		if (count >= 2 && tokens[1].len >= 4 && tokens[1].start[0] != '-' &&
			(tokens[1].len != ourCodeLen || memcmp(tokens[1].start, ourCode, ourCodeLen) != 0))
			return kRecvForeign;
		out->kind = kRecvControl;
		return kRecvControl;
	}

	return kRecvBad;
}

// ---------------------------------------------------------------- sequence

SeqCounter::SeqCounter() {
	reset();
}

void SeqCounter::reset() {
	for (int i = 0; i < kSeqSlots; ++i)
		_values[i] = 0;
}

int SeqCounter::slotFor(char type) {
	// Same order as the relay's ROUTING table, so the two read side by side.
	switch (type) {
	case 'V':
		return 0;
	case 'A':
		return 1;
	case 'K':
		return 2;
	case 'C':
		return 3;
	case 'I':
		return 4;
	case 'B':
		return 5;
	case 'M':
		return 6;
	case 'X':
		return 7;
	case 'P':
		return 8;
	default:
		return -1;
	}
}

bool SeqCounter::next(char type, uint64 *seqOut) {
	int slot = slotFor(type);
	if (slot < 0 || !seqOut)
		return false;
	// First use of a type is SEQ 0, which matches how the relay counts a fresh
	// participant. Stopping at the ceiling is a request to re-join: continuing
	// past it would reuse a nonce, and reusing a nonce under one key is how GCM
	// stops providing confidentiality at all.
	if (_values[slot] > kMaxSeq)
		return false;
	*seqOut = _values[slot]++;
	return true;
}

// --------------------------------------------------------------- fragments

void writeFragmentHeader(byte *payload, uint32 frameId, uint16 index, uint16 count) {
	payload[0] = (byte)(frameId & 0xff);
	payload[1] = (byte)((frameId >> 8) & 0xff);
	payload[2] = (byte)((frameId >> 16) & 0xff);
	payload[3] = (byte)((frameId >> 24) & 0xff);
	payload[4] = (byte)(index & 0xff);
	payload[5] = (byte)((index >> 8) & 0xff);
	payload[6] = (byte)(count & 0xff);
	payload[7] = (byte)((count >> 8) & 0xff);
}

bool readFragmentHeader(const byte *payload, int payloadLen, uint32 *frameId, uint16 *index, uint16 *count) {
	if (!payload || payloadLen < kFragHeaderBytes)
		return false;
	*frameId = readLE32(payload);
	*index = readLE16(payload + 4);
	*count = readLE16(payload + 6);
	if (*count < 1 || *count > 0xffff || *index >= *count)
		return false;
	return true;
}

int fragmentCount(int plainLen) {
	if (plainLen <= 0)
		return 0;
	return (plainLen + kMaxFragmentBytes - 1) / kMaxFragmentBytes;
}

// --------------------------------------------------------------- reassembly

FrameReassembler::FrameReassembler() {
	for (int i = 0; i < kSlots; ++i) {
		_slots[i].inUse = false;
		_slots[i].poisoned = false;
		_slots[i].frameId = 0;
		_slots[i].count = 0;
		memset(_slots[i].received, 0, sizeof(_slots[i].received));
		_slots[i].buffer = nullptr;
		_slots[i].capacity = 0;
		_slots[i].lastLen = 0;
		_slots[i].dataLen = 0;
	}
}

FrameReassembler::~FrameReassembler() {
	for (int i = 0; i < kSlots; ++i)
		delete[] _slots[i].buffer;
}

void FrameReassembler::reset() {
	for (int i = 0; i < kSlots; ++i) {
		_slots[i].inUse = false;
		_slots[i].poisoned = false;
		_slots[i].count = 0;
		memset(_slots[i].received, 0, sizeof(_slots[i].received));
		_slots[i].lastLen = 0;
		_slots[i].dataLen = 0;
	}
}

FrameReassembler::Slot *FrameReassembler::slotFor(uint32 frameId) {
	Slot *reuse = nullptr;
	for (int i = 0; i < kSlots; ++i) {
		Slot &s = _slots[i];
		if (s.inUse && s.frameId == frameId)
			return &s;
		if (!s.inUse && !reuse)
			reuse = &s;
	}
	if (!reuse) {
		// No free slot: replace the oldest incomplete frame. Video frames arrive
		// in order, so the oldest one is the one whose late fragments are least
		// likely to still show up.
		reuse = &_slots[0];
		for (int i = 1; i < kSlots; ++i) {
			if (_slots[i].frameId < reuse->frameId)
				reuse = &_slots[i];
		}
	}
	reuse->inUse = true;
	reuse->poisoned = false;
	reuse->frameId = frameId;
	reuse->count = 0;
	memset(reuse->received, 0, sizeof(reuse->received));
	reuse->lastLen = 0;
	reuse->dataLen = 0;
	return reuse;
}

bool FrameReassembler::ensureCapacity(Slot *slot, int needed) {
	if (slot->capacity >= needed)
		return true;
	int capacity = needed;
	byte *buffer = new byte[capacity];
	if (!buffer)
		return false;
	if (slot->buffer && slot->capacity > 0)
		memcpy(buffer, slot->buffer, slot->capacity);
	delete[] slot->buffer;
	slot->buffer = buffer;
	slot->capacity = capacity;
	return true;
}

bool FrameReassembler::feed(uint32 frameId, uint16 index, uint16 count, const byte *data, int dataLen, Complete *out) {
	if (!data || dataLen < 0 || count < 1 || count > kMaxFragments || index >= count)
		return false;

	Slot *slot = slotFor(frameId);
	if (slot->poisoned)
		return false;
	if (slot->count != 0 && slot->count != count) {
		// Two different fragment counts for one frameId: nobody can place them, so
		// throw the frame away instead of guessing.
		slot->poisoned = true;
		return false;
	}
	slot->count = count;

	// Uniform-chunk rule: every fragment but the last carries exactly
	// kMaxFragmentBytes, and the last one must be a real, non-empty piece.
	if (dataLen > kMaxFragmentBytes)
		return false;
	if (index != count - 1) {
		if (dataLen != kMaxFragmentBytes) {
			slot->poisoned = true;
			return false;
		}
	} else {
		if (dataLen < 1) {
			// An empty final fragment means the sender padded to a chunk boundary;
			// it should have used one fewer fragment, and accepting it would make
			// the payload size ambiguous.
			slot->poisoned = true;
			return false;
		}
		if (slot->lastLen > 0 && slot->lastLen != dataLen) {
			// Two copies of the last fragment disagree about the length: the frame
			// cannot be sized, so it is dropped rather than guessed at.
			slot->poisoned = true;
			return false;
		}
		slot->lastLen = dataLen;
	}

	const int offset = index * kMaxFragmentBytes;
	if (!ensureCapacity(slot, offset + dataLen)) {
		slot->poisoned = true;
		return false;
	}
	memcpy(slot->buffer + offset, data, dataLen);
	slot->received[index >> 6] |= (uint64)1 << (index & 63);

	// Complete only when every index arrived.
	bool complete = true;
	for (int w = 0; w < kMaxFragments / 64; ++w) {
		const int bits = (int)count - w * 64;
		const uint64 expected = (bits >= 64) ? ~(uint64)0
			: (bits <= 0 ? 0 : (((uint64)1 << bits) - 1));
		if (slot->received[w] != expected) {
			complete = false;
			break;
		}
	}
	if (!complete)
		return false;

	// The uniform-chunk rule is what makes the size computable here: every
	// fragment but the last is kMaxFragmentBytes, so only the last one's length
	// is unknown until it arrives.
	const int total = (count - 1) * kMaxFragmentBytes + slot->lastLen;
	slot->dataLen = total;
	slot->inUse = false;
	slot->poisoned = false;
	memset(slot->received, 0, sizeof(slot->received));
	slot->count = 0;
	slot->lastLen = 0;

	if (out) {
		out->frameId = frameId;
		out->data = slot->buffer;
		out->len = total;
	}
	return true;
}

// ---------------------------------------------------------------- envelope

int wrapPlaintext(byte *out, int capacity, const byte *payload, int payloadLen) {
	if (!out || !payload || payloadLen < 0)
		return 0;
	if (payloadLen + kEnvelopeBytes > capacity)
		return 0;
	out[0] = kEnvelopePlaintext;
	memcpy(out + kEnvelopeBytes, payload, payloadLen);
	return payloadLen + kEnvelopeBytes;
}

int unwrapEnvelope(const byte *payload, int payloadLen, const byte **inner, int *innerLen) {
	if (!payload || payloadLen < 1)
		return -1;
	const uint8 envelope = payload[0];
	if (envelope == kEnvelopePlaintext) {
		if (inner)
			*inner = payload + kEnvelopeBytes;
		if (innerLen)
			*innerLen = payloadLen - kEnvelopeBytes;
		return kEnvelopePlaintext;
	}
	// 0x01 is AES-256-GCM. Reading it without the key machinery would mean
	// treating ciphertext as video, so it is refused here and the caller drops
	// the frame.
	return -1;
}

} // namespace Wire
} // namespace Coop
