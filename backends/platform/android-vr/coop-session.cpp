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

// Needs the networking layer plus a sleep in the destructor, both of which pull
// system headers that collide with ScummVM's forbidden-symbol macros.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"

#include "backends/platform/android-vr/coop-session.h"

#include "common/debug.h"
#include "common/textconsole.h"
#include "common/formats/json.h"

#ifdef USE_HTTP
#include "backends/networking/http/connectionmanager.h"
#include "backends/networking/http/postrequest.h"
#endif

#include <chrono>
#include <cstring>
#include <thread>

namespace Coop {

namespace {

void copyText(const Common::String &src, char *out, int capacity) {
	if (!out || capacity <= 0)
		return;
	int i = 0;
	for (; i < capacity - 1 && i < (int)src.size(); ++i)
		out[i] = src[i];
	out[i] = 0;
}

// Room codes are the one piece of caller input that ends up inside a URL, so it
// is validated instead of pasted: 4..8 characters of [A-Z0-9], as the relay defines them.
bool isRoomCode(const char *code) {
	if (!code)
		return false;
	int len = 0;
	for (; code[len]; ++len) {
		const char c = code[len];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return len >= 4 && len <= Wire::kMaxRoomCodeLen;
}

} // namespace

Session::Session()
	: _pending(false),
	  _lastOk(false),
	  _pendingRole('g'),
	  _haveParticipant(false),
	  _haveOwner(false),
	  _gateOk(false) {
	memset(&_participant, 0, sizeof(_participant));
	memset(&_owner, 0, sizeof(_owner));
	memset(_requestedCode, 0, sizeof(_requestedCode));
	_gateReason[0] = 0;
	_error[0] = 0;
}

Session::~Session() {
	// The ConnectionManager owns in-flight requests and calls back into this
	// object from its timer thread, so destruction waits for them to land. In
	// practice a Session lives as long as the app does; this is the guard rail.
	for (int attempt = 0; attempt < 500; ++attempt) {
		if (!pending())
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	warning("coop: session destroyed with a control-plane request still in flight");
}

void Session::setBaseUrl(const Common::String &baseUrl) {
	Common::String trimmed = baseUrl;
	while (!trimmed.empty() && trimmed[trimmed.size() - 1] == '/')
		trimmed.erase(trimmed.size() - 1, 1);

	const bool http = trimmed.hasPrefix("http://");
	const bool https = trimmed.hasPrefix("https://");
	if (!http && !https) {
		warning("coop: relay URL must start with http:// or https://, got %s", baseUrl.c_str());
		std::lock_guard<std::mutex> lock(_mutex);
		_baseUrl.clear();
		copyText("relay url is not http(s)", _error, sizeof(_error));
		return;
	}
	if (!https)
		warning("coop: relay is reachable over plain http; the session secret and media key are readable on the network");

	std::lock_guard<std::mutex> lock(_mutex);
	_baseUrl = trimmed;
	_error[0] = 0;
}

Common::String Session::baseUrl() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _baseUrl;
}

bool Session::createRoom(const char *ownerName) {
	const Field fields[] = { { "ownerName", ownerName ? ownerName : "host" } };
	return post("/rooms", fields, 1, 'h');
}

bool Session::joinRoom(const char *code, const char *invite) {
	if (!isRoomCode(code) || !invite || !invite[0])
		return false;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		copyText(Common::String(code), _requestedCode, sizeof(_requestedCode));
	}
	const Common::String path = "/rooms/" + Common::String(code) + "/sessions";
	const Field fields[] = { { "role", "guest" }, { "secret", invite } };
	return post(path.c_str(), fields, 2, 'g');
}

bool Session::closeRoom(const char *code, const char *ownerSecret) {
	if (!isRoomCode(code) || !ownerSecret || !ownerSecret[0])
		return false;
	const Common::String path = "/rooms/" + Common::String(code) + "/close";
	const Field fields[] = { { "ownerSecret", ownerSecret } };
	// Zero role: closing a room does not make us a participant of it.
	return post(path.c_str(), fields, 1, 0);
}

bool Session::post(const char *path, const Field *fields, int fieldCount, char roleTag) {
#ifndef USE_HTTP
	warning("coop: this build has no HTTP support, the relay cannot be reached");
	std::lock_guard<std::mutex> lock(_mutex);
	copyText("built without USE_HTTP", _error, sizeof(_error));
	return false;
#else
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_baseUrl.empty()) {
			copyText("no relay url configured", _error, sizeof(_error));
			return false;
		}
		if (_pending)
			return false;
		_pending = true;
		_lastOk = false;
		_error[0] = 0;
		if (roleTag)
			_pendingRole = roleTag;
	}

	Common::JSONObject body;
	for (int i = 0; i < fieldCount; ++i) {
		if (!fields[i].name || !fields[i].value)
			continue;
		// The JSONValue built from the object owns these children: ~JSONValue
		// deletes the mapped values of the map it copied, so they must not be
		// freed here too.
		body.setVal(Common::String(fields[i].name), new Common::JSONValue(fields[i].value));
	}
	Common::JSONValue *jsonBody = new Common::JSONValue(body);

	Networking::PostRequest *request = new Networking::PostRequest(
		_baseUrl + Common::String(path),
		new Common::Callback<Session, const Common::JSONValue *>(this, &Session::onJsonValue),
		new Common::Callback<Session, const Networking::ErrorResponse &>(this, &Session::onError));
	request->setJSONData(jsonBody);

	// start() serialises the body and registers the inner request with the
	// connection manager. It does not take ownership of the body, and nothing
	// else reads it afterwards.
	request->start();
	delete jsonBody;

	// The connection manager deletes the PostRequest once it reaches FINISHED.
	ConnMan.addRequest(request);
	return true;
