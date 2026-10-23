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

// Host-side H.264 encoder for the coop video path. MediaCodec through the NDK C
// API, configured the way the latency budget needs it: constant low-latency rate
// control, no B-frames, periodic IDR. Input is the raw frame the capture path
// produced; output is whole access units in Annex-B form, which is what the
// guest's decoder wants and what Transport::sendMedia fragments onto the wire.
//
// This unit deliberately knows nothing about the room: no transport, no sealing,
// no relay. The caller decides which access unit becomes which 'V' frame. It
// also knows nothing about where frames come from - the bench feeds synthetic
// content, later the PBO readback will - because the encoder's measured cost is
// the same either way and the latency gate needs it measured first.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_ENCODE_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_ENCODE_H

#include "common/types.h"

// Opaque NDK handles; forward-declared so this header does not drag the media
// headers into every translation unit that ships video.
struct AMediaCodec;
struct AMediaFormat;

namespace Coop {

class VideoEncoder {
public:
	// The layout the codec actually accepts for input, read back from the
	// configured codec in open() - Android encoders vary between these two, so
	// callers must not assume I420. Frames are width*height*3/2 bytes in the
	// reported layout (the AOSP COLOR_FormatYUV420* numeric values).
	enum InputFormat {
		kInputUnknown = 0,
		kInputI420 = 19, // COLOR_FormatYUV420Planar
		kInputNV12 = 21, // COLOR_FormatYUV420SemiPlanar
	};

	// Set on every access unit that contains an IDR (the codec's SYNC_FRAME
	// flag). Such units already have the stored SPS/PPS prepended: on Android a
	// hardware encoder typically emits codec config once, out of band, and a
	// decoder that never saw it cannot start.
	static const uint32 kAuKeyframe = 1u << 0;

	// Called once per complete access unit, on the thread that called
	// submitFrame() or drain(). `au` is valid only for the call. ptsUs is the
	// presentation timestamp that was submitted with the frame - it is the
	// caller's key for matching outputs back to capture time, which is exactly
	// what the latency measurement needs.
	typedef void (*AuCallback)(void *ctx, const byte *au, int len, uint32 flags, int64 ptsUs);

	VideoEncoder();
	~VideoEncoder();

	/**
	 * Create, configure and start the encoder.
	 *
	 * idrIntervalMs is the periodic keyframe distance the protocol wants
	 * (1000-2000 ms for a room that loses datagrams); the NDK key is in
	 * seconds, so fractional values are accepted and rounded by the codec.
	 * Low-latency rate control and no B-frames are set here unconditionally -
	 * they are not tunables, they are the design.
	 */
	bool open(int width, int height, int fps, int bitrateBps, int idrIntervalMs);
	void close();
	bool isOpen() const { return _codec != nullptr; }

	InputFormat inputFormat() const { return _inputFormat; }
	int width() const { return _width; }
	int height() const { return _height; }
	int inputBytes() const { return _width * _height * 3 / 2; }

	/**
	 * Hand one raw frame to the codec, then take whatever outputs are ready
	 * (non-blocking). Returns false only on a codec error. If the encoder is
	 * behind, this frame is dropped rather than queued - inputDrops() counts
	 * those, and a live path that shows drops is telling you its bitrate or
	 * resolution is over budget.
	 */
	bool submitFrame(const byte *frame, int64 ptsUs, AuCallback cb, void *ctx);
	int inputDrops() const { return _inputDrops; }

	/**
	 * Keep taking outputs for up to maxMs. After the last frame, a pipeline
	 * still holds a frame or two; not draining them understates what the
	 * encoder produced and makes the last latency samples a lie.
	 */
	bool drain(int maxMs, AuCallback cb, void *ctx);

	/**
	 * Force the next encoded frame to be an IDR, for a guest's keyframe
	 * request. "force-sync-frame" is Android 12+; on anything older this
	 * returns false and the next periodic IDR is the recovery, which is why
	 * the periodic interval is never allowed to be large.
	 */
	bool requestKeyframe();

private:
	static const int kMaxCsdBytes = 2048; // SPS/PPS for this profile fit with room to spare

	// Hand one ready output buffer to the callback, prepending stored codec
	// config on key units. Shared by submitFrame()'s non-blocking drain and
	// drain()'s waiting one.
	void emitOut(const uint8_t *buf, int size, int flags, int64 ptsUs, AuCallback cb, void *ctx);

	AMediaCodec *_codec;
	int _width;
	int _height;
	InputFormat _inputFormat;

	// The codec's actual row padding, read back in open(). Frames handed to
	// submitFrame are tightly packed (inputBytes()); the copy into the codec
	// buffer applies this padding.
	int _stride;
	int _sliceHeight;

	// Codec config (SPS/PPS) as last emitted out-of-band, to be prepended to
	// key AUs. The in-band-vs-out-of-band choice is the codec's, not ours.
	byte _csd[kMaxCsdBytes];
	int _csdLen;
	int _inputDrops;
};

} // namespace Coop

#endif
