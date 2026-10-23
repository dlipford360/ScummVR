/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
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

// Buffer-queue mode, synchronous - same discipline as the encoder: one thread
// owns the codec, no locks, and the pts values this thread chose are the ones
// that come back, which is what makes the guest-side latency number possible.
//
// The decoder takes width/height at configure because the device's c2.*
// decoder refuses anything less - mime-only configure returned -10000 on the
// Quest (observed 2026-10-23). What it does not take is a csd: the
// bitstream's own SPS/PPS (which coop-encode prepends to key units, and any
// sane host does too) is what actually configures it, and the output format
// published with the first frame is what the rest of the code trusts.

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/android-vr/coop-decode.h"

#include "common/debug.h"
#include "common/textconsole.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <chrono>

namespace Coop {

namespace {

int64 monotonicUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VideoDecoder::VideoDecoder()
	: _codec(nullptr), _outWidth(0), _outHeight(0), _outColor(0), _outStride(0),
	  _outSlice(0), _pushed(0), _decoded(0) {
}

VideoDecoder::~VideoDecoder() {
	close();
}

bool VideoDecoder::open(int width, int height) {
	close();
	if (width <= 0 || height <= 0)
		return false;
	_codec = AMediaCodec_createDecoderByType("video/avc");
	if (!_codec) {
		warning("coop decode: no video/avc decoder on this device");
		return false;
	}

	// Width and height here, but no csd: the bitstream's own SPS/PPS (which
	// coop-encode prepends to key units, and any sane host does too) is what
	// configures the codec at the first key unit. The dimensions are needed
	// only because the c2.* decoders refuse to be created without them; the
	// first frame's format change remains the real description (see
	// readOutputFormat).
	AMediaFormat *fmt = AMediaFormat_new();
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
	const media_status_t st = AMediaCodec_configure(_codec, fmt, nullptr, nullptr, 0);
	AMediaFormat_delete(fmt);
	if (st != AMEDIA_OK) {
		warning("coop decode: decoder configure failed (%d)", (int)st);
		close();
		return false;
	}
	if (AMediaCodec_start(_codec) != AMEDIA_OK) {
		warning("coop decode: decoder start failed");
		close();
		return false;
	}
	debug("coop decode: video/avc decoder online (byte-buffer output)");
	return true;
}

void VideoDecoder::close() {
	if (!_codec)
		return;
	AMediaCodec_stop(_codec);
	AMediaCodec_delete(_codec);
	_codec = nullptr;
	_outWidth = _outHeight = _outColor = _outStride = _outSlice = 0;
	_pushed = _decoded = 0;
}

void VideoDecoder::readOutputFormat() {
	// The codec publishes width/height/crop/color/stride with its first
	// output; everything downstream (statistics now, the renderer later)
	// reads these instead of trusting the host's settings.
	AMediaFormat *fmt = AMediaCodec_getOutputFormat(_codec);
	if (!fmt)
		return;
	int32_t v = 0;
	if (AMediaFormat_getInt32(fmt, "width", &v))
		_outWidth = v;
	if (AMediaFormat_getInt32(fmt, "height", &v))
		_outHeight = v;
	if (AMediaFormat_getInt32(fmt, "color-format", &v))
		_outColor = v;
	if (AMediaFormat_getInt32(fmt, "stride", &v))
		_outStride = v;
	else
		_outStride = _outWidth;
	if (AMediaFormat_getInt32(fmt, "slice-height", &v))
		_outSlice = v;
	else
		_outSlice = _outHeight;
	AMediaFormat_delete(fmt);
	debug("coop decode: output %d x %d (stride %d slice %d, color %d)",
		_outWidth, _outHeight, _outStride, _outSlice, _outColor);
}

void VideoDecoder::takeOutput(int timeoutUs, FrameCallback cb, void *ctx) {
	for (;;) {
		AMediaCodecBufferInfo info;
		const ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(_codec, &info, timeoutUs);
		if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			readOutputFormat();
			timeoutUs = 0; // format read; keep draining what is already there
			continue;
		}
		if (outIdx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			continue; // buffer indices changed; there is nothing to hand back yet
		if (outIdx < 0)
			return; // TRY_AGAIN_LATER or an error: nothing ready

		size_t bufSize = 0;
		uint8_t *buf = AMediaCodec_getOutputBuffer(_codec, outIdx, &bufSize);
		if (buf && info.size > 0 && static_cast<size_t>(info.size) <= bufSize) {
			++_decoded;
			cb(ctx, buf, info.size, static_cast<int64_t>(info.presentationTimeUs));
		}
		const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
		AMediaCodec_releaseOutputBuffer(_codec, outIdx, false);
		if (eos)
			return;
		timeoutUs = 0; // took one frame; any further wait is for the *next* call
	}
}

bool VideoDecoder::push(const byte *au, int len, int64 ptsUs, FrameCallback cb, void *ctx) {
	if (!_codec || !au || len <= 0)
		return false;

	// A decoder input buffer is practically always available when the caller
	// feeds at wire rate; waiting here (rather than dropping like the encoder
	// does) is right because unlike a capture path, a received frame cannot be
	// recreated locally - dropping it is a hole the guest would see.
	ssize_t inIdx = -1;
	for (int tries = 0; inIdx < 0 && tries < 5; ++tries) {
		inIdx = AMediaCodec_dequeueInputBuffer(_codec, 20000); // us
		if (inIdx < 0)
			takeOutput(0, cb, ctx); // maybe it is blocked behind un-dequeued output
	}
	if (inIdx < 0) {
		warning("coop decode: no input buffer after 5 tries; the decoder is stuck");
		return false;
	}

	size_t bufSize = 0;
	uint8_t *buf = AMediaCodec_getInputBuffer(_codec, inIdx, &bufSize);
	if (!buf || bufSize < static_cast<size_t>(len)) {
		warning("coop decode: input buffer %zu bytes, AU needs %d", bufSize, len);
		return false;
	}
	memcpy(buf, au, len);
	if (AMediaCodec_queueInputBuffer(_codec, inIdx, 0, len,
			static_cast<int64_t>(ptsUs), 0) != AMEDIA_OK) {
		warning("coop decode: queueInputBuffer failed");
		return false;
	}
	++_pushed;
	takeOutput(0, cb, ctx);
	return true;
}

bool VideoDecoder::drain(int maxMs, FrameCallback cb, void *ctx) {
	if (!_codec)
		return false;
	const int64 deadline = monotonicUs() + (int64)maxMs * 1000;
	for (;;) {
		const int64 left = deadline - monotonicUs();
		if (left <= 0)
			return true;
		takeOutput(left > 50000 ? 50000 : (int)left, cb, ctx);
	}
}

} // namespace Coop
