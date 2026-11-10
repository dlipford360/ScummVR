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

// The marker file is read with stdio, and the logging goes through ScummVM's
// debug()/warning(). Same choice as coop-transport.cpp: the standard headers this
// file needs collide with the forbidden-symbol macros.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/android-vr/coop-selftest.h"
#include "backends/platform/android-vr/coop-capture.h"
#include "backends/platform/android-vr/coop-video-display.h"

#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-seal.h"
#include "backends/platform/android-vr/coop-session.h"
#include "backends/platform/android-vr/coop-transport.h"
#include "backends/platform/android-vr/coop-wire.h"

#include "common/debug.h"
#include "common/textconsole.h"

#include "backends/platform/android-vr/coop-encode.h"
#include "backends/platform/android-vr/coop-decode.h"
#include "backends/platform/android-vr/coop-gameaudio.h"
#include "backends/platform/android-vr/coop-cursor.h"
#include "backends/platform/android-vr/coop-interactive.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef __ANDROID__
// The control plane is the Java HTTP stack, which needs a JNIEnv on the calling
// thread. See runTest() below.
#include "backends/platform/android/jni-android.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace Coop {

namespace {

// adb-pushable, and the VR app can read /sdcard with the MANAGE_EXTERNAL_STORAGE
// app-op the manifest already documents. The app-private files dir would need
// `run-as`, which makes the test slower to drive than it is worth.
const char *const kDefaultMarkerPaths[] = {
	"/sdcard/scummvm-coop-test.txt",
};

// Upper bound on the marker's seconds key. A two-headset visual check needs
// the host's video window wide open while the guest starts (a 60 s cap made
// a late-joining guest race an already-finished host, 2026-10-23); 5 minutes
// is comfortable and still bounded.
// Upper bound on a bounded run's `seconds` key. Long enough that a demo does
// not hit it mid-scene (the 300 s original clamped a 600 s request silently —
// the run "ended on its own" 5 minutes in, which reads like a liveness bug).
const int kMaxRunSeconds = 900;

struct Config {
	char baseUrl[160];
	char code[Wire::kMaxRoomCodeBytes];
	char invite[64];
	char ownerName[32];
	bool asHost;
	bool plaintext;
	int seconds;
	// 0 = no video, 1 = bench the encoder only (no room needed), 2 = encode and
	// send 'V' frames to the room while joined (host role), 3 = receive the
	// host's 'V' frames and decode them (guest role).
	int videoMode;
	// With videoMode 3, also hand the decoded frames to the VR thread so the
	// virtual screen quad shows the host's video (display = true).
	bool display;
	// With videoMode 2, take the encoder's input from the live game
	// framebuffer (the §6 capture path) instead of the synthetic scene.
	bool capture;
	// A ScummVM game target to start directly (launcher skipped) so a
	// capture run has a real animated scene without anyone putting on the
	// headset. Empty = leave the launcher alone.
	char gameTarget[64];
	// 0 = no game audio, 1 = tap the mixer output and send Opus 'A' frames
	// (host role), 2 = receive 'A' frames and play them through the mixer
	// (guest role). Independent of videoMode: audio can be measured alone.
	int audioMode;
	// 0 = no cursor traffic, bit0 = send 'C' from the VR pointer ray,
	// bit1 = accept 'C' frames and draw the overlay on the screen quad.
	// Role-neutral on purpose: the relay routes host C to every guest and
	// guest C to the host, and never echoes a peer its own frames, so
	// both headsets can be senders and receivers at once. Like audio,
	// independent of videoMode.
	int cursorMode;
	bool cursorDraw;
	int cursorHz;
	// Host-only: create the room open, so guests join with the room code alone
	// (typed on the gaze keyboard) instead of code + 32-hex invite. The invite
	// still exists; the relay simply does not demand it for this room.
	bool openRoom;
};

void trim(char *text) {
	size_t start = 0;
	while (text[start] == ' ' || text[start] == '\t')
		++start;
	if (start)
		memmove(text, text + start, strlen(text + start) + 1);

	size_t end = strlen(text);
	while (end && (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t'))
		text[--end] = 0;
}

bool readConfig(const char *path, Config *out) {
	FILE *file = fopen(path, "rt");
	if (!file)
		return false;

	memset(out, 0, sizeof(*out));
	out->asHost = true;
	out->plaintext = false;
	out->seconds = 20;
	out->cursorDraw = true;
	out->cursorHz = 30;

	char line[256];
	while (fgets(line, sizeof(line), file)) {
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *equals = strchr(line, '=');
		if (!equals) {
			warning("coop selftest: ignoring malformed line in %s", path);
			continue;
		}
		*equals = 0;
		char *key = line;
		char *value = equals + 1;
		trim(key);
		trim(value);

		if (!strcmp(key, "relay"))
			snprintf(out->baseUrl, sizeof(out->baseUrl), "%s", value);
		else if (!strcmp(key, "role"))
			out->asHost = strcmp(value, "guest") != 0;
		else if (!strcmp(key, "code"))
			snprintf(out->code, sizeof(out->code), "%s", value);
		else if (!strcmp(key, "invite"))
			snprintf(out->invite, sizeof(out->invite), "%s", value);
		else if (!strcmp(key, "owner"))
			snprintf(out->ownerName, sizeof(out->ownerName), "%s", value);
		else if (!strcmp(key, "plaintext"))
			out->plaintext = atoi(value) != 0;
		else if (!strcmp(key, "video")) {
			if (!strcmp(value, "bench"))
				out->videoMode = 1;
			else if (!strcmp(value, "send"))
				out->videoMode = 2;
			else if (!strcmp(value, "recv"))
				out->videoMode = 3;
			else
				warning("coop selftest: video value must be \"bench\", \"send\" or \"recv\", got \"%s\"", value);
		} else if (!strcmp(key, "display")) {
			out->display = !strcmp(value, "true") || atoi(value) != 0;
		} else if (!strcmp(key, "capture")) {
			out->capture = !strcmp(value, "true") || atoi(value) != 0;
		} else if (!strcmp(key, "game")) {
			snprintf(out->gameTarget, sizeof(out->gameTarget), "%s", value);
		} else if (!strcmp(key, "audio")) {
			if (!strcmp(value, "send"))
				out->audioMode = 1;
			else if (!strcmp(value, "recv"))
				out->audioMode = 2;
		} else if (!strcmp(key, "cursor_draw")) {
			out->cursorDraw = !strcmp(value, "true") || atoi(value) != 0;
		} else if (!strcmp(key, "cursor_hz")) {
			out->cursorHz = atoi(value);
			if (out->cursorHz < 1 || out->cursorHz > 60)
				out->cursorHz = 30;
		} else if (!strcmp(key, "cursor")) {
			if (!strcmp(value, "send"))
				out->cursorMode = 1;
			else if (!strcmp(value, "recv"))
				out->cursorMode = 2;
			else if (!strcmp(value, "both"))
				out->cursorMode = 3;
			else
				warning("coop selftest: cursor value must be \"send\", \"recv\" or \"both\", got \"%s\"", value);
		} else if (!strcmp(key, "open")) {
			out->openRoom = !strcmp(value, "true") || atoi(value) != 0;
		} else if (!strcmp(key, "seconds")) {
			out->seconds = atoi(value);
			if (out->seconds < 1)
				out->seconds = 1;
			if (out->seconds > kMaxRunSeconds)
				out->seconds = kMaxRunSeconds;
		} else {
			warning("coop selftest: unknown key \"%s\"", key);
		}
	}
	fclose(file);

	if (!out->baseUrl[0]) {
		warning("coop selftest: %s has no relay= line, refusing to run", path);
		return false;
	}
	if (!out->asHost && (!out->code[0] || !out->invite[0])) {
		warning("coop selftest: role=guest needs both code= and invite=");
		return false;
	}
	return true;
}

int64 nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Sealing cost on this SoC, with the key the relay actually handed out. This is
// the number that says whether sealing is affordable before any of the media
// paths that would use it exist.
void benchSealing(const char *mediaKeyHex, const char *code, char roleTag, uint16 peerId) {
	Seal::Context ctx;
	if (!ctx.setFromHex(mediaKeyHex)) {
		warning("coop selftest: seal bench refused the media key");
		return;
	}
	if (!ctx.selfCheck()) {
		warning("coop selftest: seal bench self-check FAILED on this CPU - do not trust any sealing number below");
		return;
	}

	const int kPairs = 2000;
	const int kPlainBytes = 1083; // the single-datagram plaintext budget once sealed

	byte plain[kPlainBytes];
	byte sealed[kPlainBytes + 32];
	byte opened[kPlainBytes + 32];
	for (int i = 0; i < kPlainBytes; ++i)
		plain[i] = (byte)(i * 7 + 3);

	Wire::HeaderLine ref;
	snprintf(ref.code, sizeof(ref.code), "%s", code);
	ref.roleTag = roleTag;
	ref.peerId = peerId;
	ref.type = 'V';

	const int64 started = nowMs();
	int openedOk = 0;
	for (int i = 0; i < kPairs; ++i) {
		ref.seq = (uint64)i;
		const int sealedLen = Seal::sealPayload(ctx, sealed, sizeof(sealed), ref, plain, kPlainBytes);
		if (sealedLen <= 0)
			continue;
		// The AAD is the header this frame would arrive with, so the bench must
		// rebuild the same reference rather than reuse a stale seq.
		if (Seal::openPayload(ctx, opened, sizeof(opened), ref, sealed, sealedLen, false) == kPlainBytes
			&& !memcmp(opened, plain, kPlainBytes))
			++openedOk;
	}
	const int64 elapsed = nowMs() - started;

	debug("coop selftest: seal bench %d pairs of %d bytes in %lld ms = %.1f us per seal+open, %.1f MB/s, %d opened",
		kPairs, kPlainBytes, (long long)elapsed, (double)elapsed * 1000.0 / kPairs,
		(double)kPairs * kPlainBytes * 2.0 / (1024.0 * 1024.0) / ((double)elapsed / 1000.0), openedOk);
	if (openedOk != kPairs)
		warning("coop selftest: seal bench round trip failed %d of %d times", kPairs - openedOk, kPairs);
}

// --------------------------------------------------------------------- video

// The M1 numbers the latency gate needs: what the host encoder costs on this
// SoC at the video settings the design settled on, and (with video=send) what
// those access units look like once they are fragmented, sealed and put on the
// wire. The content is synthetic - a blocky gradient with one moving region -
// because a *static* scene would flatter every encoder: the moving region is
// what makes each P-frame carry real residual, and the block edges are what
// makes rate control work. The encoder cost is the same either way once real
// capture exists; this is the cost measured before the capture path does.

const int kVideoWidth = 640;
const int kVideoHeight = 480;
const int kVideoFps = 30;
const int kVideoBitrate = 4000000; // the M1 per-direction budget
const int kVideoIdrMs = 1500;      // middle of the 1-2 s the protocol wants

struct VideoRun {
	VideoEncoder enc;

	// Sending mode: every access unit also becomes a 'V' media frame.
	bool sendRoom;
	Transport *transport;
	uint32 vFrameId;

	std::vector<byte> baseY;    // the static scene, computed once
	std::vector<byte> frame;    // exactly the bytes the codec accepts
	std::vector<byte> rgba;     // capture mode: the newest readback, pre-conversion
	bool haveLastFrame;         // capture mode: frame holds the last real pixels
	std::vector<int64> submitAt; // frame index -> monotonic us at hand-off
	std::vector<int64> captureAtUs; // frame index -> when those pixels left the GPU (carried for re-feeds)
	std::vector<int64> latUs;    // completed AUs: submit -> callback latency
	std::vector<int> keyAt;      // frame indices whose output was a key AU

	int aus, keys, maxAu, sendFails, oversized;
	// X kind 9 retransmit requests received from the guest, and how many the
	// history could actually answer (an old frame may already be pruned).
	int rnrReceived, resendHit, resendMiss;
	int captured, missedCapture, refed; // capture mode: real frames used, ticks without one, duplicates re-fed
	uint64 auBytes;

	VideoRun() : sendRoom(false), transport(nullptr), vFrameId(1), haveLastFrame(false),
		aus(0), keys(0), maxAu(0), sendFails(0), oversized(0),
		rnrReceived(0), resendHit(0), resendMiss(0),
		captured(0), missedCapture(0), refed(0), auBytes(0) {}
};

int64 nowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool startVideoRun(VideoRun *r, int frames) {
	if (!r->enc.open(kVideoWidth, kVideoHeight, kVideoFps, kVideoBitrate, kVideoIdrMs))
		return false;
	if (r->enc.inputFormat() == VideoEncoder::kInputUnknown) {
		warning("coop selftest: the codec wants a pixel format this path does not produce");
		r->enc.close();
		return false;
	}

	r->baseY.assign((size_t)kVideoWidth * kVideoHeight, 0);
	byte *y = r->baseY.data();
	for (int row = 0; row < kVideoHeight; ++row)
		for (int col = 0; col < kVideoWidth; ++col)
			y[(size_t)row * kVideoWidth + col] =
				(byte)(16 + ((col >> 4) & 7) * 24 + ((row >> 4) & 3) * 8 + ((row / 96) & 1) * 40);
	r->frame.assign((size_t)r->enc.inputBytes(), 0);
	r->submitAt.assign(frames > 0 ? frames : 1, 0);
	// Same sizing as submitAt: the capture-time carry is indexed by frame,
	// and an unsized vector would write through a null data() on frame 0.
	r->captureAtUs.assign(frames > 0 ? frames : 1, 0);
	return true;
}

void renderVideoFrame(VideoRun *r, int frameIndex) {
	const int w = kVideoWidth, h = kVideoHeight;
	memcpy(r->frame.data(), r->baseY.data(), (size_t)w * h);

	// Film grain, one value per 8x8 block (an encoder's own unit of work),
	// changing every frame. Without it the first device bench produced 0.11
	// Mbit/s on a 4 Mbit/s budget: a flat synthetic scene asks nothing of the
	// encoder, so its latency number flatters the hardware. Grain is the
	// cheapest content that makes every macroblock carry residual, which is
	// what a real game frame with moving sprites and dithered gradients does.
	uint32 rng = (uint32)frameIndex * 2654435761u + 1013904223u;
	for (int by0 = 0; by0 < h; by0 += 8) {
		for (int bx0 = 0; bx0 < w; bx0 += 8) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			const int jitter = (int)(rng % 33) - 16;
			for (int row = by0; row < by0 + 8; ++row) {
				byte *d = r->frame.data() + (size_t)row * w + bx0;
				for (int col = 0; col < 8; ++col)
					d[col] = (byte)(d[col] + jitter);
			}
		}
	}

	// The moving region crosses the full width, so P-frames keep earning their
	// bits. 96x64 is roughly a character sprite at this scale.
	const int bw = 96, bh = 64;
	const int bx = (frameIndex * 37) % (w - bw);
	const int by = (h - bh) / 2;
	for (int row = by; row < by + bh; ++row)
		memset(r->frame.data() + (size_t)row * w + bx, 235, bw);

	// Chroma last, in whichever layout the codec asked for. Neutral everywhere
	// except a warm tint on the moving block (chroma residuals are part of the
	// cost too).
	byte *chroma = r->frame.data() + (size_t)w * h;
	const bool nv12 = r->enc.inputFormat() == VideoEncoder::kInputNV12;
	if (nv12) {
		for (int i = 0; i < w * h / 2; i += 2) {
			chroma[i] = 128;
			chroma[i + 1] = 128;
		}
		for (int row = by / 2; row < (by + bh) / 2; ++row) {
			byte *uv = chroma + (size_t)row * w;
			for (int col = bx / 2; col < (bx + bw) / 2; ++col) {
				uv[col * 2] = 235;
				uv[col * 2 + 1] = 45;
			}
		}
	} else {
		byte *u = chroma;
		byte *v = chroma + (size_t)(w / 2) * (h / 2);
		memset(u, 128, (size_t)(w / 2) * (h / 2));
		memset(v, 128, (size_t)(w / 2) * (h / 2));
		for (int row = by / 2; row < (by + bh) / 2; ++row) {
			memset(u + (size_t)row * (w / 2) + bx / 2, 235, bw / 2);
			memset(v + (size_t)row * (w / 2) + bx / 2, 45, bw / 2);
		}
	}
}

// The capture source's conversion: the capture unit's RGBA (row 0 = the
// BOTTOM of the image, GL convention) to the codec's top-first NV12 with
// stride 640 - the exact layout this codec was verified to accept (§14).
// Integer BT.601 limited-range, the matrix the guest's display shader uses,
// so what the host packs and what the guest unpacks describe the same
// picture. Chroma comes from the 2x2 mean rather than the top-left pixel,
// and the row flip costs one subtraction - which is why the capture unit
// leaves orientation here instead of spending a blit or a copy on it.
// One-shot frame dumps for bring-up: the first real captured frame and one
// ~10 s later go to BMP files in the app's files dir, so the exact pixels
// the capture produced can be pulled off the device and looked at. This is
// how a wrong-color or wrong-region report gets settled with evidence
// instead of argument. The path is the debug package's files dir - the
// same one scummvm.log lives in.
void dumpCaptureProbe(const byte *rgba, int n) {
	char path[160];
	snprintf(path, sizeof(path),
		"/data/data/org.scummvm.scummvm.vr.debug/files/coop-frame-%d.bmp", n);
	FILE *f = fopen(path, "wb");
	if (!f) {
		warning("coop selftest: cannot write the capture probe %s", path);
		return;
	}
	const int w = kVideoWidth, h = kVideoHeight, dim3 = w * h * 3;
	byte head[14] = {'B', 'M'};
	uint32 fileSize = 14 + 40 + dim3;
	head[2] = fileSize; head[3] = fileSize >> 8; head[4] = fileSize >> 16; head[5] = fileSize >> 24;
	head[10] = 54; // pixel data offset
	fwrite(head, 1, 14, f);
	byte ih[40] = {0};
	ih[0] = 40;
	ih[4] = w; ih[5] = w >> 8; ih[6] = w >> 16; ih[7] = w >> 24;
	// Negative height: top-down rows, which is what the walk below writes.
	ih[8] = -h & 0xFF; ih[9] = (-h) >> 8; ih[10] = (-h) >> 16; ih[11] = (-h) >> 24;
	ih[12] = 1; ih[14] = 24; // planes, bits per pixel
	ih[20] = dim3 & 0xFF; ih[21] = dim3 >> 8; ih[22] = dim3 >> 16; ih[23] = dim3 >> 24;
	fwrite(ih, 1, 40, f);
	// The mailbox keeps the GL bottom-first orientation; a top-down BMP
	// starts from the last row.
	for (int y = h - 1; y >= 0; --y) {
		const byte *row = rgba + (size_t)y * w * 4;
		byte rgb[w * 3];
		for (int x = 0; x < w; ++x) {
			rgb[x * 3] = row[x * 4 + 2];
			rgb[x * 3 + 1] = row[x * 4 + 1];
			rgb[x * 3 + 2] = row[x * 4];
		}
		fwrite(rgb, 1, w * 3, f);
	}
	fclose(f);
	debug("coop selftest: capture probe written to %s", path);
}

void rgbaToNv12(const byte *src, byte *dst) {
	const int w = kVideoWidth, h = kVideoHeight;
	byte *Y = dst;
	byte *UV = dst + (size_t)w * h;
	for (int row = 0; row < h; ++row) {
		const byte *s = src + (size_t)(h - 1 - row) * w * 4;
		byte *y = Y + (size_t)row * w;
		for (int col = 0; col < w; ++col)
			y[col] = (byte)(((66 * s[col * 4] + 129 * s[col * 4 + 1] + 25 * s[col * 4 + 2] + 128) >> 8) + 16);
		if (row & 1)
			continue; // NV12 chroma is half height; each pair of rows feeds one UV row
		const byte *s2 = src + (size_t)(h - 2 - row) * w * 4; // the row below in the image
		byte *uv = UV + (size_t)(row / 2) * w;
		for (int col = 0; col < w; col += 2) {
			const int r = (s[col * 4] + s[col * 4 + 4] + s2[col * 4] + s2[col * 4 + 4]) >> 2;
			const int g = (s[col * 4 + 1] + s[col * 4 + 5] + s2[col * 4 + 1] + s2[col * 4 + 5]) >> 2;
			const int b = (s[col * 4 + 2] + s[col * 4 + 6] + s2[col * 4 + 2] + s2[col * 4 + 6]) >> 2;
			// BT.601 limited-range chroma. The shift must match the Y
			// line's (>>8): these coefficients sum to zero and peak at
			// ~28560, so >>8 lands U/V in the correct 16..240 swing.
			// A >>10 here quietly shrinks chroma to a quarter and the
			// guest sees grayscale with green flicker (measured, 2026-10-23).
			uv[col] = (byte)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			uv[col + 1] = (byte)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
		}
	}
}

void videoAu(void *ctx, const byte *au, int len, uint32 flags, int64 ptsUs) {
	VideoRun *r = static_cast<VideoRun *>(ctx);

	// pts is frameIndex * 1e6 / fps by construction, so it is the key back to
	// the submit timestamp - which is the whole latency measurement.
	const int frameIdx = (int)(ptsUs * kVideoFps / 1000000);
	if (frameIdx >= 0 && frameIdx < (int)r->submitAt.size() && r->submitAt[frameIdx] != 0)
		r->latUs.push_back(nowUs() - r->submitAt[frameIdx]);

	++r->aus;
	r->auBytes += (uint64)len;
	if (len > r->maxAu)
		r->maxAu = len;
	if (flags & VideoEncoder::kAuKeyframe) {
		++r->keys;
		r->keyAt.push_back(frameIdx);
	}

	if (r->sendRoom && r->transport) {
		// The reassembler on the other end caps a frame at kMaxFragments.
		// A real game's keyframes reach it (144 KB measured), so the bound
		// is not theoretical - but if a driver overshoots even that, the
		// right answer is to drop the whole frame - a partial frame poisons
		// the decoder - and to count it.
		const int kMaxFrameBytes = Wire::FrameReassembler::kMaxFragments * Wire::kMaxFragmentBytes;
		if (len > kMaxFrameBytes) {
			++r->oversized;
			return;
		}
		if (!r->transport->sendMedia('V', r->vFrameId, au, len)) {
			++r->sendFails;
		} else if (r->vFrameId % kCoopLatSampleEvery == 0 && r->transport->clockSynced()) {
			// Latency sample, host end: when these pixels left the GPU, when
			// they entered the encoder, and when the last fragment left the
		 // socket, in the relay's clock - the guest stamps the same
			// frameId, and the two join offline. The submit stamp matters:
			// re-fed frames carry an older capture time (the game repaints
			// ~10-12 fps), so captured->sent alone would fold the game's
			// repaint cadence into the pipeline's cost. captured->submit is
			// the pixel age; submit->sent is the encode-and-send pipeline.
			const int64 off = r->transport->clockEpochOffsetUs();
			int64 capEpoch = 0, submitEpoch = 0;
			if (frameIdx >= 0 && frameIdx < (int)r->captureAtUs.size() && r->captureAtUs[frameIdx])
				capEpoch = r->captureAtUs[frameIdx] + off;
			if (frameIdx >= 0 && frameIdx < (int)r->submitAt.size() && r->submitAt[frameIdx])
				submitEpoch = r->submitAt[frameIdx] + off;
			debug("coop selftest: lat host id %u captured %lld submitted %lld sent %lld",
				r->vFrameId, (long long)capEpoch, (long long)submitEpoch, (long long)(nowUs() + off));
		}
		++r->vFrameId;
	}
}

// X kind 9: a retransmit request from the guest. Payload: [9][target u16le]
// [count][frameId u32le x count] - self-describing rather than a new wire
// verb, because the relay treats X bodies as opaque (the guide's catalogue
// grows, the protocol does not). The target is the all-marker: whichever
// participant holds the history answers.
bool handleRetransmitRequest(VideoRun *r, const Transport::Event &event) {
	if (event.header.type != 'X' || event.payloadLen < 4 || event.payload[0] != 9)
		return false;
	if (!(event.payload[1] == 0xFF && event.payload[2] == 0xFF))
		return false;
	const int count = event.payload[3];
	if (count < 1 || count > 8 || event.payloadLen < 4 + 4 * count)
		return false;
	++r->rnrReceived;
	for (int i = 0; i < count; ++i) {
		const byte *p = event.payload + 4 + 4 * i;
		const uint32 frameId = (uint32)p[0] | ((uint32)p[1] << 8)
			| ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
		if (r->transport && r->transport->resendMedia(frameId))
			++r->resendHit;
		else
			++r->resendMiss;
	}
	return true;
}

void reportVideoRun(VideoRun *r, int framesFed) {	if (r->latUs.empty()) {
		warning("coop selftest: the encoder produced no output for %d submitted frames", framesFed);
		return;
	}
	std::sort(r->latUs.begin(), r->latUs.end());
	const size_t n = r->latUs.size();
	const int64 p50 = r->latUs[n / 2];
	const int64 p95 = r->latUs[(n * 95) / 100];
	const double seconds = (double)framesFed / kVideoFps;

	debug("coop selftest: encoder %dx%d@%d: %d frames fed, %d AUs out (%d key, max %d B), "
			"%.2f Mbit/s, submit-to-AU p50 %lld us p95 %lld us max %lld us, encoder input drops %d",
		kVideoWidth, kVideoHeight, kVideoFps, framesFed, r->aus, r->keys, r->maxAu,
		(double)r->auBytes * 8.0 / 1e6 / (seconds > 0.0 ? seconds : 1.0),
		(long long)p50, (long long)p95, (long long)r->latUs.back(),
		r->enc.inputDrops());

	// The IDR claim is only real if the intervals can be seen. Frame gaps
	// convert to ms through the fps the encoder was told.
	if (r->keyAt.size() >= 2) {
		std::vector<int64> gaps;
		for (size_t i = 1; i < r->keyAt.size() && gaps.size() < 16; ++i)
			gaps.push_back((int64)(r->keyAt[i] - r->keyAt[i - 1]) * 1000 / kVideoFps);
		std::sort(gaps.begin(), gaps.end());
		debug("coop selftest: encoder IDRs every ~%lld ms (requested %d ms, %d key units)",
			(long long)gaps[gaps.size() / 2], kVideoIdrMs, r->keys);
	} else if (r->keys < 2) {
		warning("coop selftest: only %d keyframe(s) observed in %.1f s - the IDR interval was not honoured",
			r->keys, seconds);
	}

	if (r->sendRoom) {
		debug("coop selftest: encoder output sent as %u 'V' frames (frameId through %u), "
				"%d send refusals, %d AUs dropped for exceeding 64 fragments",
			r->vFrameId - 1, r->vFrameId - 1, r->sendFails, r->oversized);
		if (r->sendFails)
			warning("coop selftest: the transport refused %d encoded frames - role, key, or size",
				r->sendFails);
		debug("coop selftest: host retransmit: %d X-kind-9 requests received, %d frames re-sent "
				"from history, %d requests past the history window",
			r->rnrReceived, r->resendHit, r->resendMiss);
	}
}

// Standalone encoder bench: no room, no relay, no credentials. It runs first
// when asked for, because the encoder cost is the M1 gate item and a broken
// control plane must not hide it.
void benchEncoder(int seconds) {
	VideoRun run;
	const int frames = seconds * kVideoFps;
	if (!startVideoRun(&run, frames)) {
		warning("coop selftest: no usable H.264 encoder on this device");
		return;
	}
	const int64 t0 = nowUs();
	int fed = 0;
	for (int i = 0; i < frames; ++i) {
		const int64 due = t0 + (int64)i * 1000000 / kVideoFps;
		// Poll outputs *during* the frame gap, not only at submit ticks. If
		// the observation model is "look once per frame", an AU that
		// completed 2 ms after the last look appears 31 ms late, and the
		// gate would then be measuring its own instrumentation. drain(2)
		// waits at most 2 ms for an output and fires the callback if one
		// arrives.
		while (nowUs() < due)
			run.enc.drain(2, videoAu, &run);
		renderVideoFrame(&run, i);
		run.submitAt[i] = nowUs();
		if (!run.enc.submitFrame(run.frame.data(), (int64)i * 1000000 / kVideoFps, videoAu, &run)) {
			warning("coop selftest: the encoder failed at frame %d", i);
			break;
		}
		++fed;
	}
	// A pipeline still holds a frame or two after the last submit; not draining
	// it would understate the output and make the last latency samples a lie.
	run.enc.drain(1000, videoAu, &run);
	// Report what was actually fed. The first device run showed why: after an
	// immediate codec failure it claimed "300 submitted frames", which was the
	// plan, not the run.
	reportVideoRun(&run, fed);
	run.enc.close();
}

// ---------------------------------------------------------------- receive-side

// video=recv: this device is a real guest of a real video host. Every 'V'
// fragment feeds the wire reassembler; every complete AU feeds the decoder.
// The number that matters is the same shape as the encoder's, mirrored:
// AU-complete to decoded-frame callback, which is the guest's share of the
// one-way budget. Display is deliberately not part of this - the self-test
// thread has no render context - and saying so keeps the decode number honest
// about what it proves.

// Retransmit-request policy: request pacing, a grace window for frames that
// may still be arriving (reordering), an age bound past which the host's
// send history has almost certainly pruned the frame, and a per-request cap.
const int kRnrIntervalMs = 250;
const int kRnrGraceMs = 80;
const int kRnrMaxAgeMs = 1500;
const int kRnrMaxPerRequest = 8;

struct DecodeRun {
	VideoDecoder dec;
	Wire::FrameReassembler reassembler;
	// frameId -> wall time at which the AU finished arriving. The host's pts
	// is its own frame counter, not a shared clock, so the correlation key is
	// the wire frameId, which both ends agree exists.
	std::map<uint32, int64> completeAt;
	std::vector<int64> latUs;
	Transport *transport; // for the clock estimate behind latency samples
	int vFragments, completeAUs, badHeaders, pushFails;
	// Arrival observations: reordering can produce temporary forward gaps
	// and nonzero first fragment indices. Neither alone proves packet loss.
	uint32 maxFrameId;
	uint32 lastFrameId;
	int frameGaps;     // frameIds skipped entirely (whole frames lost)
	int partialFrames; // frames whose first arrival was not fragment 0 (fragments lost)
	std::set<uint32> framesSeen;
	uint32 lastCompleteId = 0;
	bool waitingForIdr = true;
	int recoveryWaits = 0, recoverySkipped = 0, idrResumes = 0;

	// frameId -> first moment it was known missing (a higher id arrived). The
	// guest asks the host to re-send these (X kind 9). Whole frames, not
	// fragments: a partial video frame is worthless by construction, so its
	// full value is in its last fragment and selective requests would cost
	// more than they save.
	std::map<uint32, int64> missingSince;
	int rnrSent, rnrEntries;
	int64 nextRnrMs;

	DecodeRun() : transport(nullptr), vFragments(0), completeAUs(0), badHeaders(0), pushFails(0),
		maxFrameId(0), lastFrameId(0), frameGaps(0), partialFrames(0),
		rnrSent(0), rnrEntries(0), nextRnrMs(0) {}
};

void decodeCb(void *ctx, const byte *plane, int len, int64 ptsUs) {
	DecodeRun *r = static_cast<DecodeRun *>(ctx);
	// ptsUs is the frameId we pushed with. An unseen id means the decoder
	// emitted something we did not feed it - worth knowing, not worth a crash.
	std::map<uint32, int64>::iterator at = r->completeAt.find((uint32)ptsUs);
	if (at != r->completeAt.end()) {
		r->latUs.push_back(nowUs() - at->second);
		if ((uint32)ptsUs % kCoopLatSampleEvery == 0 && r->transport && r->transport->clockSynced()) {
			// Latency sample, guest decode hop: arrival and decode in the
			// relay's clock. The render hop stamps the same id from the VR
			// thread; the host stamped its two ends. The join is offline.
			const int64 off = r->transport->clockEpochOffsetUs();
			debug("coop selftest: lat guest id %u received %lld decoded %lld",
				(uint32)ptsUs, (long long)(at->second + off), (long long)(nowUs() + off));
		}
		r->completeAt.erase(at);
	}
	// Touch the buffer so the callback is not optimizable to nothing, and
	// because a decode that produced zero bytes is not a decode.
	// With display wanted, the frame also goes to the VR thread's mailbox -
	// the layout values are the codec's published ones, deliberately.
	if (len > 0)
		publishVideoFrame(plane, len, nowUs(), r->dec.width(), r->dec.height(),
			r->dec.stride(), r->dec.sliceHeight(), (uint32)ptsUs);
}

// MediaCodec's AVC output is Annex B (SPS/PPS are prepended to key AUs).
// Inspect NAL headers, not codec flags which are not carried on the wire.
bool containsIdr(const byte *data, int len) {
	for (int i = 0; i + 3 < len; ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1
				&& (data[i + 3] & 0x1f) == 5)
			return true;
	}
	return false;
}

void feedVideoFragment(DecodeRun *r, const Transport::Event &event) {
	if (event.payloadLen <= Wire::kFragHeaderBytes) {
		++r->badHeaders;
		return;
	}
	uint32 frameId = 0;
	uint16 index = 0, count = 0;
	if (!Wire::readFragmentHeader(event.payload, event.payloadLen, &frameId, &index, &count)) {
		++r->badHeaders;
		return;
	}
	++r->vFragments;
	if (frameId > r->maxFrameId)
		r->maxFrameId = frameId;
	// Count arrival-order discontinuities, which can include reordering.
	if (!r->framesSeen.empty() && r->framesSeen.count(frameId) == 0 && frameId > r->lastFrameId + 1) {
		r->frameGaps += (int)(frameId - r->lastFrameId - 1);
		// Those skipped ids are what the retransmit requests are for.
		for (uint32 m = r->lastFrameId + 1; m < frameId; ++m)
			if (!r->framesSeen.count(m))
				r->missingSince[m] = nowMs();
	}
	if (r->framesSeen.count(frameId))
		r->missingSince.erase(frameId);
	if (r->framesSeen.insert(frameId).second && index != 0)
		++r->partialFrames;
	if (frameId > r->lastFrameId)
		r->lastFrameId = frameId;

	Wire::FrameReassembler::Complete done;
	if (!r->reassembler.feed(frameId, index, count,
			event.payload + Wire::kFragHeaderBytes,
			event.payloadLen - Wire::kFragHeaderBytes, &done))
		return;

	++r->completeAUs;
	// No B-frames in this stream: never feed a late AU behind a newer one.
	// A gap means reference continuity is no longer known, even if the
	// missing AU eventually arrives. Resume only at an independent picture.
	if (r->lastCompleteId && done.frameId <= r->lastCompleteId) {
		++r->recoverySkipped;
		return;
	}
	if (r->lastCompleteId && done.frameId != r->lastCompleteId + 1 && !r->waitingForIdr) {
		r->waitingForIdr = true;
		++r->recoveryWaits;
	}
	r->lastCompleteId = done.frameId;
	if (r->waitingForIdr) {
		if (!containsIdr(done.data, done.len)) {
			++r->recoverySkipped;
			return;
		}
		r->waitingForIdr = false;
		++r->idrResumes;
	}
	r->completeAt[done.frameId] = nowUs();
	if (!r->dec.push(done.data, (int)done.len, (int64)done.frameId, decodeCb, r)) {
		r->completeAt.erase(done.frameId);
		r->waitingForIdr = true;
		++r->recoveryWaits;
		++r->pushFails;
		if (r->pushFails == 1)
			warning("coop selftest: the decoder refused AU %u - video stops being measured, "
					"the room keeps running", done.frameId);
	}
}

// Game audio over the room (audio=send/recv). The frame counter GameAudio uses
// is its own; the reassembler is deliberately separate from the video one
// (FrameReassembler is per media kind by contract). At 32 kbps a 20 ms Opus
// frame is ~80 bytes - one fragment - but the reassembler keeps the receive
// path correct if that ever stops being true.
struct AudioRun {
	Coop::GameAudio audio;
	Wire::FrameReassembler reassembler;
	bool active;
};

void feedAudioFragment(AudioRun *r, const Transport::Event &event) {
	if (event.payloadLen <= Wire::kFragHeaderBytes)
		return;
	uint32 frameId = 0;
	uint16 index = 0, count = 0;
	if (!Wire::readFragmentHeader(event.payload, event.payloadLen, &frameId, &index, &count))
		return;
	Wire::FrameReassembler::Complete done;
	if (!r->reassembler.feed(frameId, index, count,
			event.payload + Wire::kFragHeaderBytes,
			event.payloadLen - Wire::kFragHeaderBytes, &done))
		return;
	r->audio.feedFrame(done.frameId, done.data, done.len);
}

// Ask the host, at most every kRnrIntervalMs, for whole frames that never
// arrived. Whole frames rather than fragments: a partial video frame is
// worthless by construction (the reassembler drops it), so its full value is
// in its last fragment and a per-fragment protocol would cost more than it
// saves at these loss rates. The grace window keeps this from double-
// requesting frames that are merely reordered, and the age bound stops asking
// once the host's send history has almost certainly dropped them.
void flushRetransmitRequests(DecodeRun *r, Transport *transport) {
	if (!transport || !transport->isOpen() || r->missingSince.empty())
		return;
	const int64 now = nowMs();
	if (now < r->nextRnrMs)
		return;
	r->nextRnrMs = now + kRnrIntervalMs;

	byte req[4 + 4 * kRnrMaxPerRequest];
	req[0] = 9;	// kind: retransmit request (the client guide's catalogue)
	req[1] = 0xFF;
	req[2] = 0xFF;
	int n = 0;
	std::map<uint32, int64>::iterator it = r->missingSince.begin();
	while (it != r->missingSince.end()) {
		const int64 age = now - it->second;
		if (age > kRnrMaxAgeMs) {
			// Past the history window: asking again cannot help. The
			// recovery guard already covered the picture consequence.
			r->missingSince.erase(it++);
			continue;
		}
		if (age >= kRnrGraceMs && n < kRnrMaxPerRequest) {
			const uint32 id = it->first;
			req[4 + 4 * n] = (byte)(id & 0xff);
			req[5 + 4 * n] = (byte)((id >> 8) & 0xff);
			req[6 + 4 * n] = (byte)((id >> 16) & 0xff);
			req[7 + 4 * n] = (byte)((id >> 24) & 0xff);
			++n;
		}
		++it;
	}
	if (!n)
		return;
	req[3] = (byte)n;
	if (transport->sendFrame('X', req, 4 + 4 * n)) {
		++r->rnrSent;
		r->rnrEntries += n;
	}
}

void reportDecodeRun(DecodeRun *r, int seconds) {
	debug("coop selftest: guest retransmit: %d X-kind-9 requests sent, %d frame requests in them",
		r->rnrSent, r->rnrEntries);
	debug("coop selftest: guest recovery: %d reference discontinuities, %d AUs skipped, %d IDR resumes",
		r->recoveryWaits, r->recoverySkipped, r->idrResumes);
	debug("coop selftest: guest video arrivals: frameIds through %u, %d AUs complete, "
			"%d forward-gap IDs (may reorder), %d frames first seen at nonzero fragment index",
			r->maxFrameId, r->completeAUs, r->frameGaps, r->partialFrames);
	if (r->completeAUs == 0) {
		warning("coop selftest: received %u V fragments but reassembled no complete AU - "
				"was a video host present, and did it start before this guest?",
				(uint32)r->vFragments);
		return;
	}
	if (r->dec.decoded() == 0) {
		warning("coop selftest: %d AUs fed to the decoder, none decoded - the stream may "
				"not start at a keyframe", r->completeAUs);
		return;
	}
	if (!r->latUs.empty()) {
		std::sort(r->latUs.begin(), r->latUs.end());
		const size_t n = r->latUs.size();
		debug("coop selftest: guest decode: %d fragments in, %d AUs complete, %d frames decoded "
				"(%d x %d, color %d, stride %d slice %d), AU-complete-to-decoded p50 %lld us "
				"p95 %lld us max %lld us, over %d s",
			r->vFragments, r->completeAUs, r->dec.decoded(),
			r->dec.width(), r->dec.height(), r->dec.colorFormat(), r->dec.stride(),
			r->dec.sliceHeight(),
			(long long)r->latUs[n / 2], (long long)r->latUs[(n * 95) / 100],
			(long long)r->latUs.back(), seconds);
	} else {
		// Decoded frames whose ids we could not match: still proof of decode,
		// but the latency column stays empty rather than inventing numbers.
		debug("coop selftest: guest decode: %d AUs in, %d decoded, but no AU->frame "
				"correlation was possible - no latency reported", r->completeAUs, r->dec.decoded());
	}
	if (r->pushFails || r->badHeaders)
		warning("coop selftest: guest decode had %d refused AUs and %d malformed fragment headers",
			r->pushFails, r->badHeaders);
}

void runTestBody(const Config &cfg) {
	// The call site is before scummvm_main, so the backend is created but not yet
	// initialised and the Java HTTP manager is not up yet. Waiting also keeps the
	// test's logging away from the startup noise.
	std::this_thread::sleep_for(std::chrono::seconds(5));

	debug("coop selftest: start, role=%s, relay=%s", cfg.asHost ? "host" : "guest", cfg.baseUrl);

	if (cfg.videoMode == 1) {
		// Encoder only. Deliberately before the control plane: the M1 latency
		// gate cares about this cost even when the network half is broken.
		benchEncoder(cfg.seconds < 10 ? cfg.seconds : 10);
	}

	Session session;
	session.setBaseUrl(Common::String(cfg.baseUrl));

	const bool requested = cfg.asHost
		? session.createRoom(cfg.ownerName[0] ? cfg.ownerName : "headset", cfg.openRoom)
		: session.joinRoom(cfg.code, cfg.invite);
	if (!requested) {
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		warning("coop selftest: the control-plane request could not even be queued: %s", reason);
		return;
	}

	// The request is asynchronous and its callback runs on the connection
	// manager's thread, so this thread waits for it to land.
	const int64 deadline = nowMs() + 15000;
	while (session.pending() && nowMs() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (session.pending()) {
		warning("coop selftest: no control-plane answer within 15 s. Nothing reached the relay: "
				"check the URL, that the relay advertises a LAN-reachable address, and that the "
				"certificate is trusted (the Java layer logs the TLS failure)");
		return;
	}
	if (!session.lastRequestOk()) {
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		warning("coop selftest: control plane refused the request: %s", reason);
		return;
	}

	char reason[Wire::kMaxHeaderBytes];
	session.copyGateReason(reason, sizeof(reason));
	if (!session.gateOk()) {
		warning("coop selftest: version gate refused this relay: %s", reason);
		return;
	}
	debug("coop selftest: gate accepted the relay");

	if (!session.haveParticipant()) {
		warning("coop selftest: the response carried no participant");
		return;
	}
	Participant p;
	session.copyParticipant(&p);
	debug("coop selftest: room %s, peerId %u, role %c, relay %s:%d, protocol v%d, payload cap %d, seq ceiling %llu, encryption %s",
		p.code, p.peerId, p.role, p.relayHost, p.relayPort, p.relayProtocolVersion, p.maxPayloadBytes,
		(unsigned long long)p.maxSeq, p.mediaEncryption);
	debug("coop selftest: secret lengths: session %zu, media key %zu (values are never logged)",
		strlen(p.sessionSecret), strlen(p.mediaKeyHex));

	if (session.haveOwnerCredentials()) {
		OwnerCredentials owner;
		session.copyOwnerCredentials(&owner);
		// This is the one place a room code and an invite are printed on purpose:
		// it is how the second headset is pointed at the same room. Both are
		// room-scoped, and neither is the owner secret. An open room does not
		// need the invite at all, and the line says so - no one should hunt for
		// a secret the relay stopped asking for.
		if (cfg.openRoom)
			debug("coop selftest: join the second headset with code=%s (open room: no invite needed)", p.code);
		else
			debug("coop selftest: join the second headset with code=%s invite=%s", p.code, owner.invite);
	}

	benchSealing(p.mediaKeyHex, p.code, p.role, p.peerId);

	Transport transport;
	if (!transport.open(p.relayHost, p.relayPort)) {
		warning("coop selftest: could not open a UDP socket to %s:%d", p.relayHost, p.relayPort);
		return;
	}
	if (cfg.plaintext) {
		warning("coop selftest: plaintext mode requested - payloads will be unencrypted and unauthenticated");
		transport.enablePlaintextMode();
	} else if (!transport.setMediaKey(p.mediaKeyHex)) {
		warning("coop selftest: the transport refused the media key");
		transport.close();
		return;
	}
	if (!transport.join(p.code, p.peerId, p.sessionSecret, p.role)) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		warning("coop selftest: join refused: %s", status);
		transport.close();
		return;
	}

	const int64 joinedAt = nowMs();
	while (!transport.isReady() && !transport.isClosed() && nowMs() < joinedAt + 10000)
		std::this_thread::sleep_for(std::chrono::milliseconds(25));

	if (!transport.isReady()) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		// The counters ARE the diagnosis on this path, so print them even though the
		// test failed. "no WELCOME" has at least three causes that look identical from
		// the game side: hellos > 0 with datagrams in == 0 means the datagram left this
		// device and nothing came back (firewall, wrong advertised relay.host, relay
		// down); datagrams in > 0 with unparseable > 0 means the relay answered and this
		// build could not read the answer; hellos == 0 means join() never got as far as
		// writing anything.
		warning("coop selftest: no WELCOME within 10 s (last relay line: %s). HELLO written %u, "
			"datagrams in %u, unparseable %u, crypto drops %u, events dropped %u",
			status[0] ? status : "nothing at all",
			transport.hellosSent(), transport.datagramsIn(), transport.badPackets(),
			transport.cryptoDrops(), transport.droppedEvents());
		transport.close();
		return;
	}
	debug("coop selftest: WELCOME after %lld ms, sealed=%d, max single-datagram payload %d bytes",
		(long long)(nowMs() - joinedAt), (int)transport.isSealed(), transport.maxAppPayloadBytes());

