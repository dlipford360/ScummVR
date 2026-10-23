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

// AES-256-GCM, written out by hand. The alternatives were rejected on purpose:
// the fork carries no OpenSSL for the Android VR target, JNI to javax.crypto
// would tie the wire layer to the Java platform module and stop it building (and
// being testable) off-target, and a KCM/socket-level option does not exist for
// plain UDP sends here.
//
// What is actually needed is smaller than a general crypto library:
//
//   - only the AES *encryption* direction. GCM uses the block cipher for the CTR
//     keystream, for H = E(0^128) and for E(J0); it never decrypts a block.
//   - only AES-256, because the room media key is 32 bytes.
//   - one GHASH multiply, done with the shift-and-xor form from SP 800-38D rather
//     than tables. Table-driven GHASH would want a per-key table rebuilt for each
//     fragment, and a few thousand 128-iteration multiplies per second is not what
//     this pipeline is limited by. On ARMv8 the vmull-based carry-less multiply is
//     the obvious place to speed this up later; correctness first.
//
// Portability notes: no allocation, no exceptions, no <cstdio>, no static state,
// and every intermediate is stack-sized, so this runs from the encoder thread and
// from the receive thread without touching the heap.

#include "common/types.h"
#include "backends/platform/android-vr/coop-seal.h"

#include <cstring>

namespace Coop {
namespace Seal {

namespace {

const int kBlockBytes = 16;
const int kRounds = 14;

// Generated from the field inverse plus the affine transform of FIPS-197 5.1.9,
// not typed by hand.
const byte kSbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

// Rcon[i] for i = 0..10; only 1..7 are used by AES-256.
const byte kRcon[11] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };

void wipe(void *p, int len) {
	// Through a volatile pointer so the compiler may not remove this as a write to
	// a dead local. Best effort, like every other zeroisation of key material.
	volatile byte *q = (volatile byte *)p;
	while (len > 0) {
		*q = 0;
		++q;
		--len;
	}
}

uint32 readBE32(const byte *p) {
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3];
}

void writeBE64(byte *p, uint64 v) {
	for (int i = 7; i >= 0; --i) {
		p[i] = (byte)(v & 0xff);
		v >>= 8;
	}
}

byte xtime(byte v) {
	return (byte)((v << 1) ^ ((v & 0x80) ? 0x1b : 0x00));
}

uint32 subWord(uint32 w) {
	return ((uint32)kSbox[(w >> 24) & 0xff] << 24) | ((uint32)kSbox[(w >> 16) & 0xff] << 16) |
		((uint32)kSbox[(w >> 8) & 0xff] << 8) | (uint32)kSbox[w & 0xff];
}

// Expands a 32-byte key into 15 round keys, stored as 16 bytes each in the same
// column-major order the state array uses below. That is what makes addRoundKey()
// a flat XOR: word w[4r+c] of the expansion is the round key byte pair for row r,
// column c, and its four bytes sit at rk[16r + 4c .. 16r + 4c + 3].
// `rk` must be Context::kRoundKeyBytes (240) bytes long. The schedule is the
// secret the whole GCM path runs on, so it is wiped here as soon as it is copied
// out.
void expandKey(const byte key[32], byte *rk) {
	uint32 w[60];
	for (int i = 0; i < 8; ++i)
		w[i] = readBE32(key + 4 * i);

	for (int i = 8; i < 60; ++i) {
		uint32 temp = w[i - 1];
		if (i % 8 == 0) {
			temp = (temp << 8) | (temp >> 24); // RotWord
			temp = subWord(temp);
			temp ^= (uint32)kRcon[i / 8] << 24;
		} else if (i % 8 == 4) {
			temp = subWord(temp);
		}
		w[i] = w[i - 8] ^ temp;
	}

	for (int i = 0; i < 60; ++i) {
		rk[4 * i + 0] = (byte)(w[i] >> 24);
		rk[4 * i + 1] = (byte)(w[i] >> 16);
		rk[4 * i + 2] = (byte)(w[i] >> 8);
		rk[4 * i + 3] = (byte)(w[i]);
	}
	wipe(w, sizeof(w));
}

void addRoundKey(byte *s, const byte *rk) {
	for (int i = 0; i < kBlockBytes; ++i)
		s[i] ^= rk[i];
}

void subBytes(byte *s) {
	for (int i = 0; i < kBlockBytes; ++i)
		s[i] = kSbox[s[i]];
}

// State bytes are flat column-major: index 4*col + row.
void shiftRows(byte *s) {
	byte t[kBlockBytes];
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col)
			t[4 * col + row] = s[4 * ((col + row) & 3) + row];
	}
	memcpy(s, t, kBlockBytes);
}