#endif
}

namespace {

const Common::JSONValue *fieldOf(const Common::JSONObject &object, const char *name) {
	if (!object.contains(name))
		return nullptr;
	return object.getVal(name);
}

Common::String stringOf(const Common::JSONObject &object, const char *name) {
	const Common::JSONValue *value = fieldOf(object, name);
	if (!value || !value->isString())
		return Common::String();
	return value->asString();
}

int intOf(const Common::JSONObject &object, const char *name, int fallback) {
	const Common::JSONValue *value = fieldOf(object, name);
	if (!value)
		return fallback;
	if (value->isIntegerNumber())
		return (int)value->asIntegerNumber();
	if (value->isNumber())
		return (int)value->asNumber();
	return fallback;
}

// Same, without narrowing to `int`. Needed for values above 2^31, which is
// exactly where the SEQ ceiling sits.
int64 int64Of(const Common::JSONObject &object, const char *name, int64 fallback) {
	const Common::JSONValue *value = fieldOf(object, name);
	if (!value)
		return fallback;
	if (value->isIntegerNumber())
		return value->asIntegerNumber();
	if (value->isNumber())
		return (int64)value->asNumber();
	return fallback;
}

const Common::JSONObject *objectOf(const Common::JSONObject &object, const char *name) {
	const Common::JSONValue *value = fieldOf(object, name);
	if (!value || !value->isObject())
		return nullptr;
	return &value->asObject();
}

} // namespace

