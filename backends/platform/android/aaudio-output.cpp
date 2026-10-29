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

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "backends/platform/android/aaudio-output.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>

// Loaded by name rather than linked: the shared object is built with a
// global API-21 target, where libaaudio.so is not on the link path, while
// the VR runtime (API 29+) always has it. If the library is missing, this
// unit stays inert - a missing sound device must not take the room down.
namespace Android {
namespace DeviceAudio {

namespace {

typedef aaudio_result_t (*fn_createStreamBuilder)(AAudioStreamBuilder **);
typedef void (*fn_setSampleRate)(AAudioStreamBuilder *, int32_t);
typedef void (*fn_setChannelCount)(AAudioStreamBuilder *, int32_t);
typedef void (*fn_setFormat)(AAudioStreamBuilder *, aaudio_format_t);
typedef void (*fn_setSharingMode)(AAudioStreamBuilder *, int32_t);
typedef void (*fn_setPerformanceMode)(AAudioStreamBuilder *, int32_t);
typedef void (*fn_setUsage)(AAudioStreamBuilder *, int32_t);
typedef aaudio_result_t (*fn_openStream)(AAudioStreamBuilder *, AAudioStream **);
typedef aaudio_result_t (*fn_deleteBuilder)(AAudioStreamBuilder *);
typedef aaudio_result_t (*fn_requestStart)(AAudioStream *);
typedef aaudio_result_t (*fn_requestStop)(AAudioStream *);
typedef aaudio_result_t (*fn_close)(AAudioStream *);
typedef int64_t (*fn_write)(AAudioStream *, const void *, int32_t, int64_t);

struct Api {
	fn_createStreamBuilder createStreamBuilder;
	fn_setSampleRate setSampleRate;
	fn_setChannelCount setChannelCount;
	fn_setFormat setFormat;
	fn_setSharingMode setSharingMode;
	fn_setPerformanceMode setPerformanceMode;
	fn_setUsage setUsage;
	fn_openStream openStream;
	fn_deleteBuilder deleteBuilder;
	fn_requestStart requestStart;
	fn_requestStop requestStop;
	fn_close close;
	fn_write write;
};

Api *loadApi() {
	static Api *api = nullptr;
	static bool attempted = false;
	if (attempted)
		return api;
	attempted = true;

	void *lib = dlopen("libaaudio.so", RTLD_NOW);
	if (!lib)
		return nullptr;

	Api *candidate = new Api();
#define NEED(name, sym) do { \
		candidate->name = (fn_##name)dlsym(lib, #sym); \
		if (!candidate->name) { delete candidate; return nullptr; } \
	} while (0)
	NEED(createStreamBuilder, AAudio_createStreamBuilder);
	NEED(setSampleRate, AAudioStreamBuilder_setSampleRate);
	NEED(setChannelCount, AAudioStreamBuilder_setChannelCount);
	NEED(setFormat, AAudioStreamBuilder_setFormat);
	NEED(setSharingMode, AAudioStreamBuilder_setSharingMode);
	NEED(setPerformanceMode, AAudioStreamBuilder_setPerformanceMode);
	NEED(setUsage, AAudioStreamBuilder_setUsage);
	NEED(openStream, AAudioStreamBuilder_openStream);
	NEED(deleteBuilder, AAudioStreamBuilder_delete);
	NEED(requestStart, AAudioStream_requestStart);
	NEED(requestStop, AAudioStream_requestStop);
	NEED(close, AAudioStream_close);
	NEED(write, AAudioStream_write);
#undef NEED
	api = candidate;
	return api;
}

AAudioStream *g_stream = nullptr;
bool g_failed = false;
int g_channels = 0;

} // namespace

bool start(int sampleRate, int channels) {
	if (g_stream)
		return true;
	if (g_failed)
		return false;

	Api *api = loadApi();
	if (!api) {
		g_failed = true;
		return false;
	}

	AAudioStreamBuilder *builder = nullptr;
	if (api->createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
		g_failed = true;
		return false;
	}

	api->setSampleRate(builder, sampleRate);
	api->setChannelCount(builder, channels);
	api->setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	// Shared rather than exclusive: the VR compositor owns the audio graph,
	// and EXCLUSIVE would fail outright the moment anything else held it.
	api->setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	api->setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	api->setUsage(builder, AAUDIO_USAGE_MEDIA);

	aaudio_result_t openResult = api->openStream(builder, &g_stream);
	api->deleteBuilder(builder);
	if (openResult != AAUDIO_OK || !g_stream) {
		g_stream = nullptr;
		g_failed = true;
		return false;
	}

	if (api->requestStart(g_stream) != AAUDIO_OK) {
		api->close(g_stream);
		g_stream = nullptr;
		g_failed = true;
		return false;
	}
	g_channels = channels;
	return true;
}

int write(const void *bytes, int byteCount) {
	if (!g_stream)
		return byteCount; // inert: report the write, keep the caller's old behavior
	const int64_t written =
		loadApi()->write(g_stream, bytes, byteCount / (g_channels * 2), -1);
	if (written < 0)
		return -1;
	return (int)written * g_channels * 2;
}

void pause() {
	if (g_stream)
		loadApi()->requestStop(g_stream);
}

void resume() {
	if (g_stream)
		loadApi()->requestStart(g_stream);
}

void stopAndClose() {
	if (g_stream) {
		Api *api = loadApi();
		api->requestStop(g_stream);
		api->close(g_stream);
		g_stream = nullptr;
	}
}

} // namespace DeviceAudio
} // namespace Android
