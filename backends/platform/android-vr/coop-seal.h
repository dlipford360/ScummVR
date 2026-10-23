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

// Payload sealing: AES-256-GCM, vendored, no OpenSSL and no JNI. AES-256 because
// the room media key is 32 bytes; GCM because the relay's contract is that it
// forwards payloads it cannot read *and* cannot rewrite, which needs the frame
// header as associated data.
//
// The relay's reference implementation is coop-relay/src/mediaCrypto.ts and its
// test file is the conformance checklist for this port. Two properties here are
// load-bearing rather than stylistic:
//
//   - sealPayload() builds the AAD itself, by calling the one header formatter in
//     the codebase (Wire::formatForwardedHeader). A caller-supplied AAD string is
//     how a port silently fails to decrypt: the bytes have to match the header the
//     recipient receives, not the "D" line the sender put on the wire.
//   - the nonce is (reserved byte, frame type, peerId, SEQ) and is never
//     transmitted, so uniqueness rests entirely on never resetting a SEQ counter
//     under one key. SeqCounter enforces the ceiling; a refusal here is not a
//     glitch to retry.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_SEAL_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_SEAL_H

#include "common/types.h"
#include "backends/platform/android-vr/coop-wire.h"

namespace Coop {
namespace Seal {

const int kKeyBytes = 32;
const int kKeyHexChars = 64; // the control plane hands the key over as hex
const int kNonceBytes = 12;

// What sealing costs a datagram: envelope byte plus the GCM tag. The fragment
// budget in Wire already subtracts this from the relay's payload cap.
const int kOverheadBytes = Wire::kEnvelopeBytes + Wire::kTagBytes;
const int kMaxPlaintextBytes = Wire::kMaxPayloadBytes - kOverheadBytes;

// Reads the `mediaKey` string the control plane returned: exactly 64 hex
// characters. Rejects anything else rather than truncating, because a half-read
// key produces ciphertext nobody can open. `key` is wiped on every failure path.
bool keyFromHex(const char *hex, byte key[kKeyBytes]);

// The nonce, for callers that need to inspect it. Inputs are validated the same
// way the relay's nonceFor() does: peerId 0..0xffff, SEQ up to 2^53-1.
bool nonceFor(byte nonce[kNonceBytes], char type, uint16 peerId, uint64 seq);

// Key-dependent material, built once per media key instead of once per frame.
//
// A room sends hundreds of fragments a second, and the per-call work of expanding
// the AES key schedule and deriving the GHASH subkey is pure waste next to the
// per-block cost. It also keeps the GHASH tables warm in L1, which is what makes
// the portable code path fast enough for a phone.
//
// The tables are the one deliberate space/speed trade here. GHASH is linear in its
// input, so a block multiply is 16 table XORs (see buildTables), and the table
// index comes from ciphertext and header bytes - public data on the wire - not
// from the key. The AES S-box lookups stay key-indexed, which is why this path is
// described as cache-soft in the guide rather than as constant time.
class Context {
public:
	Context();
	~Context();

	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	// Installs a key: expands the schedule, derives H, fills the GHASH tables.
	// Callers should keep their own copy wiped once this returns.
	bool setKey(const byte key[kKeyBytes]);
	bool setFromHex(const char *mediaKeyHex);

	bool hasKey() const { return _ready; }

	// Zeroes the schedule, H and the tables. Called by the destructor, and by
	// whoever is done with the key.
	void clear();

	// Recomputes every derived value the slow, obviously-correct way and compares.
	// Costs a few milliseconds, so it belongs where a key is installed rather than
	// on the frame path: hand-written field arithmetic earns the right to be used
	// by being checked against itself. Returns false if anything disagrees, and the
	// caller must then treat this context as unusable.
	bool selfCheck() const;

private:
	// The two envelope functions below are the only callers allowed to reach inside
	// for the schedule and the tables: keeping them private is what stops game code
	// from encrypting with a half-built context.
	friend int sealPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
		const byte *plaintext, int plaintextLen);
	friend int openPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
		const byte *payload, int payloadLen, bool allowPlaintext);

	void buildTables();

	// GHASH: Y = (Y XOR block) * H, table path.
	void ghashBlock(byte *y, const byte *block) const;
	// GHASH over a byte run, zero padded to the block boundary like the spec.
	void ghashRun(byte *y, const byte *data, int len) const;

	// Keystream XOR. GCTR is its own inverse; `ctr` is advanced past every block
	// used, so callers pass the first counter block and reuse the buffer.
	void gctr(byte *ctr, const byte *in, byte *out, int len) const;

	// Recomputes a tag. Shared by seal and open so the tag rule cannot drift
	// between the two directions.
	void computeTag(const byte *j0, const char *aad, int aadLen, const byte *ct, int ctLen, byte *tag) const;

	static const int kRoundKeyBytes = 240; // AES-256: 14 rounds, 15 block-sized round keys
	static const int kBlockBytes = 16;

	byte _rk[kRoundKeyBytes];
	byte _h[kBlockBytes];
	// _table[which][position][nibble] is a whole block. which 0 holds the low
	// nibble at `position`, which 1 holds the high nibble shifted up by 4 bits.
	byte _table[2][kBlockBytes][16][kBlockBytes];
	bool _ready;
};

// Writes envelope byte 0x01, then ciphertext of `plaintextLen`, then the 16-byte
// tag. Returns the total length, or 0 when the output would not fit, the header
// reference is unusable, the context has no key, or the SEQ is beyond the ceiling.
int sealPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
	const byte *plaintext, int plaintextLen);

// Reads a payload as received (envelope byte included). Returns the plaintext
// length, or -1 for anything that must be dropped: unknown envelope, a sealed
// payload while no plaintext is allowed, a truncated payload, or a tag mismatch
// - which includes a header that was rewritten in transit. Never produce partial
// output: a video frame that fails authentication is not worth decoding.
int openPayload(const Context &ctx, byte *out, int capacity, const Wire::HeaderLine &ref,
	const byte *payload, int payloadLen, bool allowPlaintext);

// First envelope byte, or -1 for an empty payload. Lets a caller decide whether a
// room is sealing before it has a key.
int envelopeKind(const byte *payload, int payloadLen);

} // namespace Seal
} // namespace Coop

#endif
