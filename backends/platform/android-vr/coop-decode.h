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

// Guest-side H.264 decoder for the coop video path: the mirror of coop-encode.h.
// Input is the Annex-B access units the host sent (reassembled from the wire by
// the caller - this unit knows nothing about rooms, sealing, or fragmentation);
// output is decoded frames through a callback, on the thread that feeds input.
//
// Byte-buffer output, deliberately: the proof this unit exists for is "the guest
// decoded what the host encoded", which needs pixel buffers, not a surface. The
// eventual VR render path may prefer a Surface input for zero-copy; that is a
// different unit and a different risk, and mixing the two here would make the
// measured decode number mean two things at once.
//
// The decoder must survive what the wire actually delivers: a join that starts
// mid-stream (the first AU is not a keyframe), and a keyframe whose SPS/PPS
// arrived attached to it. The first is the caller's problem to avoid (it asks
// for an IDR when it starts), but this unit reports it rather than silently
// producing nothing.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_DECODE_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_DECODE_H

#include "common/types.h"

struct AMediaCodec;
struct AMediaFormat;

namespace Coop {

class VideoDecoder {
public:
	// Called once per decoded frame on the thread that called push(). `plane`
	// is the codec's output buffer, valid only for the call; the layout is
	// whatever outputFormat() reported (the codec's, not ours). frameIndex is
	// the pts that was pushed with the AU - the caller's key back to receive
	// time, which is what the latency measurement needs.
	typedef void (*FrameCallback)(void *ctx, const byte *plane, int len, int64 ptsUs);

	VideoDecoder();
	~VideoDecoder();

	/**
	 * Create, configure and start a video/avc decoder. The dimensions are the
	 * *expected* stream size: the c2.* decoder family on the Quest rejects a
	 * mime-only configure outright (-10000, observed on device 2026-10-23),
	 * so a decoder cannot be opened purely from the bitstream the way the
	 * encoder side is. They are an expectation, not an assumption - the
	 * format change published with the first frame overwrites them, and the
	 * stats report what the bitstream actually said.
	 */
	bool open(int width, int height);
	void close();
	bool isOpen() const { return _codec != nullptr; }

	// The output description the codec published with its first frame. Zero
	// until that first frame exists. colorFormat/stride/sliceHeight are the
	// codec's choices, not negotiated ones - the render path adapts to them.
	int width() const { return _outWidth; }
	int height() const { return _outHeight; }
	int colorFormat() const { return _outColor; }
	int stride() const { return _outStride; }
	int sliceHeight() const { return _outSlice; }

	/**
	 * Feed one complete Annex-B access unit with the pts the wire carried.
	 * Non-blocking on the decoder: whatever output is ready is emitted through
	 * cb before returning. Returns false on a codec error only.
	 */
	bool push(const byte *au, int len, int64 ptsUs, FrameCallback cb, void *ctx);

	/**
	 * Keep taking outputs for up to maxMs. The pipeline holds a frame or two
	 * after the last AU; not draining them loses the tail of every run.
	 */
	bool drain(int maxMs, FrameCallback cb, void *ctx);

	// Counters the self-test reports: AUs accepted, frames emitted, and AUs
	// that produced no output for a full second of wall time (the symptom of
	// a stream that started mid-GOP, told honestly instead of a silent zero).
	int pushed() const { return _pushed; }
	int decoded() const { return _decoded; }

private:
	void takeOutput(int timeoutUs, FrameCallback cb, void *ctx);
	void readOutputFormat();

	AMediaCodec *_codec;
	int _outWidth, _outHeight, _outColor, _outStride, _outSlice;
	int _pushed, _decoded;
};

} // namespace Coop

#endif
