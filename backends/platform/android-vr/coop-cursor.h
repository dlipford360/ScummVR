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

// The host's VR pointer, live on the guest's screen: the M2 'C' channel.
//
// The pointer circle the host wearer points with must not be baked into
// the video - it travels as 'C' frames so the guest draws it over the
// decoded quad at its own scale, staying live even while the video lags
// (client guide §6/§7; payload spec: the relay README's cursor channel).
//
// The source is the VR thread's ray-plane hit - the same intersection
// that draws the dot in android-vr-main.cpp's processInput. It is NOT
// AndroidGraphicsManager::getMousePosition(): the VR path pushes mouse
// events straight into ScummVM's queue, so that getter is never fed on
// this backend. A plan that assumed it was has been corrected (§7).
//
// Coordinates are normalised 0…10000 against the *streamed image* - the
// whole captured backbuffer. In this VR backend the video covers the
// whole screen quad (the letterbox is baked into the pixels), and the
// host's ray hit is already normalised to that same backbuffer by
// construction, so host u,v maps 1:1 onto the guest's quad regardless of
// either headset's screen scale.
//
// Both directions cross threads through latest-value-overwrite mailboxes
// - the policy the video display mailbox established, because a live
// pointer has no history: only the newest sample is relevant, and
// queueing would add latency. Host: VR thread publishes, the self-test
// sender grabs at ~30 Hz. Guest: the self-test receiver stores per
// peer, the VR draw thread snapshots. Entries go stale on their own
// (~2 s without updates) so a departed peer leaves no stuck circle.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_CURSOR_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_CURSOR_H

#include "common/types.h"

#include <vector>

namespace Coop {

// 'C' payload (relay README): uint16le x, uint16le y (0…10000, normalised
// to the streamed image), uint8 flags (bit0 visible). Kept in one place
// so sender and receiver cannot drift apart.
bool encodeCursorPayload(byte *payload, int capacity, uint16 x, uint16 y, bool visible);
bool decodeCursorPayload(const byte *payload, int len, uint16 *x, uint16 *y, bool *visible);

// True while the host's self-test wants 'C' frames produced; the VR
// thread polls it before publishing (a plain bool: a torn or late read
// costs one sample of a 30 Hz stream).
extern volatile bool g_coopCursorSendWant;
// True once a guest has received any 'C' frame, so the VR thread knows
// to draw remote pointers at all (cleared when the self-test ends).
extern volatile bool g_coopCursorDisplayWant;

// Host side. publishCursorSample runs on the VR thread (processInput);
// grabCursorSample on the self-test sender thread, once per send tick.
// visible=false means "the ray is off the screen rect" - it must still
// be published, so the guest's overlay disappears instead of sticking.
void publishCursorSample(uint16 x, uint16 y, bool visible);
bool grabCursorSample(uint16 *x, uint16 *y, bool *visible);

// Guest side. receivePeerCursor runs on the self-test receive thread;
// snapshotPeerCursors on the VR draw thread, filling one entry per peer
// that is not stale. Stale entries are dropped and counted.
void receivePeerCursor(uint32 peerId, const byte *payload, int len);

struct PeerCursor {
	uint32 peerId;
	uint16 x, y;
	bool visible;
};
void snapshotPeerCursors(std::vector<PeerCursor> *out);

struct CursorStats {
	int received;    // 'C' payloads accepted for any peer
	int badPayload;  // payloads with the wrong length (never trusted)
	int expired;     // peer entries dropped by the staleness cutoff
};
CursorStats cursorStats();

} // namespace Coop

#endif