	// video=send: this device becomes a real video host for the run. It needs
	// the host role because the relay drops a guest's 'V' frames, and it
	// replaces the synthetic 4 KB probe with frames of the actual shape.
	VideoRun video;
	bool videoOn = false;
	if (cfg.videoMode == 2) {
		if (!cfg.asHost) {
			warning("coop selftest: video=send needs role=host (the relay refuses a guest's V frames); skipping");
		} else if (!startVideoRun(&video, cfg.seconds * kVideoFps)) {
			warning("coop selftest: no usable H.264 encoder on this device");
		} else {
			video.sendRoom = true;
			video.transport = &transport;
			// Answer the guest's retransmit requests (X kind 9): keep sent AUs
			// for a short window so a named missing frame can be re-sent.
			transport.enableMediaHistory();
			videoOn = true;
			if (cfg.capture) {
				// Real pixels from the §6 path instead of the synthetic
				// scene. Nothing here reads the framebuffer - that stays on
				// the graphics thread - this thread only asks for whatever
				// was last reaped.
				video.rgba.resize((size_t)Coop::kCapBytes);
				Coop::g_coopCaptureWant = true;
				debug("coop selftest: capture source requested - frames will come from the "
					"game framebuffer%s", cfg.gameTarget[0] ? " (the game= key autolaunches one)" : "");
			}
		}
	}

