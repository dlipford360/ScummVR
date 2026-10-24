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

// The hand-off between the guest's decoder and the VR renderer.
//
// The decode callback runs on the self-test/room thread, which has no GL
// context; the virtual screen quad is drawn on the VR main thread. Frames
// cross that boundary through one single-slot mailbox under a mutex: publish
// copies the codec's NV12 output (stride/sliceHeight are the codec's own,
// read back from the format - not assumed), and the render thread takes any
// frame newer than the last one it drew. A slow renderer overwrites the
// slot rather than queueing, which is the right policy for live video: the
// newest frame is the relevant one, and unbounded buffering would only add
// latency to a path whose whole purpose is to be measured.
//
// The mailbox also carries the render-side counters (frames actually uploaded
// to GL, and the decode-callback-to-upload delay) so the end-to-end latency
// chain can be closed with numbers from both ends.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_VIDEO_DISPLAY_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_VIDEO_DISPLAY_H

#include "common/types.h"

#include <mutex>
#include <vector>

namespace Coop {

struct VideoRenderStats {
	int rendered;          // frames the VR thread actually uploaded
	int decodedTo;         // frames decoded while the display was wanted
	int64 decodeToUploadUs; // us from this frame's decode callback to upload
	int64 uploadWallUs;     // steady_clock now() at the upload
	uint32 uploadFrameId;   // the wire frameId that was just uploaded
};

struct VideoMailbox {
	std::mutex m;
	std::vector<byte> frame; // NV12, laid out per stride/sliceHeight below
	uint32 seq;
	uint32 frameId;          // the wire frameId of the frame in the slot
	int64 publishedAtUs;     // steady_clock now() at the decode callback
	// What the codec actually published for the frame in the slot.
	int width, height, stride, sliceHeight;
	bool haveNew;

	VideoRenderStats stats;
	std::vector<int64> latUs; // decode-callback -> upload, sampled by the VR thread
};

// The one instance. Defined in coop-selftest.cpp: the producer side lives
// with the decoder's caller, and the render side (android-vr-main.cpp) only
// links against grabVideoFrame().
extern VideoMailbox g_coopVideo;

// True while something wants the guest's screen quad to show the received
// video instead of the local game scene. Plain bool: the VR thread only ever
// reads it, and a torn read just skips or shows one frame.
extern volatile bool g_coopVideoDisplayWant;

// End-to-end latency sampling: the decode side, the render side, and the
// host side all timestamp the same wire frameIds, and the samples join
// offline by id. One line per kCoopLatSampleEvery frames keeps the log
// itself out of the measurement.
const uint32 kCoopLatSampleEvery = 15;

// The self-test thread keeps these current from Transport's PONG-derived
// clock estimate, so the render thread can timestamp an upload in the
// relay's clock - the one clock both headsets share.
extern volatile bool g_coopClockSynced;
extern volatile int64 g_coopClockOffsetUs; // add to a local steady-clock us

// Producer side (decode callback thread). Copies the codec's NV12 output into
// the mailbox; layout values are the codec's published stride/sliceHeight.
void publishVideoFrame(const byte *plane, int len, int64 decodedAtUs,
	int width, int height, int stride, int sliceHeight, uint32 frameId);

// Consumer side (VR render thread). Returns true and fills a copy of the
// slot if it holds a frame newer than lastSeq, which is updated to the
// taken frame's seq. Also updates the render-side counters.
bool grabVideoFrame(VideoMailbox *out, uint32 lastSeq, VideoRenderStats *stats);

} // namespace Coop

#endif