void mixColumns(byte *s) {
	for (int col = 0; col < 4; ++col) {
		byte *p = s + 4 * col;
		// b_i = 2a_i ^ 3a_{i+1} ^ a_{i+2} ^ a_{i+3}, written with one common term:
		// a_i ^ xtime(a_i ^ a_{i+1}) ^ t with t = a0^a1^a2^a3.
		const byte a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
		const byte t = (byte)(a0 ^ a1 ^ a2 ^ a3);
		p[0] = (byte)(a0 ^ xtime((byte)(a0 ^ a1)) ^ t);
		p[1] = (byte)(a1 ^ xtime((byte)(a1 ^ a2)) ^ t);
		p[2] = (byte)(a2 ^ xtime((byte)(a2 ^ a3)) ^ t);
		p[3] = (byte)(a3 ^ xtime((byte)(a3 ^ a0)) ^ t);
	}
}

void encryptBlock(const byte *rk, const byte *in, byte *out) {
	byte s[kBlockBytes];
	memcpy(s, in, kBlockBytes);

	addRoundKey(s, rk);
	for (int round = 1; round <= kRounds; ++round) {
		subBytes(s);
		shiftRows(s);
		if (round != kRounds)
			mixColumns(s);
		addRoundKey(s, rk + kBlockBytes * round);
	}
	memcpy(out, s, kBlockBytes);
}

void incrementCounter(byte *ctr) {
	for (int i = 15; i >= 12; --i) {
		++ctr[i];
		if (ctr[i])
			break;
	}
}

// Multiply in GF(2^128) the way SP 800-38D spells it out: right shift with the
// R = 0xe1||0^120 reduction. This is the slow, obviously-correct path: the frame
// path uses the Context's tables, and selfCheck() compares the two.
//
// Written without data-dependent branches. The select is a mask, not an `if`, so
// the multiply's control flow does not depend on the value of the secret subkey H
// or on the round-key material that produced it.
void gfmulReference(const byte *x, const byte *y, byte *out) {
	byte z[kBlockBytes];
	byte v[kBlockBytes];
	memset(z, 0, kBlockBytes);
	memcpy(v, y, kBlockBytes);

	for (int i = 0; i < 128; ++i) {
		const byte mask = (byte)(-(int)((x[i >> 3] >> (7 - (i & 7))) & 1));
		for (int j = 0; j < kBlockBytes; ++j)
			z[j] = (byte)(z[j] ^ (v[j] & mask));

		const byte lsbMask = (byte)(-(int)(v[kBlockBytes - 1] & 1));
		for (int j = kBlockBytes - 1; j > 0; --j)
			v[j] = (byte)((v[j] >> 1) | (v[j - 1] << 7));
		v[0] = (byte)((v[0] >> 1) ^ (0xe1 & lsbMask));
	}
	memcpy(out, z, kBlockBytes);
	wipe(z, sizeof(z));
	wipe(v, sizeof(v));
}

// Multiply a block by the field element x, which is the rightshift of
// SP 800-38D Section 1. Used to build the GHASH tables: (a * x^b) * H equals
// rightshift^b(a * H), so one multiply per table row is enough.
void mulByX(byte *v) {
	const byte lsbMask = (byte)(-(int)(v[kBlockBytes - 1] & 1));
	for (int j = kBlockBytes - 1; j > 0; --j)
		v[j] = (byte)((v[j] >> 1) | (v[j - 1] << 7));
	v[0] = (byte)((v[0] >> 1) ^ (0xe1 & lsbMask));
}

bool constTimeEquals(const byte *a, const byte *b, int len) {
	byte diff = 0;
	for (int i = 0; i < len; ++i)
		diff |= (byte)(a[i] ^ b[i]);
	return diff == 0;
}

// One hex character, or -1. No table, because this runs on the control-plane
// string once per join, not on the frame path.
int hexNibble(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// Builds the J0 block (nonce || 0^31 || 1) for a frame reference.
bool counterBase(const Wire::HeaderLine &ref, byte *j0) {
	byte nonce[kNonceBytes];
	if (!nonceFor(nonce, ref.type, ref.peerId, ref.seq))
		return false;
	memcpy(j0, nonce, kNonceBytes);
	j0[12] = 0;
	j0[13] = 0;
	j0[14] = 0;
	j0[15] = 1;
	return true;
}

} // namespace

