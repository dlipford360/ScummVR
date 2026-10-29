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

// Game audio over the room: the audio mirror of coop-encode/coop-decode.
//
// Host side: a tap on the platform's mixed output
// (backends/platform/android/audio-tap.h) feeds an SPSC ring; pump(), called
// from the room loop, resamples 44.1 -> 48 kHz and emits fixed 20 ms Opus
// frames as 'A' media frames. Fixed size and fixed cadence are deliberate:
// the guest's gap detector is arithmetic (a missing seq means exactly 20 ms),
// and Opus packet-loss concealment is built for exactly that hole.
//
// Guest side: reassembled 'A' payloads are decoded into a PCM ring exposed as
// an Audio::AudioStream and started once through Mixer::playStream, so the
// game audio plays through the same output path any other game stream uses
// (the mixer rate-converts 48k -> the device's 44.1k; see client guide §9).
// Loss policy, in order: the sender's inband FEC reconstructs the frame just
// before a gap exactly, from the packet that proves the gap; frames further
// back are concealed by PLC; long gaps get silence. A ring that grows past
// its latency bound drops oldest samples, which is how the two headsets'
// crystal drift is absorbed without either clock being trusted.
//
// Threading contract: the tap callback runs on the platform audio thread, and
// the AudioStream's readBuffer runs on that same audio thread when the mixer
// mixes; feedFrame() and pump() run on the room loop thread. All shared state
// is under one small mutex held for memcpy-length work only - the audio
// thread must never wait on a socket, and nothing here touches the network.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_GAMEAUDIO_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_GAMEAUDIO_H

// The standard-library headers go first on purpose: their system chains must
// be parsed before common/forbidden.h defines its macro traps.
#include <atomic>
#include <mutex>
#include <vector>

#include "common/types.h"

struct OpusEncoder;
struct OpusDecoder;

namespace Audio {
class AudioStream;
class SoundHandle;
}

namespace Coop {

class Transport;

class GameAudio {
public:
	GameAudio();
	~GameAudio();

	// ------------------------------------------------------------- host side

	// Create the Opus encoder and register the platform output tap. The tap
	// stays installed until stopSender(); while no room is running, pump()
	// simply drains and discards, which keeps the ring honest about real time.
	bool startSender(Transport *transport);
	void stopSender();

	// Call from the room loop every iteration. Internally paced to 20 ms:
	// emits nothing until a full frame of real audio has been collected.
	void pump();

	// ----------------------------------------------------------- guest side

	// Create the decoder and hand an AudioStream to the mixer (started once,
	// silence until frames arrive). Safe before the first 'A' frame.
	bool startReceiver();
	void stopReceiver();

	// One complete 'A' payload, after the caller's reassembler has assembled
	// the fragments. seq is the media frameId the sender used; the gap
	// arithmetic here is exactly that counter.
	void feedFrame(uint32 seq, const byte *opusBytes, int len);

	// -------------------------------------------------------------- counters

	struct Counters {
		// sender
		int64 tapBytes; // bytes the tap handed us
		int framesEncoded;
		int64 bytesSent;
		int sendFailures;
		int tapRingDrops; // input bytes dropped because the room loop stalled
		int encodeSkips; // 20 ms ticks with too little audio (should be ~0)
		// receiver
		int framesReceived;
		int framesDecoded;
		int lateDrops; // seq at or before the newest one already decoded
		int gapEvents;
		int plcFrames; // concealment frames inserted
		int fecFrames; // lost frames reconstructed from the next packet's inband FEC
		int silenceFrames; // long-gap padding, beyond PLC
		int driftDrops; // samples dropped to hold the latency bound
		int underruns; // readBuffer calls that found the ring empty
	};
	// A consistent snapshot for the run summary; the audio thread keeps
	// counting while this copies.
	Counters counters() const;

	bool senderActive() const { return _encoder != nullptr; }
	bool receiverActive() const { return _decoder != nullptr; }

private:
	// The platform tap thunk and the mixer-facing stream are the two doors
	// that are not class members; they see the internals by arrangement.
	friend void tapThunk(const byte *bytes, int byteCount);
	friend class GamePcmStream;

	// Platform-audio-thread entry point for the tap.
	void onOutputBytes(const byte *bytes, int byteCount);

	// Move one 20 ms frame from the input ring through the resampler to the
	// encoder and the transport. Returns false if the ring is short.
	bool emitFrame();

	// One interleaved stereo frame from the tap ring under its mutex.
	bool tapReadFrame(int16 *frame);

	// Append interleaved stereo frames to the PCM ring under its mutex.
	void pcmPush(const int16 *frames, int count);
	// Fill up to `count` interleaved stereo frames from the PCM ring, padding
	// with silence; always returns count. This is the mixer thread's door.
	int pcmRead(int16 *frames, int count);

	Counters _counters;

	// sender
	Transport *_transport;
	OpusEncoder *_encoder;
	std::atomic<bool> _senderActive;
	int _inRate; // the mixer's actual output rate the tap sees	// Interleaved int16 input at the mixer's rate, free-run sample indices,
	// sized in samples (not frames).
	std::vector<int16_t> _tapRing;
	uint64 _tapHead, _tapTail;
	// The resampler is stateful across frames and across pumps: a window of
	// two input frames at [pos] and [pos+1], pos kept in [0,1). A 20 ms
	// output is staged here whenever the ring goes short mid-frame, so a
	// skipped tick never smears a discontinuity into the next frame's start.
	double _resPos, _resStep;
	int16_t _resA[2], _resB[2];
	bool _resHaveWindow;
	static const int kFrameSamples = 960; // 20 ms at 48 kHz
	int16_t _resOut[kFrameSamples * 2];
	int _resOutFrames;
	uint32 _audioSeq;

	// receiver
	OpusDecoder *_decoder;
	Audio::AudioStream *_stream; // we own it; the mixer only borrows it
	Audio::SoundHandle *_handle; // created with the receiver, stopped by it
	uint64 _pcmHead, _pcmTail;
	std::vector<int16_t> _pcmRing;
	bool _haveLastSeq;
	uint32 _lastSeq;
	bool _wantFec; // a gap ended at the last packet: FEC-decode the next one

	mutable std::mutex _mtx; // guards both rings and the counters they feed
};

} // namespace Coop

#endif