	// video=recv: the mirror run. This device is a guest; the video host is
	// whatever else is in the room (the Mac harness today, a second headset
	// when there is one). There is nothing to set up beyond opening the
	// decoder - the bitstream carries its own description - but the role
	// check is not decorative: as a host this device would receive nothing,
	// because the relay does not echo a peer's own frames back to it.
	DecodeRun decode;
	if (cfg.videoMode == 3) {
		if (cfg.asHost) {
			warning("coop selftest: video=recv needs role=guest (a host would receive nothing); skipping");
		} else if (!decode.dec.open(kVideoWidth, kVideoHeight)) {
			warning("coop selftest: no usable H.264 decoder on this device");
		} else {
			decode.transport = &transport;
			debug("coop selftest: decoder open for %d x %d, waiting for the room's video host",
				kVideoWidth, kVideoHeight);
			if (cfg.display) {
				g_coopVideoDisplayWant = true;
				debug("coop selftest: display requested - the VR screen quad will show the "
						"received video as frames decode");
			}
		}
	}
	const int64 videoStartMs = nowMs();
	int videoFramesFed = 0;
	int64 videoNextMs = videoStartMs;
	int64 lastContentCapUs = 0; // capture time of the newest real pixels, carried across re-feeds

	// audio=send/recv: the game-audio path. Independent of video on purpose -
	// it shares only the room - so a run can measure one leg without the other.
	// The sender is started before the loop and pumps inside it; the tap it
	// installs costs one relaxed load per audio buffer while it waits.
	AudioRun audio;
	audio.active = false;
	if (cfg.audioMode == 1) {
		if (!cfg.asHost) {
			warning("coop selftest: audio=send needs role=host (a guest's A frames would reach no one); skipping");
		} else if (!audio.audio.startSender(&transport)) {
			warning("coop selftest: the game-audio sender could not start (mixer or Opus)");
		} else {
			audio.active = true;
			debug("coop selftest: game-audio sender started - 20 ms Opus frames as 'A' at 32 kbps");
		}
	} else if (cfg.audioMode == 2) {
		if (cfg.asHost) {
			warning("coop selftest: audio=recv needs role=guest (a host would receive nothing); skipping");
		} else if (!audio.audio.startReceiver()) {
			warning("coop selftest: the game-audio receiver could not start (mixer or Opus)");
		} else {
			audio.active = true;
			debug("coop selftest: game-audio receiver started - 'A' frames play through the mixer");
		}
	}

