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

// The capture unit uses std::mutex/chrono/vector and the GL entry points; the
// standard headers it needs collide with the forbidden-symbol macros, the same
// as every other coop file.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-capture.h"

#ifdef USE_OPENGL

// GLAD, the same route the VR graphics manager takes. By the time
// captureVideoFrame is called the GLAD entry points are loaded - it is
// called from inside refreshScreen, which is drawing through them.
#include "graphics/opengl/system_headers.h"

#include "common/debug.h"
#include "common/textconsole.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace Coop {

volatile bool g_coopCaptureWant = false;

namespace {

// The mailbox. One slot, latest frame wins - the same policy as the guest
// render path, for the same reason: on a live stream the newest frame is the
// only one worth having, and queueing would add latency to the exact path
// M1 is measuring.
std::mutex g_capM;
std::vector<byte> g_capFrame; // kCapBytes of RGBA
uint32 g_capSeq;
uint32 g_capTakenSeq; // what the consumer has already been handed
int64 g_capAtUs;

// The GL plumbing. Every object here is created, used, and (never) destroyed
// on the graphics thread, which is the only caller of captureVideoFrame -
// no GL handle ever crosses a thread. Two PBO slots so a map can never race
// the GPU still writing the same buffer.
GLuint g_capFbo, g_capTex;
GLuint g_capPbo[2];
GLsync g_capFence[2];
int g_capSlot; // slot to issue into this frame; the other one is reaped
bool g_glBroken;

// Counters, written only on the graphics thread; read through captureStats.
int g_capIssues, g_capReaps, g_capMissed, g_capMapFails;

int64 nowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ensureGl() {
	if (g_capFbo)
		return true;
	if (g_glBroken)
		return false;

	glGenTextures(1, &g_capTex);
	glBindTexture(GL_TEXTURE_2D, g_capTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kCapWidth, kCapHeight, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &g_capFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_capFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, g_capTex, 0);
	const bool complete =
		glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenBuffers(2, g_capPbo);
	for (int i = 0; i < 2; ++i) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, g_capPbo[i]);
		// One static allocation per slot; streaming usage, never re-specced.
		glBufferData(GL_PIXEL_PACK_BUFFER, kCapBytes, nullptr, GL_STREAM_READ);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	if (!complete) {
		g_glBroken = true;
		warning("coop capture: the 640x480 capture FBO is not complete on this driver - "
				"the framebuffer readback will not run");
		return false;
	}
	return true;
}

} // anonymous namespace

void captureVideoFrame(int gameW, int gameH, int sceneW, int sceneH) {
	if (!ensureGl())
		return;

	// 1. Collect the previous frame's readback if the GPU has finished it.
	// A zero-timeout wait: this is the thread that renders the game, it
	// must never block on the GPU. Not-signaled means the frame is missed,
	// counted, and the slot is still retired - reusing a PBO the driver may
	// still be writing would corrupt it.
	GLsync &reapFence = g_capFence[1 - g_capSlot];
	if (reapFence) {
		const GLenum state = glClientWaitSync(reapFence, 0, 0);
		glDeleteSync(reapFence);
		reapFence = 0;

		if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, g_capPbo[1 - g_capSlot]);
			void *pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kCapBytes,
				GL_MAP_READ_BIT);
			if (pixels) {
				std::lock_guard<std::mutex> lock(g_capM);
				if (g_capFrame.size() != (size_t)kCapBytes)
					g_capFrame.resize(kCapBytes);
				memcpy(g_capFrame.data(), pixels, kCapBytes);
				g_capAtUs = nowUs();
				++g_capSeq;
			}
			// Unmap while still bound - unbinding first would make the
			// unmap an invalid operation that leaves the buffer mapped.
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			if (pixels) {
				++g_capReaps;
			} else {
				// Name the failure exactly once per mode, then go quiet:
				// this thread renders the game and must not log 30 times
				// a second.
				if (g_capMapFails < 3)
					debug("coop capture: glMapBufferRange on the readback PBO returned null "
						"(pending GL error 0x%x)", glGetError());
				++g_capMapFails;
			}
		} else {
			++g_capMissed;
		}
	}

	// 2. Issue this frame. Clamp the game region to the framebuffer first:
	// a video mode the manager never got around to honouring must not turn
	// into an out-of-range blit.
	if (gameW <= 0 || gameH <= 0 || sceneW <= 0 || sceneH <= 0)
		return;
	if (gameW > sceneW)
		gameW = sceneW;
	if (gameH > sceneH)
		gameH = sceneH;

	// The scissor state belongs to whoever rendered last; blits honor it on
	// the draw side, and a stale GUI scissor would clip the capture. Save
	// and restore it.
	const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
	if (scissorOn)
		glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_capFbo);
	// No Y flip here on purpose: the capture keeps the framebuffer's own
	// orientation (row 0 = bottom), and the consumer flips rows while it
	// converts - flipping in a walk costs nothing, flipping here would cost
	// a second blit or a buffer copy.
	glBlitFramebuffer(0, 0, gameW, gameH, 0, 0, kCapWidth, kCapHeight,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (scissorOn)
		glEnable(GL_SCISSOR_TEST);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, g_capPbo[g_capSlot]);
	// With a pack buffer bound the pointer is an offset into it, and the
	// readback itself is asynchronous - the fence below is its completion.
	glReadPixels(0, 0, kCapWidth, kCapHeight, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	g_capFence[g_capSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	++g_capIssues;

	g_capSlot = 1 - g_capSlot;
}

bool grabCapturedFrame(byte *dst, int64 *capturedAtUs) {
	std::lock_guard<std::mutex> lock(g_capM);
	// A new frame only - the render path's dedup lesson, learned before it
	// happened: without this, a consumer ticking at its own rate would
	// "capture" one frame thousands of times and count it as a stream.
	if (!g_capSeq || g_capSeq == g_capTakenSeq || g_capFrame.size() != (size_t)kCapBytes)
		return false;
	g_capTakenSeq = g_capSeq;
	memcpy(dst, g_capFrame.data(), kCapBytes);
	if (capturedAtUs)
		*capturedAtUs = g_capAtUs;
	return true;
}

CaptureStats captureStats() {
	CaptureStats s;
	s.issues = g_capIssues;
	s.reaps = g_capReaps;
	s.missed = g_capMissed;
	s.mapFails = g_capMapFails;
	return s;
}

} // namespace Coop

#endif // USE_OPENGL
