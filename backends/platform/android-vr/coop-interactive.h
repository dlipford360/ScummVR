/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
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

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_INTERACTIVE_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_INTERACTIVE_H

#include "common/scummsys.h"

#include <atomic>

namespace Coop {

// The in-VR join path: a JOIN ROOM button, a gaze-typed room string, and a
// LEAVE ROOM button. This header is the whole surface between the two halves
// that must never meet otherwise - the VR thread (which owns GL and the
// controller ray) and the coop service thread (which owns the Session and the
// Transport, both of which are thread-affine in practice).
//
//   VR thread                 service thread (in coop-selftest.cpp)
//   ---------                 -----------------------------------
//   requestJoinRoom(s)  --->  control plane, HELLO/WELCOME, then
//                             the guest media loop: 'V' decode+display,
//                             'A' audio, 'C' cursors both ways
//   requestLeaveRoom()  --->  BYE, teardown, back to idle
//                             ^
//   reads g_coopUi*  <----    publishes state and one-line status
//
// This deliberately lives next to the self-test for now: the framing helpers
// it reuses (fragmenting AUs, reassembly, retransmit) are statics there, and
// both files are expected to become the product entry point together. When
// they move, they move as a unit.

enum UiState {
	kUiIdle = 0,   // no room; JOIN/CREATE ROOM is live
	kUiJoining,    // control-plane/socket handshake in flight (either role)
	kUiJoined      // in a room; LEAVE ROOM is live
};

// The relay the interactive UI dials. The typed string is room-only by
// design: carrying a URL on a gaze keyboard is not a usability idea worth
// shipping, and the LAN test relay is what this whole build is for. A
// self-test marker's relay= key still configures the test path it runs.
#define COOP_UI_RELAY_BASE "https://192.168.0.25:19091"

// VR thread -> service thread. The line is the typed room string, already
// parsed into its parts (the UI validates before sending): `code` is the
// room name, `invite` the join secret. Called from the VR thread; the
// service wakes on the request and consumes it.
void requestJoinRoom(const char *code, const char *invite);

// VR thread -> service thread, create path: host a NEW open room named
// `name` (validated by the UI as [A-Z0-9]{1,20}). This device becomes the
// video/audio host exactly like the marker self-test's role=host run; the
// name is what the other headset types into JOIN ROOM.
void requestCreateRoom(const char *name);

// Ask whatever coop run is live - the interactive service or a marker
// self-test - to end now. Transport close sends BYE; the relay drops the
// participant. Safe to press repeatedly, and a no-op when nothing is running.
void requestLeaveRoom();

// Published by the service thread for the VR thread to draw. The atomics are
// plain relaxed stores: a torn UI frame is impossible (word-sized), and a
// one-frame-stale status string is invisible.
extern std::atomic<int> g_coopUiState;
extern std::atomic<bool> g_coopUiLeaveWanted; // also read by the marker loop
extern char g_coopUiStatus[96];               // one-line state for the panel
extern char g_coopUiRoomLine[64];             // "ROOM ABCDE - 2 peers", or ""

// A one-shot notice the VR thread toasts under the screen: "HOST LEFT THE
// ROOM". The service writes the text, then bumps the sequence; the VR thread
// bakes on the sequence edge and fades the band out. Write-then-bump and
// read-after-seeing-the-bump are seq_cst on both sides, so the VR thread
// never toasts a half-written string.
extern std::atomic<unsigned> g_coopUiNoticeSeq;
extern char g_coopUiNotice[96];

} // namespace Coop

#endif