// --------------------------------------------------------------- key context
//
// Everything below derives from the media key, so it runs once per install rather
// than once per frame. The tables turn a GHASH block multiply from 128 rounds of
// shift-and-xor into 32 table XORs, which is what makes the portable path cheap
// enough to run at video fragment rates on a phone.

Context::Context() {
	memset(_rk, 0, sizeof(_rk));
	memset(_h, 0, sizeof(_h));
	memset(_table, 0, sizeof(_table));
	_ready = false;
}

Context::~Context() {
	clear();
}

void Context::clear() {
	wipe(_rk, sizeof(_rk));
	wipe(_h, sizeof(_h));
	wipe(_table, sizeof(_table));
	_ready = false;
}

bool Context::setKey(const byte key[kKeyBytes]) {
	if (!key)
		return false;

	expandKey(key, _rk);

	// H = E(0^128), the GHASH subkey.
	memset(_h, 0, sizeof(_h));
	encryptBlock(_rk, _h, _h);

	buildTables();
	_ready = true;
	return true;
}

bool Context::setFromHex(const char *mediaKeyHex) {
	byte key[kKeyBytes];
	if (!keyFromHex(mediaKeyHex, key)) {
		wipe(key, sizeof(key));
		return false;
	}
	const bool ok = setKey(key);
	wipe(key, sizeof(key));
	if (!ok)
		clear();
	return ok;
}

void Context::buildTables() {
	// Multiplication in GF(2^128) is linear over GF(2) in each operand, so a block
	// X factors as the XOR of its nibbles placed at their byte positions:
	//
	//   X * H = XOR over p of ( table[1][p][X[p] >> 4] XOR table[0][p][X[p] & 0xf] )
	//
	// Each entry is the product of one placed nibble with H, and one multiply per
	// byte position is enough because the other 15 entries are XORs of those.
	//
	// The bit order is the part that breaks hand-written GCM ports, so it is worth
	// spelling out. SP 800-38D numbers the bits of a block from the left, with the
	// *leftmost* bit as the constant term: byte p carries the exponents 8p..8p+7
	// running from its most significant bit down to its least significant one. So
	// the memory bit 2^b of byte p is the field term x^(8p + 7 - b), and multiplying
	// a placed bit by x walks it down through memory and across into the next byte.
	// That is why the chain below starts at the top bit of the byte, not the bottom:
	// building it from 0x01 upward would put four of the eight products in the wrong
	// byte. selfCheck() compares all 512 entries against the slow multiply, and it
	// is exactly what caught that mistake.
	for (int p = 0; p < kBlockBytes; ++p) {
		byte unit[kBlockBytes];
		memset(unit, 0, sizeof(unit));
		unit[p] = 0x80; // highest memory bit = lowest exponent at this position

		// chain[k] is the product of the byte's bit (7-k) with H, so each step is a
		// multiply by x.
		byte chain[8][kBlockBytes];
		gfmulReference(unit, _h, chain[0]);
		for (int k = 1; k < 8; ++k) {
			memcpy(chain[k], chain[k - 1], kBlockBytes);
			mulByX(chain[k]);
		}

		for (int n = 0; n < 16; ++n) {
			byte *hi = _table[1][p][n]; // high nibble: memory bits 4..7 -> chain 3..0
			byte *lo = _table[0][p][n]; // low nibble:  memory bits 0..3 -> chain 7..4
			memset(hi, 0, kBlockBytes);
			memset(lo, 0, kBlockBytes);
			for (int j = 0; j < 4; ++j) {
				if (!(n & (1 << j)))
					continue;
				for (int c = 0; c < kBlockBytes; ++c) {
					hi[c] ^= chain[3 - j][c];
					lo[c] ^= chain[7 - j][c];
				}
			}
		}
		wipe(chain, sizeof(chain));
	}
}

void Context::ghashBlock(byte *y, const byte *block) const {
	byte input[kBlockBytes];
	for (int i = 0; i < kBlockBytes; ++i)
		input[i] = (byte)(y[i] ^ block[i]);

	memset(y, 0, kBlockBytes);
	for (int p = 0; p < kBlockBytes; ++p) {
		const byte *lo = _table[0][p][input[p] & 0x0f];
		const byte *hi = _table[1][p][input[p] >> 4];
		for (int j = 0; j < kBlockBytes; ++j)
			y[j] = (byte)(y[j] ^ lo[j] ^ hi[j]);
	}
	// The indices above are ciphertext and header bytes, which are public on the
	// wire; only the table *contents* are secret. So the lookups below are not
	// reading secret-dependent addresses.
	wipe(input, sizeof(input));
}