bool Session::readResponse(const Common::JSONValue *json, const char *requestedCode,
	Participant *out, OwnerCredentials *ownerOut, bool *haveOwnerOut, char *reason, int reasonCapacity) {
	if (reason && reasonCapacity > 0)
		reason[0] = 0;
	if (!json || !out || !ownerOut || !haveOwnerOut)
		return false;

	if (!json->isObject()) {
		copyText("relay response was not a JSON object", reason, reasonCapacity);
		return false;
	}

	const Common::JSONObject &root = json->asObject();

	Participant participant;
	memset(&participant, 0, sizeof(participant));

	const Common::JSONObject *session = objectOf(root, "session");
	if (session) {
		participant.peerId = (uint16)intOf(*session, "peerId", 0);
		copyText(stringOf(*session, "sessionSecret"), participant.sessionSecret, sizeof(participant.sessionSecret));
		copyText(stringOf(*session, "mediaKey"), participant.mediaKeyHex, sizeof(participant.mediaKeyHex));
	}

	// The create response names the code; the join response does not, so the
	// code that was asked for is the fallback.
	const Common::JSONObject *room = objectOf(root, "room");
	if (room)
		copyText(stringOf(*room, "code"), participant.code, sizeof(participant.code));
	else
		copyText(Common::String(requestedCode ? requestedCode : ""), participant.code, sizeof(participant.code));

	const Common::JSONObject *relay = objectOf(root, "relay");
	if (relay) {
		copyText(stringOf(*relay, "host"), participant.relayHost, sizeof(participant.relayHost));
		participant.relayPort = intOf(*relay, "port", 0);
		participant.relayProtocolVersion = intOf(*relay, "version", 0);
		participant.maxDatagramBytes = intOf(*relay, "maxDatagramBytes", 0);
		participant.maxPayloadBytes = intOf(*relay, "maxPayloadBytes", 0);
		participant.maxSeq = (uint64)int64Of(*relay, "maxSeq", 0);
		const Common::JSONObject *media = objectOf(*relay, "media");
		if (media)
			copyText(stringOf(*media, "encryption"), participant.mediaEncryption, sizeof(participant.mediaEncryption));
	}

	// The version gate. The budgets are compile-time constants on this side, so a
	// relay with different numbers is not a negotiation, it is a different build.
	Common::String problem;
	if (participant.relayProtocolVersion != kProtocolVersion) {
		problem = Common::String::format("relay speaks protocol version %d, this build speaks %d",
			participant.relayProtocolVersion, kProtocolVersion);
	} else if (participant.maxPayloadBytes != Wire::kMaxPayloadBytes) {
		problem = Common::String::format("relay payload cap is %d bytes, this build was compiled for %d",
			participant.maxPayloadBytes, Wire::kMaxPayloadBytes);
	} else if (participant.maxDatagramBytes != Wire::kMaxDatagramBytes) {
		problem = Common::String::format("relay datagram cap is %d bytes, this build was compiled for %d",
			participant.maxDatagramBytes, Wire::kMaxDatagramBytes);
	} else if (participant.maxSeq != Wire::kMaxSeq) {
		// A relay that counts sequences differently would silently start
		// rejecting frames, or worse, accept ones whose nonce namespace differs.
		problem = "relay's sequence ceiling is not the one this build seals with";
	} else if (strcmp(participant.mediaEncryption, kMediaEncryption) != 0) {
		problem = Common::String::format("relay expects media encryption \"%s\", this build knows \"%s\"",
			participant.mediaEncryption, kMediaEncryption);
	} else if (participant.peerId < 1 || participant.sessionSecret[0] == 0) {
		problem = "relay returned an incomplete session";
	} else if (participant.relayHost[0] == 0 || participant.relayPort < 1) {
		problem = "relay did not say where its UDP port is";
	}

	OwnerCredentials owner;
	memset(&owner, 0, sizeof(owner));
	const Common::JSONObject *credentials = objectOf(root, "credentials");
	if (credentials) {
		copyText(stringOf(*credentials, "invite"), owner.invite, sizeof(owner.invite));
		copyText(stringOf(*credentials, "ownerSecret"), owner.ownerSecret, sizeof(owner.ownerSecret));
	}

	*haveOwnerOut = credentials != nullptr;
	if (credentials)
		*ownerOut = owner;
	*out = participant;
	copyText(problem, reason, reasonCapacity);
	return problem.empty();
}

void Session::parseResponse(const Common::JSONValue *json) {
	char requested[Wire::kMaxRoomCodeBytes];
	char pendingRole;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		memcpy(requested, _requestedCode, sizeof(requested));
		pendingRole = _pendingRole;
	}

	Participant participant;
	OwnerCredentials owner;
	bool haveOwner = false;
	char reason[Wire::kMaxHeaderBytes];
	const bool gateOk = readResponse(json, requested, &participant, &owner, &haveOwner, reason, sizeof(reason));
	// The role is this client's own bookkeeping, never something the relay tells us.
	participant.role = pendingRole ? pendingRole : 'g';

	std::lock_guard<std::mutex> lock(_mutex);
	_participant = participant;
	_haveParticipant = participant.peerId >= 1 && participant.sessionSecret[0] != 0;
	if (haveOwner) {
		_owner = owner;
		_haveOwner = true;
	}
	_gateOk = gateOk;
	copyText(Common::String(reason), _gateReason, sizeof(_gateReason));

	if (!_gateOk)
		warning("coop: relay rejected by the version gate: %s", _gateReason);
}

void Session::onJsonValue(const Common::JSONValue *json) {
	if (json)
		parseResponse(json);

	std::lock_guard<std::mutex> lock(_mutex);
	_pending = false;
	_lastOk = json != nullptr;
	if (!json)
		copyText("relay sent no JSON", _error, sizeof(_error));
}

#ifdef USE_HTTP
void Session::onError(const Networking::ErrorResponse &error) {
	const Common::String detail = Common::String::format("HTTP %d %s", (int)error.httpResponseCode, error.response.c_str());

	std::lock_guard<std::mutex> lock(_mutex);
	_pending = false;
	_lastOk = false;
	copyText(detail, _error, sizeof(_error));
	debug("coop: control-plane request failed: %s", _error);
}
#else
// Without USE_HTTP there is no ErrorResponse to read, and no request to fail.
void Session::onError(const Networking::ErrorResponse &) {}
#endif

} // namespace Coop