	// cursor=send/recv/both: the M2 pointer channel, symmetric. The
	// sender side is just a fixed-cadence grabber: the sample itself is
	// published by this device's VR thread (its ray-plane hit IS the
	// pointer), and this thread only ships the newest one ~30x a second,
	// per the relay's cursor-channel guidance. Routing is the relay's:
	// host C reaches every guest, guest C reaches the host, and nobody
	// receives their own echo - so a headset showing both its own local
	// dot and the peer's overlay is the intended picture. Loss of a C
	// frame self-heals on the next tick - the payload is latest-state,
	// no retransmit. The receiver needs no setup at all: the VR draw
	// path turns its overlay on when the first 'C' arrives.
	bool cursorOn = false;
	int cursorSent = 0, cursorSendFails = 0, cursorNoSample = 0;
	int64 cursorStartMs = 0, cursorNextMs = 0;
	int cursorTicks = 0;
	const int kCursorHz = cfg.cursorHz;
	if (cfg.cursorMode & 1) {
		cursorOn = true;
		Coop::g_coopCursorSendWant = true;
		cursorStartMs = nowMs();
		cursorNextMs = cursorStartMs;
		debug("coop selftest: cursor sender started - 'C' frames from the VR pointer ray at %d Hz", kCursorHz);
	}

