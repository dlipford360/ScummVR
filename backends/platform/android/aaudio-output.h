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

// A native device output for the VR build. The classic Android port writes
// the mixer's output through a Java AudioTrack; the VR bootstrap has no Java
// activity around one, so until now the mixed bytes were simply dropped
// (JNI::writeAudio "pretended" to write them). That made game audio
// unhearable in VR - and it matters doubly now that a coop guest mixes the
// host's network audio: playback through the mixer only reaches a listener
// if some device actually consumes the mixer's output.
//
// This is that device: AAudio (API 26+, and the VR APK's minSdk is above
// that), 16-bit PCM, driven from the platform audio thread exactly like the
// AudioTrack was. Everything is called from that one thread, so no
// synchronization exists here on purpose. If the stream cannot be created,
// this stays permanently inert and the caller's pretend-write behavior is
// restored - a missing sound device must not break the room.

#ifndef BACKENDS_PLATFORM_ANDROID_AAUDIO_OUTPUT_H
#define BACKENDS_PLATFORM_ANDROID_AAUDIO_OUTPUT_H

namespace Android {
namespace DeviceAudio {

// Create and start the output stream at the mixer's format. Returns false
// once failed, forever; safe to call more than while failed.
bool start(int sampleRate, int channels);

// Blocking write of `byteCount` bytes; returns the count consumed (the
// whole request when the device accepted it), or -1 on device error.
// When no stream exists, reports success without storing anything, so a
// caller with no device behaves as it did before this file existed.
int write(const void *bytes, int byteCount);

// The silence-management mirrors of the AudioTrack pause/play, and the
// shutdown on thread exit. All no-ops when inert.
void pause();
void resume();
void stopAndClose();

} // namespace DeviceAudio
} // namespace Android

#endif
