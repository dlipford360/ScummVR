/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names are
 * too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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

// The cursor unit uses std::mutex/chrono/vector; the standard headers it
// pulls in collide with the forbidden-symbol macros, the same as every
// other coop file.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-cursor.h"

#include <chrono>
#include <mutex>

namespace Coop {

namespace {

// A peer's pointer stops being drawn this long after its last update:
// a peer that left (or a stream that died) must not leave a circle
// frozen on the screen. Generous next to a 30 Hz sender on purpose -
// an RTT plus a few losses must not blink the overlay.
const int64 kCursorStaleUs = 2000000;

// One entry per peer; the protocol's room sizes fit this with room for
// the M5 4-guest case plus the host.
const int kMaxPeers = 8;

inline int64 nowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Host publish slot: one sample, newest wins (see the header for why).
std::mutex g_pubM;
uint16 g_pubX = 0, g_pubY = 0;
bool g_pubVisible = false;
bool g_pubHave = false;

struct PeerSlot {
	uint32 peerId = 0; // 0 = empty
	uint16 x = 0, y = 0;
	bool visible = false;
	int64 updatedAtUs = 0;
};
std::mutex g_peerM;
PeerSlot g_peers[kMaxPeers];
CursorStats g_stats;

} // namespace

volatile bool g_coopCursorSendWant = false;
volatile bool g_coopCursorDisplayWant = false;

bool encodeCursorPayload(byte *payload, int capacity, uint16 x, uint16 y, bool visible) {
	if (capacity < 5)
		return false;
	payload[0] = (byte)(x & 0xff);
	payload[1] = (byte)(x >> 8);
	payload[2] = (byte)(y & 0xff);
	payload[3] = (byte)(y >> 8);
	payload[4] = visible ? 1 : 0;
	return true;
}

bool decodeCursorPayload(const byte *payload, int len, uint16 *x, uint16 *y, bool *visible) {
	if (len != 5)
		return false;
	*x = (uint16)(payload[0] | (payload[1] << 8));
	*y = (uint16)(payload[2] | (payload[3] << 8));
	*visible = (payload[4] & 1) != 0; // bit1 (aiming) is defined but unused by this backend
	return true;
}

void publishCursorSample(uint16 x, uint16 y, bool visible) {
	std::lock_guard<std::mutex> lock(g_pubM);
	g_pubX = x;
	g_pubY = y;
	g_pubVisible = visible;
	g_pubHave = true;
}

bool grabCursorSample(uint16 *x, uint16 *y, bool *visible) {
	std::lock_guard<std::mutex> lock(g_pubM);
	if (!g_pubHave)
		return false;
	*x = g_pubX;
	*y = g_pubY;
	*visible = g_pubVisible;
	return true;
}

void receivePeerCursor(uint32 peerId, const byte *payload, int len) {
	uint16 x, y;
	bool visible;
	if (!decodeCursorPayload(payload, len, &x, &y, &visible)) {
		std::lock_guard<std::mutex> lock(g_peerM);
		++g_stats.badPayload;
		return;
	}
	std::lock_guard<std::mutex> lock(g_peerM);
	++g_stats.received;
	PeerSlot *slot = nullptr;
	for (PeerSlot &s : g_peers) {
		if (s.peerId == peerId) {
			slot = &s;
			break;
		}
		if (!slot && s.peerId == 0)
			slot = &s; // first empty, if the peer has no slot yet
	}
	if (!slot)
		return; // more peers than kMaxPeers: newest peers lose, oldest keep
	slot->peerId = peerId;
	slot->x = x;
	slot->y = y;
	slot->visible = visible;
	slot->updatedAtUs = nowUs();
}

void snapshotPeerCursors(std::vector<PeerCursor> *out) {
	out->clear();
	const int64 now = nowUs();
	std::lock_guard<std::mutex> lock(g_peerM);
	for (PeerSlot &s : g_peers) {
		if (!s.peerId)
			continue;
		if (now - s.updatedAtUs > kCursorStaleUs) {
			s.peerId = 0;
			++g_stats.expired;
			continue;
		}
		PeerCursor c;
		c.peerId = s.peerId;
		c.x = s.x;
		c.y = s.y;
		c.visible = s.visible;
		out->push_back(c);
	}
}

CursorStats cursorStats() {
	std::lock_guard<std::mutex> lock(g_peerM);
	return g_stats;
}

} // namespace Coop
