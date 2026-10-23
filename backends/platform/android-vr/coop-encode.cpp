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

// Implementation notes worth keeping with the code:
//
// Buffer-queue mode, synchronous. No asynchronous callbacks: the only thread
// that touches the codec is the one submitting frames, so there is no state to
// lock, and the latency measurement wants timestamps this thread owns.
//
// The format keys mirror the design (low latency, no B-frames, periodic IDR)
// rather than exposing them as settings. "max-bframes" is what makes the
// no-B-frames claim explicit: the hardware default happens to be 0 today, and
// defaults are the kind of thing that changes with a driver update.

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/android-vr/coop-encode.h"

#include "common/debug.h"
#include "common/textconsole.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <vector>

namespace Coop {

namespace {

// MediaFormat keys without an NDK constant at this API level. Same strings the
// Java layer uses; keeping them literal is what lets the code build against
// older NDK headers.
const char *const kKeyMaxBFrames = "max-bframes";
const char *const kKeyForceSyncFrame = "force-sync-frame";
const int kBitrateModeLowLatency = 2; // MEDIA_BITRATE_LOW_LATENCY

// Java API's BUFFER_FLAG_SYNC_FRAME, value 1. The NDK enum stops at
// CODEC_CONFIG/END_OF_STREAM/PARTIAL_FRAME and never names this one, but the
// flag rides through in AMediaCodecBufferInfo.flags from the same native
// pipeline, and it is how a keyframe is identified in NDK encoder code.
const int kFlagSyncFrame = 1;

// Two media APIs newer than this build's minimum Android version (getInput-
// Format is 28, setParameters is 26; the app builds against 21). The NDK
// marks them unavailable at that level, so they are resolved at runtime
// instead: on any device that actually runs this VR app they are present, and
// on an older one the caller sees a missing capability rather than a link
// failure. This is the standard NDK pattern and it is why the minSdk does not
// have to move for the video path. (The bitrate-mode format key, also API-28-
// named, is passed as its literal string - keys are runtime data anyway.)
typedef media_status_t (*SetInt32Fn)(AMediaFormat *, const char *, int32_t);
typedef AMediaFormat *(*GetInputFormatFn)(AMediaCodec *);
typedef media_status_t (*SetParametersFn)(AMediaCodec *, const AMediaFormat *);

template <typename T>
T loadMediaSymbol(const char *name) {
	static void *handle = nullptr;
	if (!handle)
		handle = dlopen("libmediandk.so", RTLD_NOW);
	return handle ? reinterpret_cast<T>(dlsym(handle, name)) : nullptr;
}

bool isKeyFormat(int32_t color) {
	return color == VideoEncoder::kInputI420 || color == VideoEncoder::kInputNV12;
}

int64 monotonicUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

VideoEncoder::VideoEncoder()
	: _codec(nullptr), _width(0), _height(0), _inputFormat(kInputUnknown),
	  _stride(0), _sliceHeight(0), _csdLen(0), _inputDrops(0) {
}

VideoEncoder::~VideoEncoder() {
	close();
}

bool VideoEncoder::open(int width, int height, int fps, int bitrateBps, int idrIntervalMs) {
	close();
	if (width <= 0 || height <= 0 || fps <= 0 || bitrateBps <= 0 || idrIntervalMs <= 0)
		return false;

	_codec = AMediaCodec_createEncoderByType("video/avc");
	if (!_codec) {
		warning("coop encode: no video/avc encoder on this device");
		return false;
	}

	AMediaFormat *fmt = AMediaFormat_new();
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrateBps);
	// The interval key is float seconds. Rounding a sub-second request to 0
	// would mean "every frame", so clamp upward before the codec sees it.
	float idrSeconds = idrIntervalMs / 1000.0f;
	if (idrSeconds < 0.5f)
		idrSeconds = 0.5f;
	AMediaFormat_setFloat(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, idrSeconds);
	// Ask for the input layout explicitly instead of letting the codec pick.
	// Left to itself the Quest codec answered with a vendor format
	// (0x7F420A88) that this path cannot fill - observed 2026-10-23. NV12 is
	// the one layout the MediaCodec contract guarantees hardware encoders
	// accept; the capture path is expected to produce it (the one interleave
	// from a linear readback is the caller's cost, paid knowingly).
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, kInputNV12);
	// "bitrate-mode" has no named constant below API 28; the literal is the
	// same key the named constant resolves to. Rate control mode is not a
	// tunable here - low-latency CBR is the design.
	AMediaFormat_setInt32(fmt, "bitrate-mode", kBitrateModeLowLatency);
	AMediaFormat_setInt32(fmt, kKeyMaxBFrames, 0);
	// The first device measurement showed the pipeline holding exactly two
	// frames (p50 67 ms at 30 fps), which is pipeline depth, not codec speed.
	// These are the requests for a shallower one: KEY_LATENCY (API 31, named
	// "latency") is the platform's own ask, and "low-latency.enable" is the
	// vendor-channel convention the hardware encoders listen on (the Java
	// layer prefixes it with "vendor."; the native format does not). Unknown
	// keys are ignored by codecs that do not implement them - worth the
	// three lines when the alternative is 67 ms of standing latency.
	AMediaFormat_setInt32(fmt, "latency", 1);
	AMediaFormat_setInt32(fmt, "low-latency.enable", 1);
	AMediaFormat_setInt32(fmt, "vendor.low-latency.enable", 1);