	// Stay joined, send one application frame a second, and report everything the
	// relay sends back. With two headsets this is where a sealed frame crossing a
	// real relay in both directions gets proven.
	uint32 framesIn = 0, dataIn = 0, rosterIn = 0, controlIn = 0;
	int appSeq = 0;
	int clockStatusEvery = 0;
	int mediaProbes = 0;
	int64 nextSend = nowMs();
	const int64 runStartedMs = nowMs();
	const int64 until = runStartedMs + (int64)cfg.seconds * 1000;
	while (nowMs() < until && !transport.isClosed() && !g_coopUiLeaveWanted) {
		Transport::Event event;
		while (transport.pollEvent(&event)) {
			++framesIn;
			if (event.kind == Wire::kRecvData) {
				++dataIn;
				// In recv mode the V stream is the measurement, and at 30 fps
				// it is also ~27 datagrams a second - per-fragment debug would
				// bury every other line. The DecodeRun counters are the report.
				if (event.header.type == 'A' && audio.active) {
					feedAudioFragment(&audio, event);
				} else if (event.header.type == 'V' && decode.dec.isOpen()) {
					feedVideoFragment(&decode, event);
				} else if (video.sendRoom && handleRetransmitRequest(&video, event)) {
					// A retransmit the host answered (counted, not logged:
					// an RNR burst is a dozen datagrams a second at worst).
				} else if (event.header.type == 'C') {
					// The M2 pointer stream: store latest-per-peer; the VR
					// draw thread snapshots it. The want-flag turns the
					// overlay on at the first frame, so a room whose host
					// sends no 'C' costs the guest nothing.
					Coop::receivePeerCursor(event.header.peerId, event.payload, event.payloadLen);
					Coop::g_coopCursorDisplayWant = cfg.cursorDraw;
				} else {
					debug("coop selftest: frame '%c' from peer %u, %d bytes, envelope 0x%02x",
						event.header.type, event.header.peerId, event.payloadLen, event.envelope);
				}
			} else if (event.kind == Wire::kRecvRoster) {
				++rosterIn;
				debug("coop selftest: roster: %s", event.text);
			} else if (event.kind == Wire::kRecvControl) {
				++controlIn;
				debug("coop selftest: control: %s", event.text);
			} else if (event.kind == Wire::kRecvForeign) {
				warning("coop selftest: datagram for another room on this socket");
			} else {
				warning("coop selftest: malformed datagram");
			}
		}

		if (nowMs() >= nextSend) {
			// X kind 7 (APP_PING) with a timestamp: the catalogue in the client
			// guide, so a second headset answers it and the round trip is visible.
			byte ping[16];
			ping[0] = 7;
			ping[1] = 0xFF;
			ping[2] = 0xFF;
			const int64 stamp = nowMs();
			for (int i = 0; i < 8; ++i)
				ping[3 + i] = (byte)(((uint64)stamp >> (i * 8)) & 0xff);
			if (!transport.sendFrame('X', ping, 11))
				warning("coop selftest: sendFrame refused (app payload #%d)", appSeq);
			++appSeq;

			// A 4 KB frame exercises fragmentation and reassembly on the device,
			// which is what a video frame actually looks like on this wire. With
			// real video running it would only compete with it for bandwidth, so
			// a video host skips the stand-in; a guest still probes with `M`
			// because it has no video to send.
			if (!videoOn) {
				const char probeType = (p.role == 'h') ? 'V' : 'M';
				byte big[4096];
				for (int i = 0; i < (int)sizeof(big); ++i)
					big[i] = (byte)(i * 31 + appSeq);
				if (!transport.sendMedia(probeType, (uint32)appSeq, big, sizeof(big)))
					warning("coop selftest: sendMedia('%c') refused for the 4 KB probe", probeType);
				++mediaProbes;
			}
			nextSend += 1000;

			// Keep the shared clock current for the render thread's latency
			// samples, and state the estimate's own error bar periodically:
			// every lat line is only comparable up to that number.
			if (transport.clockSynced()) {
				g_coopClockOffsetUs = transport.clockEpochOffsetUs();
				g_coopClockSynced = true;
				if (++clockStatusEvery % 30 == 1)
					debug("coop selftest: clock: relay offset %lld us, half-RTT worst %lld us",
						(long long)g_coopClockOffsetUs, (long long)transport.clockUncertaintyUs());
			}
		}

		// Guest video: ask for what never arrived (self-paced to
		// kRnrIntervalMs inside).
		if (decode.dec.isOpen())
			flushRetransmitRequests(&decode, &transport);

		// Game audio: the tap ring fills at real time, so this cannot run
		// ahead of the game; it just has to be called often enough.
		if (audio.active)
			audio.audio.pump();

		// Pointer: a 30 Hz grab-and-send, drift-free from the run start
		// like the video cadence (the re-feed pacing lesson, §14). If the
		// VR loop has not published a sample yet there is nothing to send;
		// the counter says so rather than inventing a position.
		if (cursorOn && nowMs() >= cursorNextMs) {
			uint16 cx, cy;
			bool vis;
			if (Coop::grabCursorSample(&cx, &cy, &vis)) {
				byte payload[5];
				Coop::encodeCursorPayload(payload, sizeof(payload), cx, cy, vis);
				if (transport.sendFrame('C', payload, sizeof(payload)))
					++cursorSent;
				else
					++cursorSendFails;
			} else {
				++cursorNoSample;
			}
			cursorNextMs = cursorStartMs + (int64)(++cursorTicks) * 1000 / kCursorHz;
		}

		if (videoOn && nowMs() >= videoNextMs) {
			// Real pacing: the frame timer, not the loop tick. Drift is
			// recomputed from the run start so a slow loop iteration does not
			// turn into a permanent frame-rate loss. The run window and the
			// timestamp table are sized from the same `seconds` but start a
			// few ms apart - so cap at the table, do not overrun it.
			if (videoFramesFed >= (int)video.submitAt.size()) {
				videoOn = false;
			} else if (cfg.capture) {
				// Capture mode: the newest real pixels if there are any, otherwise
				// the last ones again. The re-feed is the design, not padding: a
				// game repaints on its own schedule (~10-12 fps for SCUMM titles),
				// and feeding the 1-frame encoder pipeline at the *game's* rate puts
				// the wait for the next input into every frame's measured latency
				// (p50 201 ms, §14). Feeding at the encode cadence with a duplicate
				// costs a zero-residual P-frame - a few hundred bytes - and keeps
				// the pipeline's 35 ms pacing regardless of who is idle. No
				// synthetic fallback: if capture never produced a frame, nothing is
				// sent, and the report says so.
				int64 capAt = 0;
				if (Coop::grabCapturedFrame(video.rgba.data(), &capAt)) {
					// Probe: the first real frame, one ~10 s into the run,
					// one ~90 s in. Two samples distinguish a static wrong
					// (bad region, bad palette) from a dynamic one (fades,
					// flicker); the third lands in the interactive part of a
					// game intro, which is where a cursor-sensitive question
					// (is the engine cursor in the copy?) can be answered at
					// all - intro scenes hide it, so an early frame proves
					// nothing. Two counter traps already hit, both settled
					// this way: gate on the encoder tick, not captured frames
					// (captured counts only real redraws, ~11/s for SCUMM
					// titles, while videoFramesFed advances at the steady
					// 30/s encode cadence, so thresholds are seconds x 30);
					// and fire on >= with a done flag, not == - this runs
					// only on real-frame ticks, and a game redraws on ~a
					// third of ticks, so an exact tick number gets missed
					// most runs. A probe is still taken only on a real
					// frame: a dump of a duplicate would duplicate one.
					static bool probe2Done = false, probe3Done = false;
					if (video.captured == 0)
						dumpCaptureProbe(video.rgba.data(), 1);
					else if (!probe2Done && videoFramesFed >= 300) {
						dumpCaptureProbe(video.rgba.data(), 2);
						probe2Done = true;
					} else if (!probe3Done && videoFramesFed >= 2700) {
						dumpCaptureProbe(video.rgba.data(), 3);
						probe3Done = true;
					}
					rgbaToNv12(video.rgba.data(), video.frame.data());
					++video.captured;
					video.haveLastFrame = true;
				} else {
					++video.missedCapture;
					if (video.missedCapture == 1 && !video.haveLastFrame)
						warning("coop selftest: the capture path has produced no frame yet - "
							"is a game running? (the marker's game= key autolaunches one)");
				}
				if (video.haveLastFrame) {
					if (capAt == 0)
						++video.refed;
					else
						lastContentCapUs = capAt;
					video.submitAt[videoFramesFed] = nowUs();
					video.captureAtUs[videoFramesFed] = lastContentCapUs;
					if (!video.enc.submitFrame(video.frame.data(),
							(int64)videoFramesFed * 1000000 / kVideoFps, videoAu, &video)) {
						warning("coop selftest: the encoder failed at frame %d; video stops, the room does not",
							videoFramesFed);
						videoOn = false;
					}
					++videoFramesFed;
					videoNextMs = videoStartMs + (int64)videoFramesFed * 1000 / kVideoFps;
				}
			} else {
				renderVideoFrame(&video, videoFramesFed);
				video.submitAt[videoFramesFed] = nowUs();
				if (!video.enc.submitFrame(video.frame.data(),
						(int64)videoFramesFed * 1000000 / kVideoFps, videoAu, &video)) {
					warning("coop selftest: the encoder failed at frame %d; video stops, the room does not",
						videoFramesFed);
					videoOn = false;
				}
				++videoFramesFed;
				videoNextMs = videoStartMs + (int64)videoFramesFed * 1000 / kVideoFps;
			}
		}
		// The same lesson the encoder bench learned on device: outputs must be
		// polled between feeds, not only at feed ticks, or the measurement
		// includes the poll cadence itself. In recv mode the loop's usual nap
		// *is* the drain - it waits the same wall time and fires any frame the
		// codec finished meanwhile.
		if (decode.dec.isOpen())
			decode.dec.drain(10, decodeCb, &decode);
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	const int64 runElapsedMs = nowMs() - runStartedMs;
	char status[Wire::kMaxHeaderBytes];
	transport.statusText(status, sizeof(status));

	// Video stats last: the room summary should lead when something else went
	// wrong, and these lines are about the encoder, not the room.
	if (cfg.videoMode == 2 && video.enc.isOpen()) {
		video.enc.drain(500, videoAu, &video);
		reportVideoRun(&video, videoFramesFed);
		video.enc.close();
		if (cfg.capture) {
			// Stop the graphics thread from issuing more readbacks first,
			// then report both sides: what the GPU path delivered and what
			// the encoder actually consumed.
			Coop::g_coopCaptureWant = false;
			const Coop::CaptureStats cs = Coop::captureStats();
			debug("coop selftest: host capture: %d readbacks issued, %d collected, %d not-ready, "
					"%d map failures; encoder fed %d frames (%d real, %d duplicate re-feeds), "
					"%d ticks had no captured frame to re-feed yet",
				cs.issues, cs.reaps, cs.missed, cs.mapFails,
				video.captured + video.refed, video.captured, video.refed, video.missedCapture);
		}
	}
	if (cursorOn) {
		// Stop the VR thread publishing first, then count both ends of the
		// sender: what the room shipped and what it had nothing to ship for.
		Coop::g_coopCursorSendWant = false;
		debug("coop selftest: cursor sender: %d 'C' frames sent (%d send refusals), "
				"%d ticks had no published sample yet",
			cursorSent, cursorSendFails, cursorNoSample);
	}
	if ((cfg.cursorMode & 2) || Coop::g_coopCursorDisplayWant) {
		const Coop::CursorStats cs = Coop::cursorStats();
		debug("coop selftest: cursor receiver: %d 'C' payloads stored (%d malformed), "
				"%d peer entries expired",
			cs.received, cs.badPayload, cs.expired);
		Coop::g_coopCursorDisplayWant = false;
	}
	if (cfg.videoMode == 3 && decode.dec.isOpen()) {
		// Whatever the room still owes us first - a final AU can sit in the
		// socket buffer after the timer expires - then whatever the decoder
		// still holds. Skipping either one loses the tail of the run.
		Transport::Event event;
		while (transport.pollEvent(&event)) {
			if (event.kind == Wire::kRecvData && event.header.type == 'V')
				feedVideoFragment(&decode, event);
			else if (event.kind == Wire::kRecvData && event.header.type == 'A' && audio.active)
				feedAudioFragment(&audio, event);
			else if (event.kind == Wire::kRecvData && event.header.type == 'C')
				Coop::receivePeerCursor(event.header.peerId, event.payload, event.payloadLen);
		}
		decode.dec.drain(1000, decodeCb, &decode);
		reportDecodeRun(&decode, cfg.seconds);
		decode.dec.close();
		// The quad goes back to the local scene the moment measuring stops.
		// (The VR thread keeps the last uploaded texture until then; it only
		// consults the flag while drawing.)
		if (cfg.display)
			g_coopVideoDisplayWant = false;
	}

	if (cfg.display && cfg.videoMode == 3) {
		// The render half of the chain, from the VR thread's counters: frames
		// it actually uploaded, and the decode-callback-to-upload delay. That
		// delay is what the render side adds to the end-to-end number.
		std::lock_guard<std::mutex> lock(g_coopVideo.m);
		if (!g_coopVideo.latUs.empty()) {
			std::sort(g_coopVideo.latUs.begin(), g_coopVideo.latUs.end());
			const size_t n = g_coopVideo.latUs.size();
			debug("coop selftest: guest render: %d frames uploaded to the screen quad "
					"(of %d decoded), decode-to-upload p50 %lld us p95 %lld us max %lld us",
				g_coopVideo.stats.rendered, g_coopVideo.stats.decodedTo,
				(long long)g_coopVideo.latUs[n / 2],
				(long long)g_coopVideo.latUs[(n * 95) / 100],
				(long long)g_coopVideo.latUs.back());
		} else {
			warning("coop selftest: display was requested but the VR thread never picked up "
					"a frame - the screen quad did not change");
		}
	}

	if (audio.active) {
		// Audio stats in the same spirit as the video ones: the counters are
		// the report, and the human verdict (did the guest headset *play* the
		// host's game, in sync, without artifacts) rides on this line.
		const Coop::GameAudio::Counters ac = audio.audio.counters();
		if (cfg.audioMode == 1) {
			audio.audio.stopSender();
			debug("coop selftest: host audio: %d Opus frames sent (%lld bytes), "
					"%d send failures; tap saw %lld bytes, %d input samples dropped (ring full), "
					"%d starved ticks",
				ac.framesEncoded, (long long)ac.bytesSent,
				ac.sendFailures, (long long)ac.tapBytes, ac.tapRingDrops, ac.encodeSkips);
		} else {
			audio.audio.stopReceiver();
			debug("coop selftest: guest audio: %d frames received, %d decoded, %d late drops; "
					"gap events %d (FEC-recovered %d, PLC frames %d, silence frames %d), "
					"%d samples drift-dropped, %d underruns",
				ac.framesReceived, ac.framesDecoded, ac.lateDrops,
				ac.gapEvents, ac.fecFrames, ac.plcFrames, ac.silenceFrames,
				ac.driftDrops, ac.underruns);
		}
	}

	debug("coop selftest: done after %lld ms (configured %d s) - %u frames in (%u data, %u roster, %u control), "
			"%d app frames and %d probe media frames sent, dropped events %u, bad packets %u, crypto drops %u, "
			"HELLO written %u, datagrams in %u",
		(long long)runElapsedMs, cfg.seconds, framesIn, dataIn, rosterIn, controlIn, appSeq, mediaProbes,
		transport.droppedEvents(), transport.badPackets(), transport.cryptoDrops(),
		transport.hellosSent(), transport.datagramsIn());
	if (status[0])
		debug("coop selftest: last relay status line: %s", status);
	if (transport.cryptoDrops())
		warning("coop selftest: inbound frames failed authentication - a shared room key means every "
				"participant can produce them, so check for another participant before suspecting the port");
	transport.close();
}

// Attach this thread to the JVM before touching the control plane.
//
// The first device run crashed 5 s in, at the first control-plane call:
//   PostRequest::start -> ConnectionManager::instance -> ConnectionManagerAndroid
//   -> JNI::getEnv -> JNI::fetchEnv -> abort()
// JNI::fetchEnv() calls GetEnv() and aborts when the answer is JNI_EDETACHED; it
// deliberately does not attach, so every native thread that reaches the Java layer
// must call attachThread() itself - which is what OSystem_Android's timer and audio
// threads do. A std::thread is not attached by definition.
//
// The transport needs no JNIEnv (plain POSIX UDP), but the thread is one unit and
// holding the attachment for its lifetime is simpler than attaching per call.
void runTest(const Config &cfg) {
#ifdef __ANDROID__
	JNI::attachThread();
#endif
	runTestBody(cfg);
#ifdef __ANDROID__
	JNI::detachThread();
#endif
}

// ---------------------------------------------------------------------------
// The interactive join service (coop-interactive.h). Same thread recipe as
// runTest - JVM attached, control plane on this thread - but it lives until
// LEAVE ROOM rather than until a timer, and it runs the guest half of a room:
// receive video to the quad, receive audio to the mixer, send and draw the
// pointer circles. The framing it reuses (feedVideoFragment's reassembly,
// flushRetransmitRequests' NACK pacing, feedAudioFragment's FEC-aware gap
// handling) is this file's statics; moving that machinery into the service is
// the same move the header comment anticipates - these files become the
// product entry point together.

// Published here (namespace Coop scope), consumed by the VR thread; the
// join request is the other direction, behind a mutex because the VR thread
// fires it whenever a trigger edge lands on SUBMIT. `create` picks the role:
// `code` then carries the host-chosen room NAME instead of an existing one.
struct UiRequest {
	std::mutex mutex;
	std::condition_variable cond;
	bool pending = false;
	bool create = false;
	char code[Wire::kMaxRoomCodeBytes];
	char invite[64];
};

UiRequest g_coopUiReq;

// Single source of truth for "who asked to end this run": both loops read it,
// nothing else writes it.

void setUiStatus(const char *text) {
	strncpy(g_coopUiStatus, text, sizeof(g_coopUiStatus) - 1);
	g_coopUiStatus[sizeof(g_coopUiStatus) - 1] = 0;
}

// The toast channel: text first, then the sequence bump (both sides are
// seq_cst, so the VR thread reads a whole string).
void publishNotice(const char *text) {
	strncpy(g_coopUiNotice, text, sizeof(g_coopUiNotice) - 1);
	g_coopUiNotice[sizeof(g_coopUiNotice) - 1] = 0;
	g_coopUiNoticeSeq.fetch_add(1);
}

// Map the relay's last control line to a toast. The transport keeps that
// line verbatim and CLOSED/KICKED/DENY are the terminal ones; a voluntary
// LEAVE maps to no toast at all, because pressing the button is its own
// answer. "host_left" is the reason a relay sends when the host's BYE ends
// the room - the case that used to leave the guest holding a frozen frame.
const char *noticeForClose(bool voluntaryLeave, const char *lastCtl) {
	if (voluntaryLeave)
		return nullptr;
	if (strstr(lastCtl, "host_left"))
		return "HOST LEFT THE ROOM";
	if (strstr(lastCtl, "idle"))
		return "ROOM EXPIRED";
	if (strstr(lastCtl, "KICKED"))
		return "YOU WERE REMOVED";
	if (strstr(lastCtl, "DENY"))
		return "SESSION REFUSED";
	return "ROOM ENDED";
}

void serviceFailed(const char *reason) {
	warning("coop service: %s", reason);
	setUiStatus(reason);
	// The panel may well be closed by now (SUBMIT closes it the moment the
	// request queues), so status alone would be invisible: a refused join
	// has to arrive as a toast.
	publishNotice(reason);
	g_coopUiRoomLine[0] = 0;
	g_coopUiState = kUiIdle;
}

void ensureUiService();

void joinServiceBody() {
	char code[sizeof(g_coopUiReq.code)], invite[sizeof(g_coopUiReq.invite)];
	{
		std::lock_guard<std::mutex> lock(g_coopUiReq.mutex);
		strcpy(code, g_coopUiReq.code);
		strcpy(invite, g_coopUiReq.invite);
		g_coopUiReq.pending = false;
	}
	g_coopUiState = kUiJoining;
	setUiStatus("joining...");
	debug("coop service: join requested for room %s", code);

	// Control plane, version gate, participant: the same gate order the
	// self-test proved, with the marker's relay replaced by the compiled one.
	Session session;
	session.setBaseUrl(Common::String(COOP_UI_RELAY_BASE));
	if (!session.joinRoom(code, invite)) {
		serviceFailed("could not queue the join request");
		return;
	}
	const int64 deadline = nowMs() + 15000;
	while (session.pending() && nowMs() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	if (session.pending()) {
		serviceFailed("relay unreachable (15 s)");
		return;
	}
	if (!session.lastRequestOk()) {
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		Common::String msg(Common::String("refused: ") + reason);
		serviceFailed(msg.c_str());
		return;
	}
	if (!session.gateOk()) {
		serviceFailed("version gate refused the relay");
		return;
	}
	if (!session.haveParticipant()) {
		serviceFailed("no participant in the response");
		return;
	}
	Participant p;
	session.copyParticipant(&p);

	Transport transport;
	if (!transport.open(p.relayHost, p.relayPort)) {
		serviceFailed("could not open a UDP socket to the relay");
		return;
	}
	if (!transport.setMediaKey(p.mediaKeyHex)) {
		serviceFailed("the transport refused the media key");
		transport.close();
		return;
	}
	if (!transport.join(p.code, p.peerId, p.sessionSecret, p.role)) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		Common::String msg(Common::String("join refused: ") + status);
		serviceFailed(msg.c_str());
		transport.close();
		return;
	}
	const int64 joinedAt = nowMs();
	while (!transport.isReady() && !transport.isClosed() && nowMs() < joinedAt + 10000)
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	if (!transport.isReady()) {
		serviceFailed("no WELCOME from the relay (10 s)");
		transport.close();
		return;
	}

	g_coopUiState = kUiJoined;
	snprintf(g_coopUiRoomLine, sizeof(g_coopUiRoomLine), "ROOM %s  peer %u", p.code, p.peerId);
	setUiStatus("in the room");

	// The guest media half, exactly the shape the controlled runs measured
	// healthy (Ethernet-relayed room XKBCT: 1,938 rendered frames / 75 s).
	DecodeRun decode;
	if (decode.dec.open(kVideoWidth, kVideoHeight)) {
		g_coopVideoDisplayWant = true;
	} else {
		warning("coop service: no usable H.264 decoder; audio and cursors only");
	}

	AudioRun audio;
	audio.active = false;
	if (audio.audio.startReceiver())
		audio.active = true;
	else
		warning("coop service: the game-audio receiver could not start");

	// Our pointer ships at the same 30 Hz latest-state cadence; the VR thread
	// publishes the sample from the same ray this device already sends when
	// the self-test drives it.
	g_coopCursorSendWant = true;
	const int kCursorHz = 30;
	int64 cursorNextMs = nowMs();
	int cursorTicks = 0;
	int64 nextClockMs = 0;

	while (!g_coopUiLeaveWanted && !transport.isClosed()) {
		Transport::Event event;
		while (transport.pollEvent(&event)) {
			if (event.kind == Wire::kRecvData) {
				if (event.header.type == 'A' && audio.active) {
					feedAudioFragment(&audio, event);
				} else if (event.header.type == 'V' && decode.dec.isOpen()) {
					feedVideoFragment(&decode, event);
				} else if (event.header.type == 'C') {
					Coop::receivePeerCursor(event.header.peerId, event.payload, event.payloadLen);
					g_coopCursorDisplayWant = true;
				}
			} else if (event.kind == Wire::kRecvRoster) {
				// The roster is JSON entries keyed by peerId; counting that
				// field is honest enough for a headcount and cheaper than a
				// parser at this size.
				int peers = 0;
				for (const char *q = event.text; (q = strstr(q, "\"peerId\"")) != nullptr; ) {
					++peers;
					++q;
				}
				if (peers > 0)
					snprintf(g_coopUiRoomLine, sizeof(g_coopUiRoomLine),
						"ROOM %s  %d PEERS", p.code, peers);
			} else if (event.kind == Wire::kRecvControl) {
				debug("coop service: control: %s", event.text);
			}
		}

		if (decode.dec.isOpen())
			flushRetransmitRequests(&decode, &transport);
		if (audio.active)
			audio.audio.pump();

		const int64 now = nowMs();
		if (now >= cursorNextMs) {
			uint16 cx, cy;
			bool vis;
			if (Coop::grabCursorSample(&cx, &cy, &vis)) {
				byte payload[5];
				Coop::encodeCursorPayload(payload, sizeof(payload), cx, cy, vis);
				transport.sendFrame('C', payload, sizeof(payload));
			}
			cursorNextMs = now + 1000 / kCursorHz;
			++cursorTicks;
		}
		if (now >= nextClockMs) {
			if (transport.clockSynced()) {
				g_coopClockOffsetUs = transport.clockEpochOffsetUs();
				g_coopClockSynced = true;
			}
			nextClockMs = now + 500;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}

	// Teardown, outward-in: socket (BYE rides in close()), audio stream,
	// then the want-flags so the VR thread stops touching video, cursors and
	// the clock. Stale peer circles expire on their own (2 s); the display
	// flag going false stops drawing them immediately.
	const bool wasClosed = transport.isClosed();
	char lastCtl[Wire::kMaxHeaderBytes];
	transport.statusText(lastCtl, sizeof(lastCtl)); // survives close(); it is
	transport.close();                               // the why of a dead room
	if (audio.active)
		audio.audio.stopReceiver();
	if (decode.dec.isOpen())
		decode.dec.close();
	g_coopVideoDisplayWant = false;
	g_coopCursorDisplayWant = false;
	g_coopCursorSendWant = false;
	g_coopUiRoomLine[0] = 0;
	setUiStatus(wasClosed ? "room ended by the relay" : "left the room");
	if (const char *note = noticeForClose(!wasClosed, lastCtl))
		publishNotice(note); // "HOST LEFT THE ROOM": the guest auto-leaves and says why
	g_coopUiState = kUiIdle;
	debug("coop service: joined %lld s, teardown complete",
		(long long)(nowMs() - joinedAt) / 1000);
}

// CREATE ROOM: this headset names a NEW open room and becomes its host -
// the same role the marker self-test drives with role=host/video=send/
// capture=true, reached from the UI instead of a file. The other headset
// JOIN ROOMs by typing the name. The media loop is the host half of the
// self-test run loop with the measurement scaffolding (probes, latency
// tables, reports) stripped: the interactive job is to be watched, not to
// measure being watched.
void createServiceBody() {
	char name[sizeof(g_coopUiReq.code)];
	{
		std::lock_guard<std::mutex> lock(g_coopUiReq.mutex);
		strcpy(name, g_coopUiReq.code);
		g_coopUiReq.code[0] = 0;
		g_coopUiReq.invite[0] = 0;
		g_coopUiReq.create = false;
		g_coopUiReq.pending = false;
	}
	g_coopUiState = kUiJoining;
	setUiStatus("creating room...");
	debug("coop service: create requested, room name %s", name);

	Session session;
	session.setBaseUrl(Common::String(COOP_UI_RELAY_BASE));
	if (!session.createRoom("headset", true, name)) {
		serviceFailed("could not queue the create request");
		return;
	}
	const int64 deadline = nowMs() + 15000;
	while (session.pending() && nowMs() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	if (session.pending()) {
		serviceFailed("relay unreachable (15 s)");
		return;
	}
	if (!session.lastRequestOk()) {
		// The two failures a typed name can actually hit: invalid_room_name
		// (the UI pre-validates, so this is the relay disagreeing) and
		// name_taken - both read better as the relay's own word than as a
		// translation of it.
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		Common::String msg(Common::String("refused: ") + reason);
		serviceFailed(msg.c_str());
		return;
	}
	if (!session.gateOk()) {
		serviceFailed("version gate refused the relay");
		return;
	}
	if (!session.haveParticipant()) {
		serviceFailed("no participant in the response");
		return;
	}
	Participant p;
	session.copyParticipant(&p);

	Transport transport;
	if (!transport.open(p.relayHost, p.relayPort)) {
		serviceFailed("could not open a UDP socket to the relay");
		return;
	}
	if (!transport.setMediaKey(p.mediaKeyHex)) {
		serviceFailed("the transport refused the media key");
		transport.close();
		return;
	}
	if (!transport.join(p.code, p.peerId, p.sessionSecret, p.role)) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		Common::String msg(Common::String("join refused: ") + status);
		serviceFailed(msg.c_str());
		transport.close();
		return;
	}
	const int64 joinedAt = nowMs();
	while (!transport.isReady() && !transport.isClosed() && nowMs() < joinedAt + 10000)
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	if (!transport.isReady()) {
		serviceFailed("no WELCOME from the relay (10 s)");
		transport.close();
		return;
	}

	g_coopUiState = kUiJoined;
	snprintf(g_coopUiRoomLine, sizeof(g_coopUiRoomLine), "ROOM %s  HOST", p.code);
	setUiStatus("hosting - others join by name");
	debug("coop service: hosting room %s as peer %u (open room)", p.code, p.peerId);

	// The host media half: capture -> encode -> 'V', exactly the shape the
	// marker runs shipped. If the encoder is unavailable the room still
	// exists and cursors/audio keep working - being a room host is not the
	// same job as being a video host.
	VideoRun video;
	bool videoOn = false;
	if (!startVideoRun(&video, kMaxRunSeconds * kVideoFps)) {
		warning("coop service: no usable H.264 encoder; hosting audio and cursors only");
	} else {
		video.sendRoom = true;
		video.transport = &transport;
		transport.enableMediaHistory(); // answers the guest's retransmit requests
		videoOn = true;
		video.rgba.resize((size_t)Coop::kCapBytes);
		Coop::g_coopCaptureWant = true;
	}

	AudioRun audio;
	audio.active = false;
	if (!audio.audio.startSender(&transport))
		warning("coop service: the game-audio sender could not start (mixer or Opus)");
	else
		audio.active = true;

	g_coopCursorSendWant = true;

	const int kCursorHz = 30;
	int64 cursorNextMs = nowMs();
	int64 videoNextMs = nowMs();
	int64 nextClockMs = 0;
	int videoFramesFed = 0;
	int64 lastContentCapUs = 0;
	bool warnedNoCapture = false;

	while (!g_coopUiLeaveWanted && !transport.isClosed()) {
		Transport::Event event;
		while (transport.pollEvent(&event)) {
			if (event.kind == Wire::kRecvData) {
				if (video.sendRoom && handleRetransmitRequest(&video, event)) {
					// A retransmit answered (counted, not logged).
				} else if (event.header.type == 'C') {
					Coop::receivePeerCursor(event.header.peerId, event.payload, event.payloadLen);
					g_coopCursorDisplayWant = true;
				}
			} else if (event.kind == Wire::kRecvRoster) {
				int peers = 0;
				for (const char *q = event.text; (q = strstr(q, "\"peerId\"")) != nullptr; ) {
					++peers;
					++q;
				}
				if (peers > 0)
					snprintf(g_coopUiRoomLine, sizeof(g_coopUiRoomLine),
						"ROOM %s  %d PEERS", p.code, peers);
			} else if (event.kind == Wire::kRecvControl) {
				debug("coop service: control: %s", event.text);
			}
		}

		if (audio.active)
			audio.audio.pump();

		const int64 now = nowMs();
		if (videoOn && now >= videoNextMs) {
			// The re-feed design from the marker runs, kept whole: feed the
			// encoder at the encode cadence, duplicates included, because
			// feeding at the game's ~11 fps redraw rate puts a frame-wait
			// into every latency sample (§14). A guest sees a held frame,
			// not a stutter, and the pipeline stays paced.
			if (videoFramesFed >= (int)video.submitAt.size()) {
				videoOn = false; // the same 900 s window a marker run caps at
				setUiStatus("hosting - video window ended");
				warning("coop service: the %d s video window ended; audio and cursors continue",
					kMaxRunSeconds);
			} else {
				int64 capAt = 0;
				if (Coop::grabCapturedFrame(video.rgba.data(), &capAt)) {
					rgbaToNv12(video.rgba.data(), video.frame.data());
					++video.captured;
					video.haveLastFrame = true;
				} else {
					++video.missedCapture;
					if (!warnedNoCapture && now - joinedAt > 8000) {
						warnedNoCapture = true;
						warning("coop service: capture has produced no frame after 8 s - "
							"launch a game and guests will see it (the launcher itself is visible too)");
					}
				}
				if (video.haveLastFrame) {
					if (capAt != 0)
						lastContentCapUs = capAt;
					video.submitAt[videoFramesFed] = nowUs();
					video.captureAtUs[videoFramesFed] = lastContentCapUs;
					if (!video.enc.submitFrame(video.frame.data(),
							(int64)videoFramesFed * 1000000 / kVideoFps, videoAu, &video)) {
						warning("coop service: the encoder failed at frame %d; video stops, the room does not",
							videoFramesFed);
						videoOn = false;
					}
					++videoFramesFed;
				}
				videoNextMs = now + 1000 / kVideoFps;
			}
		}
		if (now >= cursorNextMs) {
			uint16 cx, cy;
			bool vis;
			if (Coop::grabCursorSample(&cx, &cy, &vis)) {
				byte payload[5];
				Coop::encodeCursorPayload(payload, sizeof(payload), cx, cy, vis);
				transport.sendFrame('C', payload, sizeof(payload));
			}
			cursorNextMs = now + 1000 / kCursorHz;
		}
		if (now >= nextClockMs) {
			if (transport.clockSynced()) {
				g_coopClockOffsetUs = transport.clockEpochOffsetUs();
				g_coopClockSynced = true;
			}
			nextClockMs = now + 500;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}

	// Teardown, outward-in, same order as the guest loop's. The encoder and
	// sender are stopped before the want-flags drop so nothing publishes
	// into a closed socket. A host's BYE leaves the room to the relay's own
	// lifetime rules - guests see the end as a relay-side close.
	const bool wasClosed = transport.isClosed();
	char lastCtl[Wire::kMaxHeaderBytes];
	transport.statusText(lastCtl, sizeof(lastCtl));
	transport.close();
	if (audio.active)
		audio.audio.stopSender();
	Coop::g_coopCaptureWant = false;
	g_coopCursorDisplayWant = false;
	g_coopCursorSendWant = false;
	g_coopUiRoomLine[0] = 0;
	setUiStatus(wasClosed ? "room ended by the relay" : "closed the room");
	if (const char *note = noticeForClose(!wasClosed, lastCtl))
		publishNotice(note); // a failsafe kick reaches the host the same way
	g_coopUiState = kUiIdle;
	debug("coop service: hosted %lld s, %d frames encoded, %d captured, teardown complete",
		(long long)(nowMs() - joinedAt) / 1000, videoFramesFed, video.captured);
}

void serviceThread() {
#ifdef __ANDROID__
	JNI::attachThread();
#endif
	for (;;) {
		bool create;
		{
			std::unique_lock<std::mutex> lock(g_coopUiReq.mutex);
			g_coopUiReq.cond.wait(lock, [] { return g_coopUiReq.pending; });
			create = g_coopUiReq.create; // the body below takes the rest
		}
		if (create)
			createServiceBody();
		else
			joinServiceBody();
		g_coopUiLeaveWanted = false; // every future join starts clean
	}
	// The thread never leaves: it is one parked cond wait for the process
	// lifetime, which is cheaper than re-attaching a JVM thread per join.
}

std::thread g_coopUiService;
std::once_flag g_coopUiServiceOnce;

void ensureUiService() {
	// Detached like the self-test thread: the process exits, it ends.
	// (Declaration above; this is the definition.)
	std::call_once(g_coopUiServiceOnce, [] { g_coopUiService = std::thread(serviceThread); });
}

} // namespace

// The VR thread's two entry points, and the state it draws. Defined at Coop
// scope because the header promises external linkage; everything they touch
// lives in the file-local block above.

std::atomic<int> g_coopUiState(kUiIdle);
std::atomic<bool> g_coopUiLeaveWanted(false);
char g_coopUiStatus[96] = "";
char g_coopUiRoomLine[64] = "";
std::atomic<unsigned> g_coopUiNoticeSeq(0);
char g_coopUiNotice[96] = "";

void requestLeaveRoom() { g_coopUiLeaveWanted = true; }

void requestJoinRoom(const char *code, const char *invite) {
	ensureUiService(); // starts the listener on first use, from the VR thread
	std::lock_guard<std::mutex> lock(g_coopUiReq.mutex);
	g_coopUiReq.create = false; // a join can follow a create on the same struct
	g_coopUiReq.pending = true;
	strncpy(g_coopUiReq.code, code, sizeof(g_coopUiReq.code) - 1);
	g_coopUiReq.code[sizeof(g_coopUiReq.code) - 1] = 0;
	strncpy(g_coopUiReq.invite, invite, sizeof(g_coopUiReq.invite) - 1);
	g_coopUiReq.invite[sizeof(g_coopUiReq.invite) - 1] = 0;
	g_coopUiReq.cond.notify_all();
}

void requestCreateRoom(const char *name) {
	ensureUiService();
	std::lock_guard<std::mutex> lock(g_coopUiReq.mutex);
	g_coopUiReq.create = true;
	g_coopUiReq.pending = true;
	strncpy(g_coopUiReq.code, name, sizeof(g_coopUiReq.code) - 1);
	g_coopUiReq.code[sizeof(g_coopUiReq.code) - 1] = 0;
	g_coopUiReq.invite[0] = 0; // an open room needs no credential to join
	g_coopUiReq.cond.notify_all();
}

// The render hand-off. Defined here (inside namespace Coop, outside the
// file-local one) because the producer side belongs with whoever drives the
// decoder; the VR thread links against grabVideoFrame.
VideoMailbox g_coopVideo;
volatile bool g_coopVideoDisplayWant = false;
volatile bool g_coopClockSynced = false;
volatile int64 g_coopClockOffsetUs = 0;

void publishVideoFrame(const byte *plane, int len, int64 decodedAtUs,
	int width, int height, int stride, int sliceHeight, uint32 frameId) {
	if (!g_coopVideoDisplayWant || width <= 0 || height <= 0)
		return;
	// NV12 as the codec laid it out: a stride x sliceHeight luma plane plus
	// an interleaved UV plane of the same stride and half the slice height.
	// Anything else this unit has not seen from a Quest codec, so say so
	// once through the counters rather than copying bytes we would misread.
	if (stride * sliceHeight + stride * (sliceHeight / 2) > len)
		return;

	std::lock_guard<std::mutex> lock(g_coopVideo.m);
	++g_coopVideo.stats.decodedTo;
	const size_t bytes = (size_t)stride * sliceHeight * 3 / 2;
	if (g_coopVideo.frame.size() != bytes)
		g_coopVideo.frame.resize(bytes);
	memcpy(g_coopVideo.frame.data(), plane, bytes);
	g_coopVideo.width = width;
	g_coopVideo.height = height;
	g_coopVideo.stride = stride;
	g_coopVideo.sliceHeight = sliceHeight;
	g_coopVideo.frameId = frameId;
	g_coopVideo.publishedAtUs = decodedAtUs;
	g_coopVideo.haveNew = true;
	++g_coopVideo.seq;
}

bool grabVideoFrame(VideoMailbox *out, uint32 lastSeq, VideoRenderStats *stats) {
	std::lock_guard<std::mutex> lock(g_coopVideo.m);
	if (!g_coopVideo.seq || g_coopVideo.seq == lastSeq)
		return false;
	out->frame.swap(g_coopVideo.frame); // leaves the source empty; re-resized on the next publish
	out->seq = g_coopVideo.seq;
	out->publishedAtUs = g_coopVideo.publishedAtUs;
	out->width = g_coopVideo.width;
	out->height = g_coopVideo.height;
	out->stride = g_coopVideo.stride;
	out->sliceHeight = g_coopVideo.sliceHeight;
	out->frameId = g_coopVideo.frameId;

	stats->decodedTo = g_coopVideo.stats.decodedTo;
	++g_coopVideo.stats.rendered;
	stats->rendered = g_coopVideo.stats.rendered;
	const int64 now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	stats->decodeToUploadUs = now - g_coopVideo.publishedAtUs;
	stats->uploadWallUs = now;
	stats->uploadFrameId = g_coopVideo.frameId;
	g_coopVideo.latUs.push_back(stats->decodeToUploadUs);
	return true;
}

// The game the marker asked to autolaunch, kept outside the per-file Config
// because android-vr-main reads it (to extend the scummvm_main command line)
// while the run thread may not have even started yet. Set synchronously in
// startSelfTestIfRequested; "" means leave the launcher alone.
char g_coopGameTarget[64];

const char *selfTestGameTarget() { return g_coopGameTarget; }

bool startSelfTestIfRequested() {
	const char *candidates[3];
	int count = 0;
	const char *fromEnv = getenv("COOP_SELFTEST_FILE");
	if (fromEnv && fromEnv[0])
		candidates[count++] = fromEnv;
	for (int i = 0; i < (int)(sizeof(kDefaultMarkerPaths) / sizeof(kDefaultMarkerPaths[0])); ++i)
		candidates[count++] = kDefaultMarkerPaths[i];

	for (int i = 0; i < count; ++i) {
		Config cfg;
		if (!readConfig(candidates[i], &cfg))
			continue;
		// Synchronous, before the thread: the caller of this function may
		// read selfTestGameTarget() immediately, to decide the command line.
		memcpy(g_coopGameTarget, cfg.gameTarget, sizeof(g_coopGameTarget));
		// Its own thread: the render loop must not stall while the control plane
		// round trip runs, and Session callbacks arrive on another thread anyway.
		// Nothing is logged here because the caller runs before the backend is
		// initialised; runTest says when it found the file.
		std::thread(runTest, cfg).detach();
		return true;
	}
	return false;
}

} // namespace Coop
