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

#include "backends/platform/android-vr/coop-gameaudio.h"

#include "backends/platform/android-vr/coop-transport.h"
#include "backends/platform/android/audio-tap.h"

#include "audio/audiostream.h"
#include "audio/mixer.h"
#include "common/system.h"
#include "common/debug.h"

#include "opus/opus.h"

#include <cstring>

namespace Coop {

// The Opus side is fixed and does not negotiate: 48 kHz stereo, 20 ms frames,
// ~32 kbps. Fixed because the guest's arithmetic depends on it: one sequence
// number is exactly one 20 ms hole, which is exactly the concealment Opus was
// designed for.
static const int kOpusRate = 48000;
static const int kOpusChannels = 2;
static const int kOpusBitrate = 32000;

// Host input ring: one second at the mixer's rate. The room loop only ever
// falls behind by milliseconds; a full ring means something stopped, and the
// oldest audio is the wrong thing to keep, so the writer drops oldest.
static const int kTapRingSeconds = 1;
// Guest PCM ring: 2 s at 48 kHz. The latency bound is a drop trigger, not an
// error: host and guest run on different crystals, and dropping the oldest
// audio when the buffer sags past the bound is how that drift disappears.
static const int kPcmRingSeconds = 2;
static const int kMaxBufferedFrames = 480; // ms before drift-drop
static const int kDrainToFrames = 240; // ms to keep when drift-dropping
// A gap longer than this is a gap, not a concealment: beyond ~100 ms of PLC
// the decoder's guess is worse than silence.
static const int kMaxPlcFrames = 5;

// The tap callback has no context parameter (a C contract from the platform),
// and there is exactly one GameAudio per process for as long as a sender is
// started - which the selftest's single-session design guarantees.
GameAudio *g_tapTarget = nullptr;

void tapThunk(const byte *bytes, int byteCount) {
	GameAudio *self = g_tapTarget;
	if (self)
		self->onOutputBytes(bytes, byteCount);
}

// The guest's stream object. It reports 48 kHz and lets the mixer's own
// rate converter land that on the device's rate - the same path any game
// stream takes, so no second resampler exists in this unit.
class GamePcmStream : public Audio::AudioStream {
public:
	explicit GamePcmStream(GameAudio *owner) : _owner(owner) {}

	// numSamples is interleaved sample count; stereo means /2 frames. Always
	// returns the full count: a short read here would end the stream, and a
	// live network stream never ends - the padding is silence instead.
	int readBuffer(int16 *buffer, const int numSamples) {
		_owner->pcmRead(buffer, numSamples / kOpusChannels);
		return numSamples;
	}

