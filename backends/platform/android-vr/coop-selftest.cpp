/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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

// The marker file is read with stdio, and the logging goes through ScummVM's
// debug()/warning(). Same choice as coop-transport.cpp: the standard headers this
// file needs collide with the forbidden-symbol macros.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/android-vr/coop-selftest.h"

#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-seal.h"
#include "backends/platform/android-vr/coop-session.h"
#include "backends/platform/android-vr/coop-transport.h"
#include "backends/platform/android-vr/coop-wire.h"

#include "common/debug.h"
#include "common/textconsole.h"

#ifdef __ANDROID__
// The control plane is the Java HTTP stack, which needs a JNIEnv on the calling
// thread. See runTest() below.
#include "backends/platform/android/jni-android.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>

namespace Coop {

namespace {

// adb-pushable, and the VR app can read /sdcard with the MANAGE_EXTERNAL_STORAGE
// app-op the manifest already documents. The app-private files dir would need
// `run-as`, which makes the test slower to drive than it is worth.
const char *const kDefaultMarkerPaths[] = {
	"/sdcard/scummvm-coop-test.txt",
};

const int kMaxRunSeconds = 60;

struct Config {
	char baseUrl[160];
	char code[Wire::kMaxRoomCodeBytes];
	char invite[64];
	char ownerName[32];
	bool asHost;
	bool plaintext;
	int seconds;
};

void trim(char *text) {
	size_t start = 0;
	while (text[start] == ' ' || text[start] == '\t')
		++start;
	if (start)
		memmove(text, text + start, strlen(text + start) + 1);

	size_t end = strlen(text);
	while (end && (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t'))
		text[--end] = 0;
}

bool readConfig(const char *path, Config *out) {
	FILE *file = fopen(path, "rt");
	if (!file)
		return false;

	memset(out, 0, sizeof(*out));
	out->asHost = true;
	out->plaintext = false;
	out->seconds = 20;

	char line[256];
	while (fgets(line, sizeof(line), file)) {
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *equals = strchr(line, '=');
		if (!equals) {
			warning("coop selftest: ignoring malformed line in %s", path);
			continue;
		}
		*equals = 0;
		char *key = line;
		char *value = equals + 1;
		trim(key);
		trim(value);

		if (!strcmp(key, "relay"))
			snprintf(out->baseUrl, sizeof(out->baseUrl), "%s", value);
		else if (!strcmp(key, "role"))
			out->asHost = strcmp(value, "guest") != 0;
		else if (!strcmp(key, "code"))
			snprintf(out->code, sizeof(out->code), "%s", value);
		else if (!strcmp(key, "invite"))
			snprintf(out->invite, sizeof(out->invite), "%s", value);
		else if (!strcmp(key, "owner"))
			snprintf(out->ownerName, sizeof(out->ownerName), "%s", value);
		else if (!strcmp(key, "plaintext"))
			out->plaintext = atoi(value) != 0;
		else if (!strcmp(key, "seconds")) {
			out->seconds = atoi(value);
			if (out->seconds < 1)
				out->seconds = 1;
			if (out->seconds > kMaxRunSeconds)
				out->seconds = kMaxRunSeconds;
		} else {
			warning("coop selftest: unknown key \"%s\"", key);
		}
	}
	fclose(file);

	if (!out->baseUrl[0]) {
		warning("coop selftest: %s has no relay= line, refusing to run", path);
		return false;
	}
	if (!out->asHost && (!out->code[0] || !out->invite[0])) {
		warning("coop selftest: role=guest needs both code= and invite=");
		return false;
	}
	return true;
}

int64 nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Sealing cost on this SoC, with the key the relay actually handed out. This is
// the number that says whether sealing is affordable before any of the media
// paths that would use it exist.
void benchSealing(const char *mediaKeyHex, const char *code, char roleTag, uint16 peerId) {
	Seal::Context ctx;
	if (!ctx.setFromHex(mediaKeyHex)) {
		warning("coop selftest: seal bench refused the media key");
		return;
	}
	if (!ctx.selfCheck()) {
		warning("coop selftest: seal bench self-check FAILED on this CPU - do not trust any sealing number below");
		return;
	}

	const int kPairs = 2000;
	const int kPlainBytes = 1083; // the single-datagram plaintext budget once sealed

	byte plain[kPlainBytes];
	byte sealed[kPlainBytes + 32];
	byte opened[kPlainBytes + 32];
	for (int i = 0; i < kPlainBytes; ++i)
		plain[i] = (byte)(i * 7 + 3);

	Wire::HeaderLine ref;
	snprintf(ref.code, sizeof(ref.code), "%s", code);
	ref.roleTag = roleTag;
	ref.peerId = peerId;
	ref.type = 'V';

	const int64 started = nowMs();
	int openedOk = 0;
	for (int i = 0; i < kPairs; ++i) {
		ref.seq = (uint64)i;
		const int sealedLen = Seal::sealPayload(ctx, sealed, sizeof(sealed), ref, plain, kPlainBytes);
		if (sealedLen <= 0)
			continue;
		// The AAD is the header this frame would arrive with, so the bench must
		// rebuild the same reference rather than reuse a stale seq.
		if (Seal::openPayload(ctx, opened, sizeof(opened), ref, sealed, sealedLen, false) == kPlainBytes
			&& !memcmp(opened, plain, kPlainBytes))
			++openedOk;
	}
	const int64 elapsed = nowMs() - started;

	debug("coop selftest: seal bench %d pairs of %d bytes in %lld ms = %.1f us per seal+open, %.1f MB/s, %d opened",
		kPairs, kPlainBytes, (long long)elapsed, (double)elapsed * 1000.0 / kPairs,
		(double)kPairs * kPlainBytes * 2.0 / (1024.0 * 1024.0) / ((double)elapsed / 1000.0), openedOk);
	if (openedOk != kPairs)
		warning("coop selftest: seal bench round trip failed %d of %d times", kPairs - openedOk, kPairs);
}

void runTestBody(const Config &cfg) {
	// The call site is before scummvm_main, so the backend is created but not yet
	// initialised and the Java HTTP manager is not up yet. Waiting also keeps the
	// test's logging away from the startup noise.
	std::this_thread::sleep_for(std::chrono::seconds(5));

	debug("coop selftest: start, role=%s, relay=%s", cfg.asHost ? "host" : "guest", cfg.baseUrl);

	Session session;
	session.setBaseUrl(Common::String(cfg.baseUrl));

	const bool requested = cfg.asHost
		? session.createRoom(cfg.ownerName[0] ? cfg.ownerName : "headset")
		: session.joinRoom(cfg.code, cfg.invite);
	if (!requested) {
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		warning("coop selftest: the control-plane request could not even be queued: %s", reason);
		return;
	}

	// The request is asynchronous and its callback runs on the connection
	// manager's thread, so this thread waits for it to land.
	const int64 deadline = nowMs() + 15000;
	while (session.pending() && nowMs() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (session.pending()) {
		warning("coop selftest: no control-plane answer within 15 s. Nothing reached the relay: "
				"check the URL, that the relay advertises a LAN-reachable address, and that the "
				"certificate is trusted (the Java layer logs the TLS failure)");
		return;
	}
	if (!session.lastRequestOk()) {
		char reason[Wire::kMaxHeaderBytes];
		session.copyError(reason, sizeof(reason));
		warning("coop selftest: control plane refused the request: %s", reason);
		return;
	}

	char reason[Wire::kMaxHeaderBytes];
	session.copyGateReason(reason, sizeof(reason));
	if (!session.gateOk()) {
		warning("coop selftest: version gate refused this relay: %s", reason);
		return;
	}
	debug("coop selftest: gate accepted the relay");

	if (!session.haveParticipant()) {
		warning("coop selftest: the response carried no participant");
		return;
	}
	Participant p;
	session.copyParticipant(&p);
	debug("coop selftest: room %s, peerId %u, role %c, relay %s:%d, protocol v%d, payload cap %d, seq ceiling %llu, encryption %s",
		p.code, p.peerId, p.role, p.relayHost, p.relayPort, p.relayProtocolVersion, p.maxPayloadBytes,
		(unsigned long long)p.maxSeq, p.mediaEncryption);
	debug("coop selftest: secret lengths: session %zu, media key %zu (values are never logged)",
		strlen(p.sessionSecret), strlen(p.mediaKeyHex));

	if (session.haveOwnerCredentials()) {
		OwnerCredentials owner;
		session.copyOwnerCredentials(&owner);
		// This is the one place a room code and an invite are printed on purpose:
		// it is how the second headset is pointed at the same room. Both are
		// room-scoped, and neither is the owner secret.
		debug("coop selftest: join the second headset with code=%s invite=%s", p.code, owner.invite);
	}

	benchSealing(p.mediaKeyHex, p.code, p.role, p.peerId);

	Transport transport;
	if (!transport.open(p.relayHost, p.relayPort)) {
		warning("coop selftest: could not open a UDP socket to %s:%d", p.relayHost, p.relayPort);
		return;
	}
	if (cfg.plaintext) {
		warning("coop selftest: plaintext mode requested - payloads will be unencrypted and unauthenticated");
		transport.enablePlaintextMode();
	} else if (!transport.setMediaKey(p.mediaKeyHex)) {
		warning("coop selftest: the transport refused the media key");
		transport.close();
		return;
	}
	if (!transport.join(p.code, p.peerId, p.sessionSecret, p.role)) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		warning("coop selftest: join refused: %s", status);
		transport.close();
		return;
	}

	const int64 joinedAt = nowMs();
	while (!transport.isReady() && !transport.isClosed() && nowMs() < joinedAt + 10000)
		std::this_thread::sleep_for(std::chrono::milliseconds(25));

	if (!transport.isReady()) {
		char status[Wire::kMaxHeaderBytes];
		transport.statusText(status, sizeof(status));
		warning("coop selftest: no WELCOME within 10 s (last relay line: %s). UDP reaches the relay "
				"only outbound, so a silent firewall or a wrong advertised relay.host looks exactly like this",
			status[0] ? status : "nothing at all");
		transport.close();
		return;
	}
	debug("coop selftest: WELCOME after %lld ms, sealed=%d, max single-datagram payload %d bytes",
		(long long)(nowMs() - joinedAt), (int)transport.isSealed(), transport.maxAppPayloadBytes());

	// Stay joined, send one application frame a second, and report everything the
	// relay sends back. With two headsets this is where a sealed frame crossing a
	// real relay in both directions gets proven.
	uint32 framesIn = 0, dataIn = 0, rosterIn = 0, controlIn = 0;
	int appSeq = 0;
	int64 nextSend = nowMs();
	const int64 until = nowMs() + (int64)cfg.seconds * 1000;
	while (nowMs() < until && !transport.isClosed()) {
		Transport::Event event;
		while (transport.pollEvent(&event)) {
			++framesIn;
			if (event.kind == Wire::kRecvData) {
				++dataIn;
				debug("coop selftest: frame '%c' from peer %u, %d bytes, envelope 0x%02x",
					event.header.type, event.header.peerId, event.payloadLen, event.envelope);
			} else if (event.kind == Wire::kRecvRoster) {
				++rosterIn;
				debug("coop selftest: roster: %s", event.text);
			} else if (event.kind == Wire::kRecvControl) {
				++controlIn;
				debug("coop selftest: control: %s", event.text);
			} else if (event.kind == Wire::kRecvForeign) {
				warning("coop selftest: datagram for another room on this socket");
			} else {
				warning("coop selftest: malformed datagram");
			}
		}

		if (nowMs() >= nextSend) {
			// X kind 7 (APP_PING) with a timestamp: the catalogue in the client
			// guide, so a second headset answers it and the round trip is visible.
			byte ping[16];
			ping[0] = 7;
			ping[1] = 0xFF;
			ping[2] = 0xFF;
			const int64 stamp = nowMs();
			for (int i = 0; i < 8; ++i)
				ping[3 + i] = (byte)(((uint64)stamp >> (i * 8)) & 0xff);
			if (!transport.sendFrame('X', ping, 11))
				warning("coop selftest: sendFrame refused (app payload #%d)", appSeq);
			++appSeq;

			// A 4 KB frame exercises fragmentation and reassembly on the device,
			// which is what a video frame actually looks like on this wire. The
			// relay drops a guest's `V` frame and the transport refuses to stamp one,
			// so a guest probes with `M`: the one type both roles may send. Nothing
			// here is a real voice frame, it is a payload of the right shape.
			const char probeType = (p.role == 'h') ? 'V' : 'M';
			byte big[4096];
			for (int i = 0; i < (int)sizeof(big); ++i)
				big[i] = (byte)(i * 31 + appSeq);
			if (!transport.sendMedia(probeType, (uint32)appSeq, big, sizeof(big)))
				warning("coop selftest: sendMedia('%c') refused for the 4 KB probe", probeType);
			nextSend += 1000;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	char status[Wire::kMaxHeaderBytes];
	transport.statusText(status, sizeof(status));
	debug("coop selftest: done after %d s - %u frames in (%u data, %u roster, %u control), "
			"%d app frames and %d 4 KB frames sent, dropped events %u, bad packets %u, crypto drops %u",
		cfg.seconds, framesIn, dataIn, rosterIn, controlIn, appSeq, appSeq,
		transport.droppedEvents(), transport.badPackets(), transport.cryptoDrops());
	if (status[0])
		debug("coop selftest: last relay status line: %s", status);
	if (transport.cryptoDrops())
		warning("coop selftest: inbound frames failed authentication - a shared room key means every "
				"participant can produce them, so check for another participant before suspecting the port");
	transport.close();
}

// Attach this thread to the JVM before touching the control plane.
//
// The first device run crashed 5 s in, at the first control-plane call:
//   PostRequest::start -> ConnectionManager::instance -> ConnectionManagerAndroid
//   -> JNI::getEnv -> JNI::fetchEnv -> abort()
// JNI::fetchEnv() calls GetEnv() and aborts when the answer is JNI_EDETACHED; it
// deliberately does not attach, so every native thread that reaches the Java layer
// must call attachThread() itself - which is what OSystem_Android's timer and audio
// threads do. A std::thread is not attached by definition.
//
// The transport needs no JNIEnv (plain POSIX UDP), but the thread is one unit and
// holding the attachment for its lifetime is simpler than attaching per call.
void runTest(const Config &cfg) {
#ifdef __ANDROID__
	JNI::attachThread();
#endif
	runTestBody(cfg);
#ifdef __ANDROID__
	JNI::detachThread();
#endif
}

} // namespace

bool startSelfTestIfRequested() {
	const char *candidates[3];
	int count = 0;
	const char *fromEnv = getenv("COOP_SELFTEST_FILE");
	if (fromEnv && fromEnv[0])
		candidates[count++] = fromEnv;
	for (int i = 0; i < (int)(sizeof(kDefaultMarkerPaths) / sizeof(kDefaultMarkerPaths[0])); ++i)
		candidates[count++] = kDefaultMarkerPaths[i];

	for (int i = 0; i < count; ++i) {
		Config cfg;
		if (!readConfig(candidates[i], &cfg))
			continue;
		// Its own thread: the render loop must not stall while the control plane
		// round trip runs, and Session callbacks arrive on another thread anyway.
		// Nothing is logged here because the caller runs before the backend is
		// initialised; runTest says when it found the file.
		std::thread(runTest, cfg).detach();
		return true;
	}
	return false;
}

} // namespace Coop