	media_status_t st = AMediaCodec_configure(_codec, fmt, nullptr, nullptr,
		AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	AMediaFormat_delete(fmt);
	if (st != AMEDIA_OK) {
		warning("coop encode: configure(%d x %d @ %d fps, %d bps) failed (%d)",
			width, height, fps, bitrateBps, (int)st);
		close();
		return false;
	}

	// Ask the codec what it actually wants, do not assume - and reject it
	// honestly when it does not want something this path can produce. Along
	// with the color layout, read the stride and slice height: hardware
	// encoders pad rows, and copying a tightly packed frame into a padded
	// buffer without accounting for that smears every row into the next.
	_inputFormat = kInputUnknown;
	_stride = width;
	_sliceHeight = height;
	const GetInputFormatFn getInputFormat = loadMediaSymbol<GetInputFormatFn>("AMediaCodec_getInputFormat");
	if (getInputFormat) {
		AMediaFormat *inFmt = getInputFormat(_codec);
		if (inFmt) {
			int32_t color = 0;
			if (AMediaFormat_getInt32(inFmt, "color-format", &color) && isKeyFormat(color))
				_inputFormat = static_cast<InputFormat>(color);
			int32_t stride = 0, slice = 0;
			if (AMediaFormat_getInt32(inFmt, "stride", &stride) && stride >= width)
				_stride = stride;
			if (AMediaFormat_getInt32(inFmt, "slice-height", &slice) && slice >= height)
				_sliceHeight = slice;
			AMediaFormat_delete(inFmt);
		}
	}
	if (_inputFormat == kInputUnknown) {
		// Reaching here means the codec ignored the explicit NV12 request (or
		// getInputFormat is too old to ask). Assuming a layout is exactly the
		// mistake that produced the first device failure, so refuse instead:
		// the caller's report is then "no usable encoder", which is true.
		warning("coop encode: codec did not settle on I420 or NV12 input; refusing to guess");
		close();
		return false;
	}

	if (AMediaCodec_start(_codec) != AMEDIA_OK) {
		warning("coop encode: start failed");
		close();
		return false;
	}

	_width = width;
	_height = height;
	_csdLen = 0;
	// Read the codec's own latency claim for the record. "latency" in the
	// output format is the codec telling how many pipeline frames it holds;
	// if that number matches the measured submit-to-AU depth, the delay is
	// the codec's design and no client-side tuning will move it.
	int claimedLatency = -1;
	// getOutputFormat before any output exists is not guaranteed to answer;
	// failing to read it changes nothing except what the log can say.
	if (AMediaFormat *outFmt = AMediaCodec_getOutputFormat(_codec)) {
		int32_t lat = 0;
		if (AMediaFormat_getInt32(outFmt, "latency", &lat))
			claimedLatency = (int)lat;
		AMediaFormat_delete(outFmt);
	}
	debug("coop encode: video/avc online, %d x %d @ %d fps, input %s, codec layout "
			"stride %d slice %d, codec claims latency %d frames", width, height, fps,
		_inputFormat == kInputI420 ? "I420" : "NV12", _stride, _sliceHeight, claimedLatency);
	return true;
}

void VideoEncoder::close() {
	if (!_codec)
		return;
	AMediaCodec_stop(_codec);
	AMediaCodec_delete(_codec);
	_codec = nullptr;
	_width = _height = 0;
	_inputFormat = kInputUnknown;
	_stride = _sliceHeight = 0;
	_csdLen = 0;
}

bool VideoEncoder::submitFrame(const byte *frame, int64 ptsUs, AuCallback cb, void *ctx) {
	if (!_codec || !frame)
		return false;

	const int packed = inputBytes(); // what the caller handed us
	ssize_t inIdx = AMediaCodec_dequeueInputBuffer(_codec, 2000); // us
	if (inIdx < 0) {
		// The encoder is behind. Dropping is the correct response - holding a
		// backlog is how a live video path turns its own latency into a
		// runaway - but it must be loud, because the symptom downstream is
		// missing frames with no error anywhere.
		debug("coop encode: no input buffer, dropped the frame at pts %lld",
			(long long)ptsUs);
		++_inputDrops;
	} else {
		size_t bufSize = 0;
		uint8_t *buf = AMediaCodec_getInputBuffer(_codec, inIdx, &bufSize);
		// The buffer is sized by the codec's stride and slice height, not by
		// the frame's - on a padded codec a packed-size comparison passes and
		// then the row copy runs off the end.
		const size_t padded = (size_t)_stride * _sliceHeight * 3 / 2;
		if (!buf || bufSize < padded) {
			warning("coop encode: input buffer is %zu bytes, codec layout needs %zu "
					"(%d x %d, stride %d, slice %d)", bufSize, padded, _width, _height,
					_stride, _sliceHeight);
			close();
			return false;
		}
		if (_stride == _width && _sliceHeight == _height) {
			memcpy(buf, frame, packed);
		} else {
			// Pad row by row into the codec's layout. Chroma rows follow the
			// same stride rule; the destination planes start at sliceHeight
			// rows, which is exactly where a padded codec looks for them.
			for (int row = 0; row < _height; ++row)
				memcpy(buf + (size_t)row * _stride, frame + (size_t)row * _width, _width);
			// Rows between height and sliceHeight are inside the buffer but
			// outside the crop; leaving another frame's pixels there feeds the
			// encoder motion it should not see. Black is the honest filler.
			for (int row = _height; row < _sliceHeight; ++row)
				memset(buf + (size_t)row * _stride, 16, _stride);
			const byte *srcC = frame + (size_t)_width * _height;
			uint8_t *dstC = buf + (size_t)_stride * _sliceHeight;
			const int chromaRow = (_inputFormat == kInputNV12) ? _width : _width / 2;
			const int chromaRows = (_inputFormat == kInputNV12) ? _height / 2 : _height;
			for (int row = 0; row < chromaRows; ++row)
				memcpy(dstC + (size_t)row * _stride, srcC + (size_t)row * chromaRow, chromaRow);
			for (int row = chromaRows; row < _sliceHeight / 2; ++row)
				memset(dstC + (size_t)row * _stride, 128, _stride);
		}
		if (AMediaCodec_queueInputBuffer(_codec, inIdx, 0, (size_t)padded,
				static_cast<int64_t>(ptsUs), 0) != AMEDIA_OK) {
			warning("coop encode: queueInputBuffer failed");
			close();
			return false;
		}
	}

	// Take what is ready without waiting. Latency for this caller is measured
	// end to end (submit to AU callback), and a blocking dequeue here would
	// hide the queuing inside the call.
	for (;;) {
		AMediaCodecBufferInfo info;
		const ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(_codec, &info, 0);
		if (outIdx < 0)
			break; // nothing more ready (or a format change, picked up on next call)

		size_t bufSize = 0;
		uint8_t *buf = AMediaCodec_getOutputBuffer(_codec, outIdx, &bufSize);
		if (buf && info.size > 0 && static_cast<size_t>(info.size) <= bufSize)
			emitOut(buf, info.size, info.flags, static_cast<int64_t>(info.presentationTimeUs), cb, ctx);
		const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
		AMediaCodec_releaseOutputBuffer(_codec, outIdx, false);
		if (eos)
			break;
	}
	return true;
}

void VideoEncoder::emitOut(const uint8_t *buf, int size, int flags, int64 ptsUs,
	AuCallback cb, void *ctx) {
	if (flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
		if (size <= kMaxCsdBytes) {
			memcpy(_csd, buf, size);
			_csdLen = size;
		}
		return;
	}
	const bool key = (flags & kFlagSyncFrame) != 0;
	if (key && _csdLen > 0) {
		std::vector<byte> withCsd;
		withCsd.reserve(_csdLen + size);
		withCsd.insert(withCsd.end(), _csd, _csd + _csdLen);
		withCsd.insert(withCsd.end(), buf, buf + size);
		cb(ctx, withCsd.data(), (int)withCsd.size(), kAuKeyframe, ptsUs);
	} else {
		cb(ctx, buf, size, key ? kAuKeyframe : 0, ptsUs);
	}
}

bool VideoEncoder::drain(int maxMs, AuCallback cb, void *ctx) {
	if (!_codec)
		return false;
	const int64 deadline = monotonicUs() + maxMs * 1000;
	for (;;) {
		const int64 left = deadline - monotonicUs();
		if (left <= 0)
			return true;
		AMediaCodecBufferInfo info;
		const ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(_codec, &info,
			left > 50000 ? 50000 : left);
		if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			continue; // the pipeline still holds frames; that is what this call is for
		if (outIdx < 0)
			return true; // drained, or the codec changed format under us
		size_t bufSize = 0;
		uint8_t *buf = AMediaCodec_getOutputBuffer(_codec, outIdx, &bufSize);
		if (buf && info.size > 0 && static_cast<size_t>(info.size) <= bufSize)
			emitOut(buf, info.size, info.flags, static_cast<int64_t>(info.presentationTimeUs), cb, ctx);
		const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
		AMediaCodec_releaseOutputBuffer(_codec, outIdx, false);
		if (eos)
			return true;
	}
}

bool VideoEncoder::requestKeyframe() {
	if (!_codec)
		return false;
	// setParameters is API 26 and this build's minimum is 21, so it is
	// resolved at runtime; the force-sync-frame key it needs is Android 12+
	// anyway. Either way a refusal is not fatal: the periodic IDR bounds how
	// long any participant can wait for a keyframe, and it is why IDR
	// requests over `X` are an optimisation and not the recovery mechanism.
	const SetParametersFn setParameters = loadMediaSymbol<SetParametersFn>("AMediaCodec_setParameters");
	if (!setParameters) {
		debug("coop encode: no setParameters (API < 26); the next periodic IDR will do");
		return false;
	}
	AMediaFormat *params = AMediaFormat_new();
	AMediaFormat_setInt32(params, kKeyForceSyncFrame, 1);
	const media_status_t st = setParameters(_codec, params);
	AMediaFormat_delete(params);
	if (st != AMEDIA_OK) {
		debug("coop encode: forced IDR refused (%d); the next periodic IDR will do", (int)st);
		return false;
	}
	return true;
}

} // namespace Coop