	bool isStereo() const { return true; }
	int getRate() const { return kOpusRate; }
	bool endOfData() const { return false; }

private:
	GameAudio *_owner;
};

GameAudio::GameAudio()
	: _counters(), _transport(nullptr), _encoder(nullptr), _senderActive(false),
	  _inRate(0), _tapHead(0), _tapTail(0), _resPos(0), _resStep(1),
	  _resHaveWindow(false), _resOutFrames(0), _audioSeq(0), _decoder(nullptr),
	  _stream(nullptr), _handle(nullptr), _pcmHead(0), _pcmTail(0),
	  _haveLastSeq(false), _lastSeq(0), _wantFec(false) {
}

GameAudio::~GameAudio() {
	stopSender();
	stopReceiver();
	delete _handle;
	delete _stream;
}

// ------------------------------------------------------------------ sender

bool GameAudio::startSender(Transport *transport) {
	if (_encoder || !transport)
		return false;

	int err = 0;
	_encoder = opus_encoder_create(kOpusRate, kOpusChannels, OPUS_APPLICATION_AUDIO, &err);
	if (!_encoder || err != OPUS_OK) {
		warning("coop gameaudio: opus encoder create failed (%d)", err);
		return false;
	}
	opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(kOpusBitrate));
	// Inband FEC: on the two runs that measured the downlink with real video
	// in flight, 13-18 % of these datagrams never arrived (rooms VWWGV,
	// C87CK) - audio loss IS this path's failure mode, and the listener
	// notices. FEC puts the previous frame's redundancy inside the NEXT
	// packet, so a lost frame usually reconstructs exactly instead of leaning
	// on concealment. The loss ratio is static at the measured rate because
	// there is no loss estimation on this path yet (§17); Opus stays inside
	// the bitrate cap either way.
	opus_encoder_ctl(_encoder, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(_encoder, OPUS_SET_PACKET_LOSS_PERC(15));

	int rate = 0, stereo = 0;
	Audio::Mixer *mixer = g_system->getMixer();
	if (!mixer || !mixer->isReady()) {
		warning("coop gameaudio: mixer not ready, no tap to take");
		opus_encoder_destroy(_encoder);
		_encoder = nullptr;
		return false;
	}
	rate = (int)mixer->getOutputRate();
	stereo = mixer->getOutputStereo();
	if (!stereo) {
		// The VR backend builds the mixer stereo (createVR); anything else is
		// a different configuration than this unit was measured with.
		warning("coop gameaudio: mixer is not stereo, refusing to send");
		opus_encoder_destroy(_encoder);
		_encoder = nullptr;
		return false;
	}

	_inRate = rate;
	_resStep = (double)_inRate / (double)kOpusRate;
	const int ringSamples = _inRate * kOpusChannels * kTapRingSeconds;
	_tapRing.assign(ringSamples, 0);
	_tapHead = _tapTail = 0;
	_resHaveWindow = false;
	_resPos = 0;
	_resOutFrames = 0;
	_audioSeq = 0;
	_transport = transport;

	g_tapTarget = this;
	_senderActive.store(true);
	Android::setAudioOutputTap(tapThunk);
	return true;
}

void GameAudio::stopSender() {
	if (_senderActive.exchange(false)) {
		Android::setAudioOutputTap(nullptr);
		g_tapTarget = nullptr;
	}
	if (_encoder) {
		opus_encoder_destroy(_encoder);
		_encoder = nullptr;
	}
	_transport = nullptr;
}

// Runs on the platform audio thread. Copy and return - the whole cost of a
// disabled sender is this function's one relaxed load.
void GameAudio::onOutputBytes(const byte *bytes, int byteCount) {
	const int16 *samples = reinterpret_cast<const int16 *>(bytes);
	const int count = byteCount / (int)sizeof(int16);
	if (count <= 0)
		return;

	std::lock_guard<std::mutex> lock(_mtx);
	_counters.tapBytes += byteCount;
	const int capacity = (int)_tapRing.size();
	if (!capacity)
		return;
	uint64 available = _tapTail - _tapHead;
	if (available + (uint64)count > (uint64)capacity) {
		// Full: keep the newest. This is the room loop not pumping.
		uint64 drop = available + (uint64)count - (uint64)capacity;
		_tapHead += drop;
		_counters.tapRingDrops += (int)drop;
	}
	for (int i = 0; i < count; ++i)
		_tapRing[(_tapTail + i) % capacity] = samples[i];
	_tapTail += count;
}

bool GameAudio::tapReadFrame(int16 *frame) {
	const int capacity = (int)_tapRing.size();
	if (_tapTail - _tapHead < (uint64)kOpusChannels)
		return false;
	frame[0] = _tapRing[_tapHead % capacity];
	frame[1] = _tapRing[(_tapHead + 1) % capacity];
	_tapHead += kOpusChannels;
	return true;
}

bool GameAudio::emitFrame() {
	// Fill the staged 20 ms of 48 kHz output, resampling as we go. The window
	// state (_resA/_resB/_resPos) is continuous across frames and across
	// early returns, so a starved tick resumes mid-window without a click.
	while (_resOutFrames < kFrameSamples) {
		if (!_resHaveWindow) {
			if (!tapReadFrame(_resA) || !tapReadFrame(_resB))
				return false;
			_resHaveWindow = true;
			_resPos = 0;
		}
		_resPos += _resStep;
		if (_resPos >= 1.0) {
			_resPos -= 1.0;
			memcpy(_resA, _resB, sizeof(_resA));
			if (!tapReadFrame(_resB)) {
				// The window's right edge ran dry: keep the partial output
				// staged and wait for real audio; do not encode short.
				return false;
			}
		}
		const double w = _resPos;
		for (int c = 0; c < kOpusChannels; ++c) {
			double v = _resA[c] * (1.0 - w) + _resB[c] * w;
			int q = (int)(v + (v >= 0 ? 0.5 : -0.5));
			if (q > 32767) q = 32767;
			if (q < -32768) q = -32768;
			_resOut[_resOutFrames * kOpusChannels + c] = (int16)q;
		}
		++_resOutFrames;
	}

	byte packet[4000]; // an Opus frame is <= 1275 B/channel; 4000 is headroom
	const int len = opus_encode(_encoder, _resOut, kFrameSamples, packet, (int)sizeof(packet));
	if (len < 0) {
		warning("coop gameaudio: opus_encode failed (%d)", len);
		_resOutFrames = 0;
		return false;
	}
	_resOutFrames = 0;

	if (!_transport->sendMedia('A', _audioSeq, packet, len)) {
		++_counters.sendFailures;
		return false;
	}
	++_counters.framesEncoded;
	_counters.bytesSent += len;
	++_audioSeq;
	return true;
}

void GameAudio::pump() {
	if (!_senderActive.load() || !_encoder || !_transport)
		return;
	// Pacing comes from the tap ring, which fills at real time: this cannot
	// run ahead of the game. One frame per call, deliberately: the room loop
	// runs faster than audio produces frames (10 ms vs 20 ms), so catching up
	// after a stall never needs a burst — and back-to-back small datagrams on
	// one socket are what the relay saw reordered into a seq regression.
	{
		std::lock_guard<std::mutex> lock(_mtx);
		const int neededSamples =
			(int)(kFrameSamples * _resStep) * kOpusChannels + kOpusChannels; // one full frame + lookahead
		if (!_resOutFrames && _tapTail - _tapHead < (uint64)neededSamples) {
			++_counters.encodeSkips;
			return;
		}
		emitFrame();
	}
}

// ---------------------------------------------------------------- receiver

bool GameAudio::startReceiver() {
	if (_decoder)
		return false;

	int err = 0;
	_decoder = opus_decoder_create(kOpusRate, kOpusChannels, &err);
	if (!_decoder || err != OPUS_OK) {
		warning("coop gameaudio: opus decoder create failed (%d)", err);
		return false;
	}
	// Same measured expectation as the sender's FEC setting: the decoder uses
	// it to weight concealment against the FEC data it is being handed.
	opus_decoder_ctl(_decoder, OPUS_SET_PACKET_LOSS_PERC(15));

	_pcmRing.assign(kOpusRate * kOpusChannels * kPcmRingSeconds, 0);
	_pcmHead = _pcmTail = 0;
	_haveLastSeq = false;
	_lastSeq = 0;
	_wantFec = false;

	Audio::Mixer *mixer = g_system->getMixer();
	if (!mixer || !mixer->isReady()) {
		warning("coop gameaudio: mixer not ready, no playback");
		opus_decoder_destroy(_decoder);
		_decoder = nullptr;
		return false;
	}

	// One permanent stream for the whole session, silence until data: it is
	// started before the first frame so arrival latency never includes a
	// playStream call, and permanent=true keeps a stopAll elsewhere (a game
	// starting music) from killing the room's audio.
	_stream = new GamePcmStream(this);
	_handle = new Audio::SoundHandle();
	mixer->playStream(Audio::Mixer::kSFXSoundType, _handle, _stream, -1,
			Audio::Mixer::kMaxChannelVolume, 0, DisposeAfterUse::NO, true);
	return true;
}

void GameAudio::stopReceiver() {
	if (_decoder) {
		Audio::Mixer *mixer = g_system->getMixer();
		if (mixer && _handle)
			mixer->stopHandle(*_handle);
		delete _handle;
		_handle = nullptr;
		delete _stream;
		_stream = nullptr;
		opus_decoder_destroy(_decoder);
		_decoder = nullptr;
	}
}

void GameAudio::feedFrame(uint32 seq, const byte *opusBytes, int len) {
	std::lock_guard<std::mutex> lock(_mtx);
	if (!_decoder)
		return;
	++_counters.framesReceived;

	if (_haveLastSeq) {
		if ((int32_t)(seq - _lastSeq) <= 0) {
			++_counters.lateDrops; // duplicate or reordered past the frontier
			return;
		}
		const uint32 gap = seq - _lastSeq - 1;
		if (gap > 0) {
			++_counters.gapEvents;
			// The arriving packet carries its predecessor's redundancy
			// (sender-side inband FEC), so the *last* lost frame can be
			// reconstructed exactly instead of concealed: hold it back here
			// and take it from this packet below. Earlier frames stay PLC's.
			_wantFec = true;
			const int conceal =
				(int)(gap < (uint32)kMaxPlcFrames ? gap : (uint32)kMaxPlcFrames) - 1;
			int16 plc[kFrameSamples * kOpusChannels];
			for (int i = 0; i < conceal; ++i) {
				// NULL input is the decoder's own concealment continuation.
				if (opus_decode(_decoder, nullptr, 0, plc, kFrameSamples, 1) > 0) {
					pcmPush(plc, kFrameSamples);
					++_counters.plcFrames;
				}
			}
			if (gap > (uint32)kMaxPlcFrames) {
				int silence = (int)((gap - (uint32)kMaxPlcFrames) < 50u
						? gap - (uint32)kMaxPlcFrames : 50u);
				static int16 zeros[kFrameSamples * kOpusChannels] = {};
				for (int i = 0; i < silence; ++i)
					pcmPush(zeros, kFrameSamples);
				_counters.silenceFrames += silence;
			}
		}
	}

	// FEC first: the recovered audio belongs to the gap that just ended, so
	// it goes into the ring before this packet's own frame. A decode_fec
	// call returns 0 when the packet carries no redundancy - not an error.
	if (_wantFec) {
		_wantFec = false;
		int16 fecPcm[kFrameSamples * kOpusChannels];
		const int fec = opus_decode(_decoder, opusBytes, len, fecPcm, kFrameSamples, 1);
		if (fec > 0) {
			pcmPush(fecPcm, fec);
			++_counters.fecFrames;
		}
	}

	int16 pcm[kFrameSamples * kOpusChannels];
	const int got = opus_decode(_decoder, opusBytes, len, pcm, kFrameSamples, 0);
	if (got <= 0) {
		warning("coop gameaudio: opus_decode failed (%d)", got);
		return;
	}
	++_counters.framesDecoded;
	pcmPush(pcm, got);
	_lastSeq = seq;
	_haveLastSeq = true;
}

// Caller holds _mtx (feedFrame) - and pcmRead takes it itself.
void GameAudio::pcmPush(const int16 *frames, int count) {
	const int capacity = (int)_pcmRing.size();
	if (!capacity || count <= 0)
		return;
	const int total = count * kOpusChannels;
	uint64 available = _pcmTail - _pcmHead;
	if (available + (uint64)total > (uint64)capacity) {
		uint64 drop = available + (uint64)total - (uint64)capacity;
		_pcmHead += drop;
		_counters.driftDrops += (int)drop;
	}
	for (int i = 0; i < total; ++i)
		_pcmRing[(_pcmTail + i) % capacity] = frames[i];
	_pcmTail += total;

	// Latency bound: if the two crystals' drift (or a late arrival burst)
	// pushed the buffer past the bound, shed the oldest down to the target.
	const int maxSamples = kMaxBufferedFrames * kOpusRate / 1000 * kOpusChannels;
	const int keepSamples = kDrainToFrames * kOpusRate / 1000 * kOpusChannels;
	if (_pcmTail - _pcmHead > (uint64)maxSamples) {
		uint64 drop = _pcmTail - _pcmHead - keepSamples;
		_pcmHead += drop;
		_counters.driftDrops += (int)drop;
	}
}

// Runs on the platform audio thread through the mixer; never returns short, so
// the permanent stream survives an underrun as silence instead of stopping.
int GameAudio::pcmRead(int16 *frames, int count) {
	std::lock_guard<std::mutex> lock(_mtx);
	const int capacity = (int)_pcmRing.size();
	int filled = 0;
	if (capacity) {
		while (filled < count && _pcmTail > _pcmHead) {
			frames[filled * kOpusChannels] = _pcmRing[_pcmHead % capacity];
			frames[filled * kOpusChannels + 1] = _pcmRing[(_pcmHead + 1) % capacity];
			_pcmHead += kOpusChannels;
			++filled;
		}
	}
	if (filled < count) {
		// Honest accounting: any padding is an underrun. Pre-arrival silence
		// counts too - it is the same observable condition.
		++_counters.underruns;
		std::memset(frames + filled * kOpusChannels, 0,
				(size_t)(count - filled) * kOpusChannels * sizeof(int16));
	}
	return count;
}

GameAudio::Counters GameAudio::counters() const {
	std::lock_guard<std::mutex> lock(_mtx);
	return _counters;
}

} // namespace Coop
