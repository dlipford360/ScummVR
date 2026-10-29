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

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// A tap on the audio backend's *mixed output*: exactly the bytes the output
// path would hand to the device, after the mixer has composed every game
// stream. This header is deliberately separate from android.h so a consumer
// (the network game-audio sender) can register a tap without pulling in the
// platform's JNI machinery.
//
// Contract, stated because the audio thread enforces it by construction:
// the callback runs on the audio output thread, between the mixer's fill and
// the device write, at the mixer's output format (16-bit signed, interleaved
// stereo at the configured rate) and at roughly the device buffer cadence.
// It must copy and return: no blocking, no allocation storms, no reentering
// the mixer. A tap that misbehaves stutters the local audio, which is the
// feedback.
//
// In VR mode the device write is skipped (no AudioTrack), but the mixer
// still fills every buffer - so the tap sees real game audio on a Quest
// even while the headset plays nothing locally.

#ifndef BACKENDS_PLATFORM_ANDROID_AUDIO_TAP_H
#define BACKENDS_PLATFORM_ANDROID_AUDIO_TAP_H

#include "common/types.h"

namespace Android {

typedef void (*AudioOutputTap)(const byte *bytes, int byteCount);
// Install (or with nullptr, remove) the tap. Safe from any thread; the
// audio thread reads it as a single word-sized store, so toggling can
// happen while audio is running.
void setAudioOutputTap(AudioOutputTap tap);

} // namespace Android

#endif
