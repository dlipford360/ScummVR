/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names are
 * too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
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

// The host's captured frames: the game's own pixels, read off the GPU.
//
// The guide's §6 says capture happens after the blit in
// AndroidVRGraphicsManager::refreshScreen(), through an async readback, and
// that is exactly this shape. Per frame, on the graphics (ScummVM) thread:
// collect the previous frame's readback if its GPU fence has signaled, then
// issue the current frame's glReadPixels into the other of two PBOs. Two
// slots because a single PBO would have to be mapped while the driver might
// still be writing it; with two, the map never races the write. The wait is
// a zero-timeout poll: the graphics thread is never blocked on the GPU here,
// and a frame that has not landed by the next refresh is simply missed and
// counted (in practice the driver finishes long before, and refreshScreen
// already ends in a glFinish).
//
// The readback goes through a fixed 640x480 capture FBO - the M1 encode
// target - because one GL blit does any scaling the game's video mode needs
// and a CPU downscale would not. For a 640x480 game (classic SCUMM, the DOTT
// reference) the blit is a 1:1 copy. Bytes cross to the sending thread
// through the same single-slot mailbox shape the guest render path uses; the
// consumer converts RGBA to the codec's NV12 on its own thread.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_CAPTURE_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_CAPTURE_H

#include "common/types.h"

namespace Coop {

// The M1 encode target, fixed here so the sender and the capture agree
// without either exporting configuration.
enum { kCapWidth = 640, kCapHeight = 480 };
enum { kCapBytes = kCapWidth * kCapHeight * 4 }; // RGBA8

// Set by whoever wants frames (the self-test), read by the graphics thread.
extern volatile bool g_coopCaptureWant;

// Graphics-thread side. Call once per rendered frame: reaps the previous
// readback into the mailbox, then issues this frame's. gameW/gameH is the
// game's own video region (drawn at the origin of the default framebuffer),
// sceneW/sceneH its full size; the blit source is clamped to the framebuffer
// so a mis-declared video mode cannot read outside it.
void captureVideoFrame(int gameW, int gameH, int sceneW, int sceneH);

// Consumer side (the sending thread). Copies the newest frame not yet taken
// into dst, which must hold exactly kCapBytes. Row 0 is the BOTTOM of the
// image - the GL convention - and converting to top-first NV12 means one
// flipped row walk, which is the consumer's job, not this unit's.
// capturedAtUs gets the wall clock (steady_clock us) at the readback's copy.
bool grabCapturedFrame(byte *dst, int64 *capturedAtUs);

struct CaptureStats {
	int issues;      // readbacks issued
	int reaps;       // readbacks collected into the mailbox
	int missed;      // reaps attempted with the fence un-signaled
	int mapFails;    // signaled but glMapBufferRange refused
};

// Snapshot of the counters, for whoever requested the capture to report.
CaptureStats captureStats();

} // namespace Coop

#endif