void Context::ghashRun(byte *y, const byte *data, int len) const {
	int off = 0;
	for (; len - off >= kBlockBytes; off += kBlockBytes)
		ghashBlock(y, data + off);
	if (len > off) {
		byte block[kBlockBytes];
		memset(block, 0, sizeof(block));
		memcpy(block, data + off, len - off);
		ghashBlock(y, block);
	}
}

void Context::gctr(byte *ctr, const byte *in, byte *out, int len) const {
	byte stream[kBlockBytes];
	int off = 0;
	for (; off < len; off += kBlockBytes) {
		encryptBlock(_rk, ctr, stream);
		incrementCounter(ctr);
		const int n = (len - off < kBlockBytes) ? (len - off) : kBlockBytes;
		for (int i = 0; i < n; ++i)
			out[off + i] = (byte)(in[off + i] ^ stream[i]);
	}
	wipe(stream, sizeof(stream));
}

void Context::computeTag(const byte *j0, const char *aad, int aadLen, const byte *ct, int ctLen,
	byte *tag) const {
	byte y[kBlockBytes];
	memset(y, 0, sizeof(y));
	ghashRun(y, (const byte *)aad, aadLen);
	ghashRun(y, ct, ctLen);

	byte lens[kBlockBytes];
	writeBE64(lens, (uint64)aadLen * 8u);
	writeBE64(lens + 8, (uint64)ctLen * 8u);
	ghashBlock(y, lens);

	byte ej0[kBlockBytes];
	encryptBlock(_rk, j0, ej0);
	for (int i = 0; i < Wire::kTagBytes; ++i)
		tag[i] = (byte)(y[i] ^ ej0[i]);

	wipe(y, sizeof(y));
	wipe(ej0, sizeof(ej0));
}

bool Context::selfCheck() const {
	if (!_ready)
		return false;

	// H must still be E(0^128) under the installed schedule. Recompute it from a
	// throwaway copy: a context whose round keys and subkey disagree would produce
	// tags nobody can open.
	byte h[kBlockBytes];
	memset(h, 0, sizeof(h));
	encryptBlock(_rk, h, h);
	if (!constTimeEquals(h, _h, sizeof(h))) {
		wipe(h, sizeof(h));
		return false;
	}
	wipe(h, sizeof(h));

	// Every table entry against the slow multiply. 512 multiplies, which is a few
	// milliseconds here and never on the frame path.
	for (int which = 0; which < 2; ++which) {
		for (int p = 0; p < kBlockBytes; ++p) {
			for (int n = 0; n < 16; ++n) {
				byte unit[kBlockBytes];
				memset(unit, 0, sizeof(unit));
				unit[p] = (byte)(n << ((which == 0) ? 0 : 4));

				byte expected[kBlockBytes];
				gfmulReference(unit, _h, expected);
				const bool ok = constTimeEquals(expected, _table[which][p][n], kBlockBytes);
				wipe(expected, sizeof(expected));
				wipe(unit, sizeof(unit));
				if (!ok)
					return false;
			}
		}
	}
	return true;
}

bool keyFromHex(const char *hex, byte key[kKeyBytes]) {
	if (!hex || !key)
		return false;

	// Length first, before any parsing: the input comes off the network, and a
	// short string must be rejected without reading past its terminator. Exactly
	// 64 characters too - trailing junk means the caller misread the response, and
	// a silently truncated key is undetectable later.
	for (int i = 0; i < 2 * kKeyBytes; ++i) {
		if (hex[i] == 0)
			return false;
	}
	if (hex[2 * kKeyBytes] != 0)
		return false;

	byte parsed[kKeyBytes];
	memset(parsed, 0, sizeof(parsed));
	for (int i = 0; i < kKeyBytes; ++i) {
		const int hi = hexNibble(hex[2 * i]);
		const int lo = hexNibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			wipe(parsed, sizeof(parsed));
			return false;
		}
		parsed[i] = (byte)((hi << 4) | lo);
	}

	memcpy(key, parsed, kKeyBytes);
	wipe(parsed, sizeof(parsed));
	return true;
}

bool nonceFor(byte nonce[kNonceBytes], char type, uint16 peerId, uint64 seq) {
	if (!nonce)
		return false;
	// Same limits the relay's nonceFor() enforces; a value past the ceiling would
	// mean the header itself is not something the relay could have forwarded.
	if (seq > Wire::kMaxSeq)
		return false;

	nonce[0] = 0;
	nonce[1] = (byte)type;
	nonce[2] = (byte)(peerId >> 8);
	nonce[3] = (byte)(peerId & 0xff);
	for (int i = 7; i >= 0; --i) {
		nonce[4 + i] = (byte)(seq & 0xff);
		seq >>= 8;
	}
	return true;
}

