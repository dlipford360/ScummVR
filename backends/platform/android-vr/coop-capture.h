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
// The guide's §6 says capture is an async readback of the composited
// backbuffer taken before the cursor is drawn into it - AndroidVRGraphicsManager
// overrides the OpenGL manager's preCursorHook for exactly that. Per frame, on
// the graphics (ScummVM) thread: collect the previous frame's readback if its
// GPU fence has signaled, then issue the current frame's glReadPixels into the
// other of two PBOs. Two slots because a single PBO would have to be mapped
// while the driver might still be writing it; with two, the map never races
// the write. The wait is a zero-timeout poll: the graphics thread is never
// blocked on the GPU here, and a frame that has not landed by the next call is
// simply missed and counted (in practice the driver finishes long before).
//
// The readback is the whole backbuffer minus cursor and OSD - the same
// composition the host's own virtual screen shows up to the cursor (the base
// OpenGL manager composites the game into it, scaled and letterboxed by its
// own draw-rect logic), and a first run's guess that the game "occupies the
// origin region" was measured wrong: probe dumps showed the game at an
// unexpected offset with black around it (BMP probes, 2026-10-23). Capturing
// the buffer itself removes every assumption about where the game sits. The
// cursor exclusion is M1's rule (§6): the guest gets the pointer as live 'C'
// data, so a cursor baked into the video would double it. The fixed 640x480
// capture FBO is used only if
// the backbuffer is ever a different size than the encode target - one GL
// blit does that scaling, a CPU downscale would not. Bytes cross to the
// sending thread through the same single-slot mailbox shape the guest render
// path uses; the consumer converts RGBA to the codec's NV12 on its own thread.

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
// readback into the mailbox, then issues this frame's. sceneW/sceneH is the
// default framebuffer's full size - the capture is that whole buffer, since
// the compositor has already placed the game inside it. If the buffer is
// larger than the encode target the capture FBO's blit scales it; otherwise
// the readback goes straight off the backbuffer.
void captureVideoFrame(int sceneW, int sceneH);

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
