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

// Control plane for the coop relay: create a room, join one, close one. Plain
// HTTP + JSON on top of ScummVM's Networking layer, which is the only HTTP
// client the fork has.

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_SESSION_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_SESSION_H

#include "common/types.h"
#include "common/str.h"
#include "backends/platform/android-vr/coop-wire.h"

#include <mutex>

namespace Common {
class JSONValue;
}

namespace Networking {
struct ErrorResponse;
}

namespace Coop {

// This build speaks protocol v3 and expects the sealing scheme named below. The
// relay advertises both, and a mismatch is refused rather than guessed at.
// v3 added the `P` head-pose type and the relay's advertised SEQ ceiling.
const int kProtocolVersion = 3;
const char kMediaEncryption[] = "aes-256-gcm";

// Everything the control plane hands one participant.
struct Participant {
	char code[Wire::kMaxRoomCodeBytes];
	uint16 peerId;
	char sessionSecret[129];
	// 32 bytes as hex. This came over HTTPS and must never be put on the UDP
	// channel, where anyone with the room code could read it.
	char mediaKeyHex[65];
	char role; // 'h' or 'g'

	char relayHost[128];
	int relayPort;
	int relayProtocolVersion;
	int maxDatagramBytes;
	int maxPayloadBytes;
	// Advertised SEQ ceiling. Compared against Wire::kMaxSeq, so a relay that
	// counts differently is caught at the gate instead of showing up as a
	// dropped frame much later. Truncating this to `int` would be a bug: the
	// ceiling is 2^53-1.
	uint64 maxSeq;
	char mediaEncryption[32];
};

// Owner-only values: the invite is for guests, the owner secret is for closing
// the room and kicking people, and neither ever goes to a guest.
struct OwnerCredentials {
	char invite[64];
	char ownerSecret[64];
};

// Requests are asynchronous: Networking::ConnectionManager pumps them on its own
// timer thread, so the callbacks arrive off the game thread. Results are copied
// out eagerly into guarded fields and read back through the accessors. Two rules
// from the framework drive this shape:
//
//   - PostRequest deletes the JSONValue it passes to the callback the moment the
//     callback returns, so nothing may be kept from it.
//   - PostRequest itself is owned by the ConnectionManager once it is added, and
//     is deleted on the timer thread when it finishes. Hence a Session must
//     outlive its requests, which is why the destructor waits.
//
// One request at a time: these calls return false while one is in flight.
class Session {
public:
	Session();
	~Session();

	// "http://host:port" or "https://host". In production this must be https:
	// the session secret and the media key ride in these responses.
	void setBaseUrl(const Common::String &baseUrl);
	Common::String baseUrl() const;

	bool createRoom(const char *ownerName, bool openRoom = false, const char *customName = nullptr);
	bool joinRoom(const char *code, const char *invite); // invite may be empty for an open room
	bool closeRoom(const char *code, const char *ownerSecret);

	bool pending() const;
	// Whether the most recently finished request succeeded.
	bool lastRequestOk() const;
	// Which of the two result sets the last response actually filled in: a guest
	// join never carries owner credentials, and a failed request fills in neither.
	bool haveParticipant() const;
	bool haveOwnerCredentials() const;

	// Reads one control-plane response into the structs above and evaluates the
	// version gate. Split out because it is pure: it can be fed the relay's real
	// JSON without any networking, which is how the field mapping gets checked.
	// `requestedCode` covers the join response, which does not echo the code.
	// Returns false when the gate refuses, with `reason` filled in.
	static bool readResponse(const Common::JSONValue *json, const char *requestedCode,
		Participant *out, OwnerCredentials *ownerOut, bool *haveOwnerOut, char *reason, int reasonCapacity);

	void copyParticipant(Participant *out) const;
	void copyOwnerCredentials(OwnerCredentials *out) const;

	// The version gate: did the relay advertise the protocol and the budgets this
	// build was compiled against? The reason is filled in either way so the UI can
	// say what is wrong.
	bool gateOk() const;
	void copyGateReason(char *out, int capacity) const;
	void copyError(char *out, int capacity) const;

private:
	struct Field {
		const char *name;
		const char *value;
	};

	bool post(const char *path, const Field *fields, int fieldCount, char roleTag);
	void parseResponse(const Common::JSONValue *json);

	// Called on the ConnectionManager's timer thread.
	void onJsonValue(const Common::JSONValue *json);
	void onError(const Networking::ErrorResponse &error);

	mutable std::mutex _mutex;
	Common::String _baseUrl;

	bool _pending;
	bool _lastOk;
	char _pendingRole;
	// The join response does not echo the room code, so it is remembered here.
	char _requestedCode[Wire::kMaxRoomCodeBytes];

	bool _haveParticipant;
	bool _haveOwner;
	Participant _participant;
	OwnerCredentials _owner;

	bool _gateOk;
	char _gateReason[Wire::kMaxHeaderBytes];
	char _error[Wire::kMaxHeaderBytes];
};

} // namespace Coop

#endif