int envelopeKind(const byte *payload, int payloadLen) {
	if (!payload || payloadLen < 1)
		return -1;
	return (int)payload[0];
}

int sealPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
	const byte *plaintext, int plaintextLen) {
	if (!out || !plaintext || plaintextLen < 0)
		return 0;
	if (!ctx.hasKey())
		return 0;
	// The relay's payload cap applies to the sealed form, so the plaintext that
	// fits one datagram is the cap minus envelope and tag. Sending more than that
	// is a bug in the caller's fragmentation, not something to truncate here.
	if (plaintextLen > kMaxPlaintextBytes)
		return 0;
	if (plaintextLen + kOverheadBytes > capacity)
		return 0;

	// AAD built by the one formatter in this tree, never handed in by the caller:
	// it has to be the line the recipient receives, which is what makes the tag
	// cover the sender's identity.
	char aad[Wire::kMaxHeaderBytes];
	const int aadLen =
		Wire::formatForwardedHeader(aad, sizeof(aad), ref.code, ref.roleTag, ref.peerId, ref.type, ref.seq);
	if (aadLen == 0)
		return 0;

	byte j0[kBlockBytes];
	if (!counterBase(ref, j0))
		return 0;

	out[0] = Wire::kEnvelopeSealed;
	byte ctr[kBlockBytes];
	memcpy(ctr, j0, kBlockBytes);
	incrementCounter(ctr);
	ctx.gctr(ctr, plaintext, out + Wire::kEnvelopeBytes, plaintextLen);

	byte tag[Wire::kTagBytes];
	ctx.computeTag(j0, aad, aadLen, out + Wire::kEnvelopeBytes, plaintextLen, tag);
	memcpy(out + Wire::kEnvelopeBytes + plaintextLen, tag, Wire::kTagBytes);

	wipe(ctr, sizeof(ctr));
	wipe(j0, sizeof(j0));
	wipe(tag, sizeof(tag));
	return Wire::kEnvelopeBytes + plaintextLen + Wire::kTagBytes;
}

int openPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
	const byte *payload, int payloadLen, bool allowPlaintext) {
	if (!out || !payload)
		return -1;

	const int kind = envelopeKind(payload, payloadLen);
	if (kind < 0)
		return -1;

	if (kind == Wire::kEnvelopePlaintext) {
		if (!allowPlaintext)
			return -1;
		const int plainLen = payloadLen - Wire::kEnvelopeBytes;
		if (plainLen > capacity)
			return -1;
		if (plainLen > 0)
			memcpy(out, payload + Wire::kEnvelopeBytes, plainLen);
		return plainLen;
	}

	if (kind != Wire::kEnvelopeSealed)
		return -1;
	if (!ctx.hasKey())
		return -1;
	// Envelope byte plus the tag, and nothing else, is the smallest sealed frame.
	// The relay accepts an empty GCM payload, so an exact-size frame is legal.
	if (payloadLen < Wire::kEnvelopeBytes + Wire::kTagBytes)
		return -1;

	const int ctLen = payloadLen - Wire::kEnvelopeBytes - Wire::kTagBytes;
	if (ctLen > capacity)
		return -1;

	char aad[Wire::kMaxHeaderBytes];
	const int aadLen =
		Wire::formatForwardedHeader(aad, sizeof(aad), ref.code, ref.roleTag, ref.peerId, ref.type, ref.seq);
	if (aadLen == 0)
		return -1;

	byte j0[kBlockBytes];
	if (!counterBase(ref, j0))
		return -1;

	// Authenticate first, decrypt second: nothing downstream may see plaintext
	// from a frame whose header was rewritten in transit.
	byte expected[Wire::kTagBytes];
	ctx.computeTag(j0, aad, aadLen, payload + Wire::kEnvelopeBytes, ctLen, expected);
	const bool ok = constTimeEquals(expected, payload + payloadLen - Wire::kTagBytes, Wire::kTagBytes);
	wipe(expected, sizeof(expected));

	if (!ok) {
		wipe(j0, sizeof(j0));
		return -1;
	}

	byte ctr[kBlockBytes];
	memcpy(ctr, j0, kBlockBytes);
	incrementCounter(ctr);
	if (ctLen > 0)
		ctx.gctr(ctr, payload + Wire::kEnvelopeBytes, out, ctLen);

	wipe(ctr, sizeof(ctr));
	wipe(j0, sizeof(j0));
	return ctLen;
}

} // namespace Seal
} // namespace Coop
