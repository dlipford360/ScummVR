# Co-op play in ScummVR: client integration guide

How the `android-vr` backend must be shaped to talk to **coop-relay**, the game-agnostic
relay in `coop-relay/`. Nothing described here is implemented yet — this is the
implementation contract, written after the relay was built, so it reflects what the relay
actually does rather than what was once hoped.

**Location convention:** this guide lives at the workspace root beside the other
`MULTIPLAYER-*.md` docs, **not** inside the `ScummVR/` fork — the fork stays clean of
feature docs about an external sibling repo. So paths that begin `backends/…`, `common/…`
or `dists/…` are relative to `ScummVR/`, and paths that begin `coop-relay/…` are relative
to this directory.

## 0. Where truth lives

| Document | Role |
|---|---|
| `coop-relay/README.md` | **the wire spec.** Frame types, byte layouts, budgets, credential model. Single source of truth. Never duplicate it here except where the game must act on a number. |
| `coop-relay/examples/dummy-client.ts` | a **working client** in TypeScript (fake host + two guests, 36 checks, `npm run example` in that repo). Not the C++ you must write, but every behaviour described below — sealing, fragmentation, the `X` catalogue, host-mixed voice, `B`→`K` acks — is demonstrated there and known to work against the real relay. Port it, do not re-derive it. |
| this file | how ScummVR is shaped to speak it: module layout, state machines, buffers, engine gotchas, milestone order |
| `MULTIPLAYER-FEASIBILITY.md`, `MULTIPLAYER-IMPLEMENTATIONS.md` | rationale (Model A vs B, why Quest, capture/encode notes). Their transport sections were written **before** the relay existed — read their "as built" notes at the top before trusting an interface detail. |

Current relay facts that shape everything below: protocol **v3**; frame types `V A M C I B K X P`;
host + up to 4 guests; one room-wide 256-bit **mediaKey** for client-side payload sealing; the
relay forwards **unordered, lossy datagrams and never retransmits**; fan-out is **broadcast-only**
for media (no host→single-guest, no guest→guest path — the one exception is `P`, which goes to
every *other* participant); voice is **host-mixed**.

## 1. Gate on the version before anything else

Costs one request and prevents an entire class of silent failure. A v1 client ignores frame types
it does not know, so a v2 peer waiting for a keyframe answer waits forever.

```
POST /rooms  →  relay.version == 3
                relay.media.encryption == "aes-256-gcm"
                relay.maxPayloadBytes == 1100
                relay.maxDatagramBytes == 1232
                relay.maxSeq == 9007199254740991
```

Refuse to join on mismatch and say why. `GET /` reports the same values plus the room/bitrate
limits, which is what a settings screen should read. `Coop::Session::readResponse` implements exactly
this and fills in a human-readable reason (`"relay speaks protocol version 1, this build speaks 3"`)
whether or not the gate passed, so the UI can show it instead of a generic failure. The budgets are
compile-time constants on the C++ side, which is why a different cap is a refusal rather than a
negotiation. `relay.maxSeq` is in that list for the same reason: it is the ceiling the sealing nonce
is built inside, and a relay that counts differently is a different nonce namespace, not a slower one.

Two things to take from that same response instead of hard-coding:

- **Where to send UDP**: `relay.host` + `relay.port`. The relay reports the port it *actually
  bound*, so a relay started with `UDP_PORT=0` is still reachable — do not assume 9797.
- **Your fragment budget**: `relay.maxPayloadBytes − 1` (envelope byte) `− 16` (GCM tag)
  `− 8` (fragment header) = **1075** today. Deriving it keeps you correct if the cap moves.

## 2. Control plane from C++

Do **not** hand-roll TLS in native code. The fork already has a JSON-capable HTTP client whose
Android implementation drives the Java TLS stack:

- `Networking::PostRequest(url, jsonCallback, errorCallback)` — `backends/networking/http/postrequest.h:47`,
  with `setJSONData(Common::JSONValue *)` for the body.
- Android connection layer: `backends/networking/http/android/connectionmanager-android.cpp`,
  backed by `backends/platform/android/org/scummvm/scummvm/net/HTTPRequest.java` and
  `TLSSocketFactory.java`.

Endpoints to call (`role` is `"host"` or `"guest"`):

| Call | When | Keep the result |
|---|---|---|
| `POST /rooms` | hosting | `code`, `invite`, `ownerSecret`, `session.sessionSecret`, `session.mediaKey` |
| `POST /rooms/<code>/sessions` | joining | `session.*` (same shape, including `mediaKey`) |
| `POST /rooms/<code>/kick` | host kicks someone (needs `ownerSecret`) | — |
| `POST /rooms/<code>/close` | closing (needs `ownerSecret`) | — |
| `GET /rooms/<code>` | lobby UI polling | roster is also pushed as `R` on UDP |

Close is documented as `DELETE /rooms/<code>` in the relay's README, but this fork's HTTP layer has
only GET and POST request classes — there is no `HttpDeleteRequest` — so the relay also accepts
`POST /rooms/<code>/close` with the same body and auth. Use the POST form from C++.

`relay.host` / `relay.port` in every response are what the client must connect to for UDP, and the
relay answers with whatever `RELAY_HOST` (falling back to `PUBLIC_HOST`) was set to — **`127.0.0.1` by
default**. On a headset that is a dead address, so a LAN test has to start the relay as
`RELAY_HOST=<the machine's LAN address>`, or the create call succeeds and then nothing ever reaches
`WELCOME`.

`Coop::Session` (`backends/platform/android-vr/coop-session.cpp`) is the as-built client for the first
three of these. Three rules from the framework shaped it, and they are worth knowing before writing
another request by hand:

- `Networking::PostRequest::handle()` is empty, so the request does nothing unless the caller calls
  `start()`; `start()` is also what serialises the body and registers the inner request. Then
  `ConnMan.addRequest(postRequest)` hands ownership over — the connection manager deletes a request
  once it reaches `FINISHED`. The body `JSONValue` is *not* owned by `PostRequest`, so the caller
  deletes it right after `start()`.
- `PostRequest` deletes the `JSONValue` it passes to the callback the moment the callback returns.
  Copy everything out eagerly; keeping a pointer is a use-after-free.
- Callbacks run on the connection manager's **timer thread**, not the game thread. Hence `Session`
  parses into mutex-guarded fields and the game thread reads them through accessors; it also means a
  `Session` must outlive its requests, which is what the destructor's wait loop is for.

The response mapping is deliberately a pure static, `Session::readResponse`, precisely so it can be fed
the relay's real JSON without any networking; that is how the field names are checked on a host
machine. The version gate in the same function refuses a relay whose advertised protocol version,
datagram cap, payload cap, sequence ceiling (`relay.maxSeq`) or media encryption scheme differ from
the constants this build was compiled against — those numbers are compile-time constants on the C++
side, so a mismatch is not negotiable and the reason string is meant to be shown in the UI rather than
logged. A missing `maxSeq` is refused as well: a relay that does not state the ceiling it counts
inside is a relay whose nonce namespace is unknown, and `Session::readResponse` maps it through
`int64Of` because an `int` would truncate 2^53−1 into something meaningless.

`joinRoom()` is guest-only by design — the C++ client has no host-rejoin path, because the relay
refuses one (`409 host_session_exists`). A host that lost its ticket calls `createRoom()` again (§13).

**A thread that calls `Session` must be attached to the JVM.** The control plane is the Java HTTP
stack, and `JNI::fetchEnv()` calls `GetEnv()` and then `abort()` — it deliberately does not attach.
So calling `createRoom()`/`joinRoom()` from a thread that never called `JNI::attachThread()` does not
fail gracefully, it kills the process:

```
Coop::Session::createRoom → PostRequest::start → ConnectionManager::instance()
  → ConnectionManagerAndroid → JNI::getEnv → JNI::fetchEnv → abort()   (SIGABRT)
```

This was the first thing the device run found (2026-10-23), because the self-test runs on a
`std::thread`. The fix follows the pattern `OSystem_Android`'s own timer and audio threads use:
`JNI::attachThread()` on entry, `JNI::detachThread()` on exit. Note what this is *not*: the transport
is plain POSIX UDP and needs no JNIEnv, so a game's coop thread only has to worry about this if it
makes control-plane calls.

**Nobody in the game drives the control plane, and callbacks do not run on your thread.**
`ConnectionManager::addRequest` calls `startTimer()`, which installs `connectionsThread` as a timer
proc (100 Hz) on `g_system->getTimerManager()`; on Android that timer manager is driven by
`OSystem_Android`'s own timer thread — a thread that *is* JVM-attached, which is why the classic port's
HTTP path works without any game-side driving. So `createRoom()` is fire-and-forget: there is no
`update()` to call, and the response callback lands on that timer thread, not the caller's. That is the
second reason `Session` keeps its parsed state behind a mutex.

**`USE_HTTP` has never worked in the VR port** (found on the second device run, 2026-10-23).
`Networking::NetJNI::init(env)` — the call that resolves `org.scummvm.scummvm.net.HTTPManager` and
friends into the static `jclass` cache — sits at the end of the classic `JNI::init()` and nowhere else,
so `JNI::createVR()` left the cache empty and the first request aborted:

```
Abort message: 'JNI DETECTED ERROR IN APPLICATION: NewObjectV received NULL jclass'
  at Networking::ConnectionManagerAndroid::ConnectionManagerAndroid()   (NewObject(_CLS_HTTPManager, …))
```

Adding the call is not sufficient, and this is the trap to know about: the VR port bootstraps on a
thread it created itself, and `JNIEnv::FindClass` resolves an application class using the class loader
of the *caller's frame* — on such a thread it returns null for `org.scummvm.*` while still resolving
platform classes like `java.net.URL`. That is why `createVR` already skips the SAF classes ("class may
not be loadable from native threads") and why `android-vr-main.cpp` caches a class loader at startup.
So `NetJNI` gained `setClassLoader()` and a `ClassLoader.loadClass` fallback behind `FindClass`, and
`createVR` hands it the loader obtained from its own object's `Class` object before calling `init()`.
The classic path is untouched: with no loader installed the fallback is never used. All four classes the
native side resolves (`SSocket`, `HTTPManager`, `HTTPRequest` — `init()` aborts if any is missing) are
present in the VR APK's dex files, which is worth checking before assuming a name problem.

**Credential handling rules.** `invite` is shareable; `ownerSecret` is not; `sessionSecret`
authorises one `peerId`; `mediaKey` decrypts everyone's media. None of them may ever:

- appear on the UDP path, least of all inside an `X` payload — a broadcast `X` is delivered to every
  participant, and anyone who can read a room code plus one invite can join and read it too;
- appear in `debug_printf`/logs or crash reports;
- be written into a savegame. If you want a "recent rooms" feature, keep them in app-private
  storage, not in `~/Documents/ScummVR/`, which is browsable by the game's own file dialog.

**Where the key goes in code.** `Coop::Session` hands `mediaKey` back as 64 hex characters; pass it to
`Coop::Transport::setMediaKey(hex)` **before** `join()`, because the receive thread starts inside
`join()` and reads the key. It validates the length rather than truncating, and it does not echo the
value. There is no rotation API and no way to clear the key except `close()`, which wipes it: one key
per room for the room's lifetime (§12).

**The control plane is HTTPS, and the relay can serve it itself.** Setting `TLS_CERT_FILE` and
`TLS_KEY_FILE` makes the control-plane port speak TLS only — cleartext on that port fails rather than
downgrading, and `GET /` reports `"tls": true`. Production should still terminate TLS at a proxy with
a public certificate; the in-process listener exists so a LAN bring-up needs no extra infrastructure.

Cleartext is not an option on this platform, which is worth knowing before you spend an hour
debugging it: `HTTPRequest.java:232` uses `HttpURLConnection`, and at `targetSdkVersion 32` the
platform blocks cleartext outright. A `http://192.168.0.30:18090` request dies in Java before it
reaches a socket, with `CLEARTEXT communication to … not permitted by network security policy`.

So a LAN test needs a **trusted** chain, and a LAN address makes that awkward: the certificate must
carry an **IP `subjectAltName`** (a `CN` alone fails Java hostname verification).
`coop-relay/scripts/lan-cert.sh <address>` generates a throwaway CA plus such a certificate and
copies the CA to `dists/android/res/raw/coop_relay_ca.pem`, which
`dists/android/res/xml/network_security_config.xml` lists as a `debug-overrides` trust anchor.
`debug-overrides` is deliberate: it applies only to a debuggable APK, so a release build falls back to
system anchors and a real certificate. Regenerate the CA per network, and keep `coop-relay/certs/` out
of version control — whoever holds that key is trusted by every APK carrying the anchor.

The address in the SAN is the fragile part on a LAN: a dev machine's DHCP address moves, and
the marker's `relay =` and the certificate then name an address nobody answers (this happened
once: the Mac went from `.30` to `.20` and the host saw `Host unreachable` at `POST /rooms`).
The recovery is cheap because `lan-cert.sh` **reuses** the existing CA: re-run it with the new
address, restart the relay with `PUBLIC_HOST`/`UDP_BIND` on it, and update the marker. The CA
bytes do not change, so the APK keeps trusting it — no rebuild. Starting the relay bound to
`0.0.0.0` while advertising a specific address is the other way this breaks; `config.ts` names
that hazard and `UDP_BIND` exists to avoid it.

One caveat that has not been verified on a headset yet: this fork's `HTTPManager` installs
`TLSSocketFactory` as the default socket factory, and its `LETrustManager` delegates to the platform
trust manager obtained from `TrustManagerFactory.init((KeyStore) null)`. On Android that factory does
honour the network security config, so the bundled anchor *should* work through the indirection — but
if it does not, the coop self-test prints the Java-side TLS failure and the fix is a certificate from
a public authority under a real hostname, which needs none of this. Do not solve a trust failure by
relaxing cleartext: the `mediaKey` and the `sessionSecret` both ride that request.

## 3. Module shape

New code under `backends/platform/android-vr/`, one concern per file. The fork does **not** keep a
generic `Common::Network` facade, and the relay speaks bare datagrams — so the data plane is plain
POSIX UDP (`SOCK_DGRAM`), *not* ENet: `backends/networking/enet` would wrap your frames in its own
framing and the relay would fail to parse them.

| File | Responsibility | Status |
|---|---|---|
| `coop-wire.cpp/.h` | pure byte layer: budgets, header lines, `SEQ` counters, fragment header, reassembler, envelope byte. No allocation, no `Common::String` in the formatting path | written |
| `coop-transport.cpp/.h` | connected UDP socket, receive thread, `HELLO` retry + `PING` keepalive, per-(peer,type) `SEQ`, `X`/`C`/`I`/`B` sends, video fragmentation, inbound event queue, roster pushes | written |
| `coop-session.cpp/.h` | HTTP control plane through `Networking::PostRequest` (create / join / close), credential store, version gate | written |
| `coop-seal.cpp/.h` | the sealing half of the envelope: vendored AES-256-GCM (encrypt-only AES core, GCTR, GHASH), a port of `coop-relay/src/mediaCrypto.ts`. `Seal::sealPayload` builds the AAD itself by calling `Wire::formatForwardedHeader` — that one line is both the AAD and what the recipient parses, so a caller-supplied AAD string is how a port silently fails to decrypt | written; host-verified byte-for-byte against the relay's own implementation (§12) |
| `coop-video.cpp/.h` | host: encoded-sample feed → `sendMedia` → IDR forcing. guest: `FrameReassembler` → `AMediaCodec` decoder → texture | planned |
| `coop-gameaudio.cpp/.h` | host: mixer tap → Opus → `A`. guest: jitter buffer → `AudioStream` fed to `Mixer::playStream` (`audio/mixer.h:130`) | built; audible on a Quest 2 with a human "sounds good" (§16, room `VWWGV`) |
| `coop-voice.cpp/.h` | AAudio capture → Opus → `M` (both roles); host-side per-speaker jitter buffers + mixdown; PTT state | planned |
| `coop-input.cpp/.h` | `I`/`B`/`K`; host-side authoritative input policy; injecting a permitted guest's input into the local engine | planned |
| `coop-cursor.cpp/.h` | host: `C` from `AndroidGraphicsManager::getMousePosition()`. guest: per-peer overlay on the screen quad | planned |
| `coop-pose.cpp/.h` | both: local head pose from `OpenXRSession`'s tracked space → 20-byte `P` payload at ≤30 Hz with a deadband; inbound → remote avatar only, never the local view | planned; the wire half (`P`, its routing and its `SEQ` slot) is done and tested (§11) |
| `coop-chat.cpp/.h` | `X` chat render + input (`gui/widgets/edittext.cpp`; the IME comes from `kFeatureVirtualKeyboard` → `JNI::showVirtualKeyboard`) | planned |
| `coop-selftest.cpp/.h` | bring-up probe on a device: marker file at `/sdcard/scummvm-coop-test.txt`, then create/join over the control plane, `HELLO`→`WELCOME`, one `X` kind 7 and one 4 KB probe a second, and a seal bench with the real room key. Its own thread; capped at 60 s joined | written; compiles for `aarch64-linux-android29` and links into `libscummvm.so`, **not yet run on a device** (§14) |

The five written files are wired into `VR_OBJS` in `backends/platform/android/module.mk`. Verification
is now two layers, and the distinction matters:

- **host-verified behaviour**: built with `clang++ -std=c++11` on macOS and run against a live relay —
  the byte-level parity and round-trip claims below;
- **arm64-verified build**: this machine does have the SDK and NDK 23.2.8568313, so all four units
  compile clean for `aarch64-linux-android29` at `-Wall -Wextra` with NDK clang 12, `make
  libscummvm.so` links them into the shipping shared object, and a debug APK packages it. The link
  step caught a real bug the host harnesses could not see: `Session::pending()` and its sibling
  accessors were declared and never defined, which only shows up once something references them.
  **Nothing has executed on a headset yet**, and that includes the crypto — an x86_64 host cannot run
  an arm64 binary, so sealing cost and correctness on the shipping ABI are still a device measurement.

`coop-session.cpp` needs `USE_HTTP` (the configured VR `config.mk` sets it, and `--host=android-*`
selects it automatically); without it the file still compiles and the calls report that the relay is
unreachable. `coop-seal.cpp` needs nothing at all — that is the point of vendoring it (§12).

What the host checks cover today: the C++ header lines are byte-identical to the relay's own
`headerBytes()`; the relay's parser accepts every C++ `D`/`HELLO`/`PING`/`BYE`; the vendored AES-256-GCM
produces the same bytes as the relay's `sealPayload` for twelve vectors and OpenSSL opens what the C++
sealed, with every tamper case (flipped ciphertext byte, rewritten `SEQ`/`peerId`/role/room code, wrong
key, truncated payload) rejected on both sides; and a two-process C++ run against a live relay
round-trips `X` in both directions and a 4 KB video frame through sealing, fragmentation and reassembly
byte-for-byte, including the negative case where a guest holds a different key and authenticates
nothing. Bugs found by writing this way: `sendMedia` wrapped its staging buffer instead of the filled
prefix (out-of-bounds read, wrong reassembled length) and the reassembler took the frame length from
whichever fragment closed a gap instead of the last one, both caught by the live-relay run; a first
draft of `mixColumns` used the common-term trick as `t ^ xtime(a0^a1)`, which computes `3a0 ^ 3a1`
instead of `2a0 ^ 3a1` — caught while re-deriving the identity, before the vectors ran, which is worth
recording because the vector comparison is exactly the check that would have caught it too.

Build wiring: add further `.o` entries to the same `VR_OBJS` list. APK/gradle targets live in
`backends/platform/android-vr/android-vr.mk`.

**Threading.** One dedicated coop thread owns the socket and all seq/ack state. It never calls into
the engine. Handoffs:

- video: coop thread → decoder → GL thread uploads into a texture. The VR backend already moves
  pixels across threads this way — the graphics manager writes the game surface into a shared
  texture (`initSharedTexture`, `backends/platform/android-vr/android-vr-main.cpp:1246`) which
  `drawScreen` (`:523`) draws on the quad; the remote-video texture should use the same pattern, not
  a new synchronisation scheme.
- input: coop thread → `Common::Event` pushed onto the event source (the path `processInput`,
  `:841`, already feeds).
- audio/voice: coop thread decodes into ring buffers; the ScummVM mixer pulls from them on the audio
  callback thread. Do not let the audio callback do networking.

## 4. State machines

**Host**

| State | Enter when | Exit / obligation |
|---|---|---|
| `IDLE` | — | nothing on the wire |
| `CREATING` | user picks Host | `POST /rooms`; on failure back to `IDLE` with the error text |
| `WAITING` | session issued | `HELLO` → `WELCOME`; encoder running or idle; show code + invite on the quad |
| `STREAMING` | first guest bound | full `V A C` egress; mixing `M`; enforcing input policy |
| `DEGRADED` | egress/budget pressure self-detected | lower encoder bitrate/resolution **before** the relay drops you (the relay disconnects, it never degrades for you) |
| `CLOSED` | `DELETE`, or relay reports closed | stop all TX; `BYE`; zero secrets in memory |

**Guest**

| State | Enter when | Exit / obligation |
|---|---|---|
| `IDLE` | — | nothing |
| `JOINING_HTTP` | code + invite entered | `POST .../sessions`; gate the version here |
| `BINDING` | session issued | `HELLO`; retry once on timeout, then surface `bad_secret`/`revoked`/`room_closed` distinctly — they mean different things to the user |
| `RENDERING` | first complete video frame | draw quad + overlays; send `I`/`C`/`M` per policy |
| `STALLED` | no complete frame for > ~250 ms | keep last frame + "waiting" hint; `X` keyframe request (rate-limited); do **not** blank the quad |
| `KICKED` / `CLOSED` | `KICKED`/`CLOSED` frame | stop TX immediately, explain, return to lobby |

A guest's local ScummVM still runs — it owns the launcher, the options dialogs and the chat UI — but
**its engine is not the game**. The game instance lives on the host. Do not let a guest's own input
reach its own engine once remote play is active, or you get a local copy of the game reacting to
nothing.

## 5. Latency budget, and the gate you must pass first

Model A is only acceptable if one-way latency stays in the range where pointing at a 320×200 object
still feels direct. Gate: **35–110 ms one-way**, measured, before committing to pointer-driven
co-op. Per stage, roughly:

| Stage | Budget |
|---|---|
| capture → encoder input | 8–16 ms (PBO readback, no sync `glReadPixels`) |
| encode (low-latency MediaCodec, no B-frames) | 8–25 ms |
| uplink + relay + fan-out + downlink | 5–30 ms on a real connection |
| decode | 3–8 ms |
| guest jitter buffer | 30–80 ms ← the part you control |
| scan-out / compositor | 8–16 ms |

Measure, don't infer: `PING`/`PONG` gives network RTT (divide by two), and the app-level kind 7/8
exchange below measures queueing on top of that. Then do the honest test — tap the controller and
film the screen — because encoder and jitter-buffer costs are invisible to both. If the number lands
above ~110 ms, the feature is still fine for turn-based and click-to-move pacing, and the right
answer is to say so in the UI rather than to ship it as if it were local.

Update from the first measurements (2026-10-23): the Quest 3 **encode** stage came in at **p50 37 ms
/ p95 73 ms** submit-to-AU while actually feeding a room, not the 8–25 ms budgeted above (35.4/37.8
when benched alone — the gap is the feed loop sharing a thread with the room loop; §14 has both
runs and the instrumentation trap around them). The **decode** stage first measured **p50 23.7 ms /
p95 116.6 ms** AU-complete-to-decoded from a paced Mac feed, but the two-headset run (Quest 3 →
relay → Quest 2, same day) put the decoder's own contribution at **p50 2.5 ms / p95 3.4 ms** — the
earlier number was mostly feed cadence, not codec work. So the codec reserves ~45 ms of the 110 at
p50 (essentially the encoder's 1-frame pipeline), and the budget hinges on capture/render staying
near zero and the network plus jitter buffer staying near their 1–3 ms LAN values, which makes the
end-to-end measurement M1's real verdict rather than a formality. Design the capture path off the
room loop's thread from day one.

## 6. Host video path

- Capture after the blit in `AndroidVRGraphicsManager::refreshScreen()`
  (`backends/graphics/android-vr/android-vr-graphics.cpp:232`) into a PBO, async readback. Capture
  the single source texture, never each eye image.
  Built and device-verified (2026-10-23, §14) as `backends/platform/android-vr/coop-capture.{h,cpp}`,
  called from `refreshScreen()` only while something has asked for frames. The region to read is
  the game's own video mode, which `loadVideoMode` must remember — the backbuffer is bigger than
  the game, and nothing else in the VR path records it. Two PBO slots and a **zero-timeout** fence
  poll, so the graphics thread never blocks on the GPU; a frame that has not landed by the next
  refresh is missed and counted, never waited for. `glUnmapBuffer` must be called **while the
  buffer is still bound** — after an unbind it is an invalid operation that leaves the buffer
  mapped, and this driver then refuses every later map. The readback keeps the framebuffer's
  bottom-first orientation (no flip in GL); the consumer converts RGBA→NV12 top-first on its own
  thread, which is where the flip and the color matrix belong.
- Encode 640×480@30 for a first target, `BITRATE_MODE_LOW_LATENCY`, no B-frames, IDR every 1–2 s.
  Classic SCUMM is 320×200/320×240, so 640×480 is visually lossless. Measured on the Quest 3
  (2026-10-23, §14): that configuration holds its bitrate cap, honors the IDR interval exactly, and
  costs **35.4 ms p50 / 37.8 ms p95** submit-to-AU benched alone — the codec's 1-frame pipeline at
  30 fps, not a per-frame processing cost — rising to **p95 73 ms** when the encoder is fed while
  its thread also serves a room. Ask for the input layout explicitly (`color-format` = NV12) and
  verify the read-back: left to itself, the Quest codec answers a vendor format and a buffer smaller
  than the computed frame. `latency` / `low-latency.enable` format keys are accepted but change
  nothing on this codec.
- Fragment per the relay's spec, **and do the MTU math after sealing**: payload ≤ 1083 bytes sealed
  → minus the 8-byte fragment header → **≤ 1075 bytes of NAL data per datagram**. `SEQ` increments
  once per datagram, so every fragment consumes one.
- **Every fragment but the last must carry exactly those 1075 bytes.** The fragment header carries
  `frameId`/`index`/`count` and no offsets, so the receiver places fragment *index* at
  `index * 1075`; a short *middle* fragment makes the frame unplaceable and both the relay's reference
  client and the C++ `FrameReassembler` drop it rather than guess. `Transport::sendMedia` gets this
  right by construction (its loop arithmetic only shortens the final chunk) — do not hand-roll a second
  fragmenter next to it.
- Drop the whole frame on a gap or timeout. Partial frames poison the decoder.
- Keyframes: a guest that loses a fragment loses the frame; it asks for an IDR over `X` (kind 1).
  **Rate-limit it** — one IDR per peer per ~500 ms, and drop requests while the encoder is already
  producing one. An unthrottled IDR request is a self-inflicted DoS on every guest, because an IDR is
  tens of kilobytes and fan-out multiplies it by the guest count.

**The cursor prerequisite — shipped, and central rather than per-engine.** The host must not
composite its *VR pointer* into the captured frame: the floating circle the wearer points with is
what the guest must receive live as `C`, and a baked-in copy of it would arrive video-stale.
(The in-game cursor is a different thing: the owner has explicitly said its presence in the
stream is fine and not worth chasing.) An earlier draft of this guide guessed the lever would be
per-generation and engine-side (`OSystem::showMouse` looked too weak because SCUMM-era engines
draw their own cursor); exploration of this fork proved that guess wrong. SCUMM-era "engine-drawn"
cursors still go through the *backend*: `Graphics::CursorManager` hands the cursor bitmap to
`OpenGLGraphicsManager`, whose `updateScreen()` composites in a fixed order — game screen,
overlay, cursor (`renderCursor()` GL quad), OSD. So the lever is one virtual: `preCursorHook()`
in that base class, called after the overlay step and before the cursor and OSD;
`AndroidVRGraphicsManager` overrides it and runs the coop capture there (§6 capture path). The
host's own display is untouched — it keeps cursor and OSD — and the copy happens to exclude both
(a harmless bonus given the game-cursor ruling). The VR pointer dot (`makeCursorTexture` /
`uiCursorTex`, `android-vr-main.cpp`) needed no change: the VR thread draws it over the shared
texture, never into the captured FBO, so it was never in the stream — confirmed by the wearer on
a live run (room `ERADD`: no circle in the streamed picture). The machine-side evidence that the
game cursor is also out of the copy: capture probes at 0/10/90 s of DOTT — the 90 s frame is the
interactive scene and carries no cursor (§14). The sequencing stands: **the `C` channel puts a
pointer back on the guest's screen**, sent from `AndroidGraphicsManager::getMousePosition()` (the
VR manager inherits it; it is what `backends/platform/android/events.cpp:630` already uses). Do
not take the shortcut of baking any cursor into the video: it arrives a full RTT stale and
rescaled, which defeats the `C` channel.

## 7. Guest render path

`C` coordinates are normalised 0…10000 against the game frame, so letterboxing and scaling are free:
map the normalised pair onto whatever quad rect the video actually occupies (the geometry helpers in
`backends/platform/android-vr/vr-screen-geometry.cpp` already know that rect). Draw one overlay per
`peerId` from the identity header.

Two options for the video itself:

1. **Recommended:** a dedicated remote-video quad, drawn in place of the game surface while remote
   play is active, with the local ScummVM GUI still available for menus and chat. Cheapest to
   integrate, keeps the launcher usable, and leaves the local game surface untouched.
2. Feed decoded frames through the existing shared-texture path so the guest's own screen *is* the
   remote frame. Simpler to look at, but you lose the local GUI unless you composite it back in.

The receive plumbing already exists: `Transport::pollEvent` hands up a parsed `F` frame whose payload
has been authenticated and stripped of its envelope on the receive thread (nothing points into the
socket buffer, and nothing that failed the tag is ever queued), `Wire::readFragmentHeader` reads
`frameId`/`index`/`count` from offset 0, and `Wire::FrameReassembler` returns complete payloads in
order of arrival. Use one reassembler per media type — video and voice must not share slot state.
`event.envelope` records which envelope was on the wire (`0x01` once a media key is set) and
`Transport::cryptoDrops()` counts what was refused, which is the counter to surface if a guest suddenly
stops seeing video.

Do not attempt A/V re-sync signalling that the relay does not provide. `A` and `V` are separate
streams clocked by the host's media; keep both jitter buffers bounded and drop late video frames and
stale audio rather than chasing drift.

## 8. Input path

Payload layouts and the ack/reconciliation loop are in the relay README (`I` 16 bytes at ~60 Hz,
`B` 7 bytes edge-triggered, `K` 4-byte ack, `eventId` monotonic per sender, reconcile on gaps).

The two obligations that are easy to get wrong:

- **Release everything on silence.** A guest that vanishes must have all buttons released and sticks
  neutralised after ~500 ms. Stuck input is the worst failure mode in this design.
- **Release everything on revoke.** When the host drops a peer's level to 0, that peer's input must go
  through the *same* release path as the timeout. Two code paths that both "stop input" is how stuck
  buttons ship.

Input policy is the host's decision, expressed over `X` kind 3: level `0` none, `1` pointer only,
`2` full controller. The relay will happily forward input from a peer you have revoked — enforcement
is yours.

## 9. Voice

Host-mixed: a guest's `M` reaches **only** the host; the host decodes every speaker, mixes, and emits
one `M` stream. There is no guest→guest path, so this is not a design preference — it is the only
shape the relay permits.

- Capture: **AAudio** on its own thread (API 28+; Quest is fine), 16 kHz mono, or Java
  `AudioRecord` over JNI. Add `RECORD_AUDIO` to `dists/android-vr/AndroidManifest.xml`
  (`INTERNET` is already there, line 9).
- Codec: libopus, **20 ms frames, 24–32 kbps, inband FEC + DTX on**. The 20 ms frame length is not a
  taste choice: it is what makes `SEQ` gaps equal 20 ms of concealable loss.
- Jitter buffer + PLC driven by the authenticated header `SEQ` (there is no sequence number inside
  the `M` payload; see the README's voice section). ~40–60 ms, not the 50–100 ms game audio wants.
- Playback on the guest: a small `AudioStream` subclass over the voice ring buffer, played with
  `Mixer::playStream` (`audio/mixer.h:130`), so voice mixes with game audio naturally and inherits
  the mixer's volume controls.
- Mute is **host-enforced by not mixing**. Separately, send kind 4 so a muted guest stops
  transmitting and saves its uplink.
- PTT by default. Bind it to an unused OpenXR action in `backends/platform/android-vr/openxr-input.cpp`
  (thumbstick-click or left grip). It also solves echo, which is otherwise the ugliest voice bug: the
  host's microphone hears the host's own game audio. `AcousticEchoCanceler` later, PTT now.
- One benefit of host mixing worth taking: the host can duck game audio, or attenuate a specific
  speaker, *before* mixing, and it costs nothing extra on the wire.

## 10. Text chat

Needs no relay feature: it is `X` kind 5. One message per datagram, so cap UTF-8 text at ~240 bytes
after the envelope and application framing. Rate-limit locally (~1/s) — `X` shares the guest's
512 kbps budget, and flooding it degrades everyone's video. For input, do not write a
keyboard: the Android backend already raises the system IME for a ScummVM edit field —
`kFeatureVirtualKeyboard` is reported by `OSystem_Android`
(`backends/platform/android/android.cpp:736`) and routed to
`JNI::showVirtualKeyboard` (`backends/platform/android/jni-android.cpp:417`), and the VR
build uses that same OSystem (its graphics manager is swapped in at
`android.cpp:591`). So `gui/widgets/edittext.cpp` plus
`sys->setFeatureState(OSystem::kFeatureVirtualKeyboard, true)` gives you typing on Quest.

Because `X` fan-out is broadcast-only, a "whisper" is carried with a target `peerId` in the payload
and **is still received by everyone else**. Every participant also holds the room `mediaKey`. So a
whisper is addressed, not confidential; if you want confidential messaging, that is a per-recipient
key, which a room-wide key is not, and it is a feature decision rather than a detail.

## 11. The `X` payload catalogue — this table is the game-side spec

```
byte 0      kind
byte 1-2    uint16le targetPeerId   (0xFFFF = everyone; guests send 0xFFFF to the host)
```

Every recipient drops an `X` frame whose `targetPeerId` is neither `0xFFFF` nor its own `peerId`,
and drops unknown kinds silently. Nothing in an `X` payload is ever a secret.

| kind | direction | after the common header | notes |
|---|---|---|---|
| 1 `KEYFRAME_REQUEST` | guest→host | — | host forces an IDR, rate-limited per peer |
| 2 `STATS_REPORT` | guest→host | `u16le rttMs, u8 lossPercent, u32le downKbps, u8 freeKbps` | lets the host degrade **its own** encoder before the relay disconnects it |
| 3 `INPUT_POLICY` | host→all | `u8 level` (0 none / 1 pointer / 2 full) | host must run the input release path when a level drops |
| 4 `VOICE_POLICY` | host→all | `u8 allowed, u8 maxKbps` | saves a muted guest's uplink; the actual mute is host-side mixing |
| 5 `CHAT` | both | `u16le authorPeerId, u8 flags (bit0 whisper), u8 textLen, utf8 text[]` | `authorPeerId` is inside the payload because a host re-broadcast is stamped as the host's |
| 6 `HOST_INFO` | host→all | `u8 appMajor, u8 appMinor, u8 videoCodec, u8 voiceCodec, u16le encWidth, u16le encHeight` | app-level capability exchange; the relay's `R` roster carries none of this |
| 7 `APP_PING` | both | `u64le clientMicros` | measures app-level queueing, unlike relay `PING` |
| 8 `APP_PONG` | both | `u64le clientMicros` echoed | |
| 9 `VIDEO_RETRANSMIT_REQUEST` | guest→host | `u8 count, u32le frameId[count]` (count ≤ 8) | asks the host to re-send whole video frames by id. Whole frames, not fragments: a partial video frame is worthless by construction, so its value is entirely in its last fragment. The host answers from a short AU send history (`Transport::enableMediaHistory`/`resendMedia`, ~2 s / 1 MB); fragments are idempotent at the reassembler, so a partially-lost frame simply completes. Rate-limited by the requester: ≥80 ms grace (reordering), ≤1.5 s age, ≤8 ids per request, ≤1 request per 250 ms. Kind 4 was taken by `VOICE_POLICY` — this is 9. |

Version the catalogue by bumping `appMajor`/`appMinor` in kind 6 and refusing to play against a
major mismatch — the relay will not do this for you, it only gates the wire version.

### Head pose (`P`) — not part of the `X` catalogue

`P` is its own frame type with its own routing, not an `X` kind, because it is the one channel that
goes to **every other participant** rather than to the host or to the guests. The byte layout, rate
cap and staleness rules are in the README ([Head pose (`P`)](coop-relay/README.md#head-pose-p)) —
this section only records what the C++ side already assumes:

- `isKnownFrameType('P')` is true and `maySendAs()` allows it for **both** roles — the only type a
  guest and a host may send alike, because everyone publishes their own pose;
- it has its **own** `SeqCounter` slot: `SeqCounter::slotFor('P')` returns 8, and `kSeqSlots == 9`.
  A pose frame must not advance the video counter: the nonce includes the type, so sharing a counter
  would be survivable but would make a rewind in one channel invisible in the other. `wire_check`
  asserts the slots are distinct; `wire_parity.ts` asserts the slot count matches the relay's frame
  types;
- the payload is a fixed 20 bytes, so it never needs fragmenting — send it with
  `Transport::sendFrame('P', payload, 20)`, **not** `sendMedia()`, which adds a 8-byte fragment header
  and would put the pose over budget for no reason;
- nothing publishes or consumes `P` yet. Reserving it is what lets presence land as a payload change
  instead of another wire version bump.

## 12. Sealing in C++

Port `coop-relay/src/mediaCrypto.ts` rather than inventing your own, and keep the test file
`coop-relay/test/mediaCrypto.test.ts` as the conformance checklist for the port.

- envelope byte `0x00` plaintext / `0x01` sealed, then `ciphertext ‖ 16-byte GCM tag`;
- nonce (12 B, never sent) = `0x00 ‖ type ASCII ‖ peerId u16BE ‖ SEQ u64BE`;
- **AAD = the exact forwarded header line** `F <CODE> <h|g> <PEERID> <TYPE> <SEQ>\n`, built by the
  single header-formatting function in the codebase. Construct it from the *forwarded* form (role tag
  + your own `peerId`), not from the `D` line you put on the wire;
- refuse `0x00` when encryption is required, and treat an authentication failure as "drop the frame" —
  never render partial output;
- **never reset a `SEQ` counter while the media key is in use.** The nonce *is* (type, peerId, SEQ);
  GCM nonce reuse is a total break. If you approach the counter's limit (`Wire::kMaxSeq`, which the
  relay advertises as `relay.maxSeq`), end the session and rejoin for a fresh key;
- **one connection instance is one identity.** `Coop::Transport` enforces it: `join()` is one-shot per
  object, and reconnecting means a fresh `Transport` plus a fresh control-plane session. That is not
  tidiness — the relay hands a *guest* a new `peerId` every time, but it never re-mints a *host*
  identity, and `POST /rooms/:code/sessions` with `role:"host"` now answers
  `409 host_session_exists`. A host that lost its ticket hosts a new room; there is no path back into
  the old one under the same key;
- the relay polices the same rule from its side: it keeps a sliding replay window per (peer, frame
  type) — a high-water `SEQ` plus the set of recently accepted seqs — and only faults what cannot be
  UDP reordering: a *seen* `(type, SEQ)` pair is dropped, and a `SEQ` far below the window (which can
  only be a restarted counter) revokes the sender with `KICKED … seq_regression`; a *host* doing that
  closes the room (`CLOSED host_seq_regression`). Ordinary reordering is forwarded. Reaching the kick
  is a bug in your client, and it is also a moment to treat the room's key as suspect;
- there is no key rotation: a media key lives as long as the room, so changing it means closing the
  room and creating a new one.

**As built**: option 1 — a vendored AES-256-GCM in `coop-seal.cpp/.h`, no OpenSSL and no JNI. The
conformance method is the interesting part, because a crypto port that is merely self-consistent is
worthless:

- vectors come from running the relay's own `sealPayload` (OpenSSL underneath); the C++ output must
  equal them byte for byte, which pins the AES core, the GHASH multiply, the counter rule and the AAD
  rule all at once;
- the C++ output is then opened by the relay's `openPayload`, so the check is not one-directional;
- tamper cases in both directions: flipped ciphertext byte, rewritten `SEQ` / `peerId` / role tag /
  room code in the forwarded header, wrong key, truncated payload, unknown envelope byte, output
  buffer too small;
- `SEQ` = 2^53-1 is usable and one past it is refused by `Seal::nonceFor` and by `Wire::SeqCounter`;
- the whole path is then run against a live relay: sealed 4 KB video reassembles byte-identical on a
  guest that holds the key, and a guest holding a *different* key authenticates nothing (`cryptoDrops`
  climbs, no frame ever reaches the reassembler) while its session stays up.

Things a reviewer should know before reading the file:

- only the AES **encryption** direction is implemented. GCM uses the block cipher for the CTR
  keystream, for `H = E(0^128)` and for `E(J0)`; it never decrypts a block, so there is no inverse
  cipher here and that is not an omission;
- GHASH uses the SP 800-38D shift-and-xor multiply rather than tables, because the table form wants a
  per-key table rebuilt for every fragment. Cost on an x86 host at `-O2`: about 200 µs per 1083-byte
  fragment per direction (~5 MB/s single threaded), i.e. roughly a tenth of a core at 4 Mbit/s of
  video. If a headset profile says that is too much, the fix is ARMv8 carry-less multiply
  (`vmull.p64` / `PMULL`) or a 4-bit table cached with the key — both local to `gfmul`;
- the tag is compared in constant time, and plaintext is produced only after it passes;
- key material is zeroised on the way out of every function, best effort through a volatile pointer,
  and `keyFromHex` checks the length before parsing so a short control-plane string is refused
  without reading past its terminator.

**`Seal::Context` — key-dependent work happens once per key, not once per frame.** The media key is
installed with `Context::setKey` / `setFromHex`, which expands the AES-256 schedule, derives
`H = E(0^128)`, and fills the GHASH tables. `sealPayload` / `openPayload` take a `const Context &`
and refuse to do anything (`0` / `-1`) when it holds no key. All the GCM internals are private to
those two functions.

GHASH is table-driven: multiplication is linear in its input, so a block is 16 nibble-pair table
XORs instead of 128 shift-and-xor rounds. Two honest caveats about that:

- **The bit order is where hand-written GCM ports die.** SP 800-38D numbers a block's bits from the
  left with the *leftmost* bit as the constant term, so byte `p` carries exponents `8p…8p+7` running
  from its most significant bit *down* to its least significant one. Building the tables from the
  bottom bit of each byte upward walks the products out of the byte they belong in — four of eight
  entries land wrong, every tag fails, and nothing tells you which multiply is off. The build chains
  from `0x80` instead; `Context::selfCheck()` exists because that mistake was made here.
- **`Context::selfCheck()` re-derives `H` and all 512 table entries** with the slow reference
  multiply and compares. It costs a few milliseconds, so `Transport::setMediaKey()` runs it at
  install time and refuses to seal with a context that fails. That is the conformance trick for
  hand-written field arithmetic: the fast path is checked against the dumb path, on the real key,
  before any media moves. Measured after this change, x86 host at `-O2`: **122 µs per seal+open pair
  of a 1083-byte fragment, ~17 MB/s single threaded** (was ~400 µs per pair, ~5 MB/s). Always bench
  with `-O2`; the same binary at `-O0` is ~542 µs per pair and tells you nothing.
- **Measured on the shipping ABI** (Quest 3, arm64, the self-test's bench with the real room key):
  **560 µs per seal+open pair of a 1083-byte fragment, ~3.7 MB/s of payload processed, 2000 of 2000
  opened.** That is 4.6× the x86 figure above; the reason is not measured, and the honest reading is
  that the host number was never the shipping number. What it costs: one 1083-byte seal is ≈280 µs, so
  at the 4 Mbit/s the M1 budgets assume (§5) — ≈462 fragments/s — sealing is **≈13 % of one core on the
  host, and the same again for opening on a guest**. Tolerable, not free, and it will compete with an
  encoder that does not exist yet. This does change the ARMv8 note below from "unverifiable here" to
  "testable": a device is in the loop now, so an `AESE`/`PMULL` path can be measured where it runs.
  Bench it against 560 µs and keep `selfCheck()` in front of it.
- The table *indices* are ciphertext and header bytes, which are public on the wire, so the lookups
  are not reading secret-dependent addresses. What is **not** claimable is constant-time-ness overall:
  the AES S-box lookups are key-indexed and cache-soft. ARMv8 carry-less-multiply intrinsics
  (`AESE`/`PMULL`) would close both gaps and were deliberately **not** shipped: the development host
  is `x86_64`, so an intrinsics path could be compile-checked but never executed here, and shipping
  unverifiable crypto is worse than slow verified crypto. If you add them, profile them on a headset.

The transport applies the envelope at the one place that knows the sequence number:
`Transport::sendEnclosed` seals *after* allocating the datagram's `SEQ` (the nonce needs it), and
`Transport::openEnvelope` authenticates and strips the envelope on the receive thread before anything
is queued. The context is installed before the receive thread exists and is never replaced while it
runs, so the GCM calls need no lock; `close()` clears it only after the thread is joined. The policy
is fail-closed:

- `setMediaKey(hex)` before `join()` → outbound frames carry `0x01`, inbound `0x00` frames are refused.
  A downgrade attempt is not accommodated, because the version gate already means every participant was
  handed the key. Calling `setMediaKey` twice, or after `join()`, is refused: the counters on that
  instance belong to the key that is already installed.
- no key configured → `join()` **refuses**, unless `enablePlaintextMode()` was called explicitly first.
  Both a key and plaintext mode is also a refusal. A plaintext dev room still works, but only when the
  caller said so out loud, and it logs that payloads are unencrypted *and* unauthenticated;
- anything failing authentication increments `Transport::cryptoDrops()` and never reaches a consumer.

`Transport::maxAppPayloadBytes()` is the number to size application payloads against: sealing costs
17 bytes per datagram, so the single-datagram plaintext budget is 1083 sealed versus 1099 plaintext.
The media fragment budget (`Wire::kMaxFragmentBytes` = 1075) is unchanged either way, which is what
keeps the uniform-chunk rule in the README true for both modes.

## 13. Failure and reconnect matrix

| Event | Detection | Client action |
|---|---|---|
| kicked | `KICKED <code> <peerId> <reason>` | stop TX, explain, lobby |
| kicked for `seq_regression` | same, with that reason | **your client reset a sequence counter.** Treat the room key as suspect, tear the session down, and start a new room |
| room closed | `CLOSED <code> <reason>` | stop TX, teardown secrets |
| room closed for `host_seq_regression` | same, with that reason | the host's counters restarted, so the room's key is the thing at risk; nobody can rejoin it |
| peer left | roster `R` change | drop their overlay, their voice speaker, and their input state (release it) |
| idle expiry (host) | host `HELLO` reopens the room | guests re-`HELLO` with their existing `sessionSecret` — no new HTTP session needed, **and no counter reset** |
| relay restart | send fails / no `PONG` | retry `HELLO` with the same credentials; sessions survive in Postgres |
| `HELLO` says `revoked` | DENY reason | credentials are dead; rejoin from the lobby with a **new HTTP session** |
| host lost its ticket | `POST /rooms/:code/sessions` → `409 host_session_exists` | host a **new** room. There is no way to re-mint a host identity, and that is deliberate |
| degraded path | own loss/RTT stats (kind 2) | host lowers bitrate, then resolution, then frame rate — always at the encoder |
| input silence | no `I` from a peer for ~500 ms | release all input for that peer |
| pose silence | no `P` from a peer for ~200 ms | hold their last pose and mark it tracked-lost; do not remove the avatar |
| `SEQ` near limit | counter threshold | end the session, `close()` the object, and create a **new `Transport` + new session** for a fresh key. Never rewind |

`PING` about every 5 s, unconditionally: it refreshes both participant and room activity, so a quiet
guest is unbound as idle without it.

`Coop::Transport` implements the detection half of that matrix today: `HELLO` is retried every 2 s
until `WELCOME` or `DENY`, `PING` goes out every 5 s once joined, and 15 s without any answer marks the
room closed (`statusText()` then reads `"liveness lost"`). The **reconnect** half is not implemented:
on liveness loss the transport stops sending and waits. It does *not* wait for you to call `join()`
again on the same object, because that call is now refused — `join()` is one-shot per `Transport`, and
`close()` deliberately does not undo it. What the caller must do is build a **new `Transport`** and
fetch a **new control-plane session**, which is the same thing as "same credentials, counters back to
zero" being forbidden. Until the game layer owns that, liveness loss is terminal for the room. The
"retry `HELLO` with the same credentials" row above therefore describes the protocol, not this class:
what survives is the *session*, not the object. `KICKED`/`CLOSED`/`DENY` are recognised, recorded in
`statusText()`, and pushed as events for the UI to explain.

## 14. Build, libraries and permissions

Verified against this tree (2026-10-23), because the planning notes were vague on all of
this:

- **Where new objects go**: the OBJS list in `backends/platform/android/module.mk` — the
  VR entries sit around lines 31–33 (`platform/android-vr/openxr-session.o`,
  `vr-screen-geometry.o`). `backends/platform/android-vr/module.mk` contains **no** OBJS
  list (it only explains that APK targets live in `android-vr.mk`), so adding files under
  that directory without touching the android module.mk compiles nothing.
- **How it reaches the headset**: `make libscummvm.so` from normal configure/make →
  `android-vr.mk:38–40` installs it into `android_project_vr/lib/$(ABI)/` → gradle packages
  it via `jniLibs.srcDirs 'lib/'` (`dists/android-vr/build.gradle:58`). So native
  dependencies belong in the **ScummVM build**, not in gradle.
- **Opus is not in this fork at all** — no `opus` entry in `configure`, no bundled sources.
  Add it the way ENet is handled: vendored sources plus an `add_component` line in
  `configure` (ENet's is `configure:341`, `USE_ENET`) and a `module.mk` OBJS list; or ship a
  prebuilt `libopus.so` in `lib/arm64-v8a/`. Single ABI —
  `abiFilters 'arm64-v8a'` (`build.gradle:31`) — so no multi-ABI matrix to babysit.
- **AAudio needs no fallback**: `minSdkVersion 29`, `targetSdkVersion 32`,
  `ndkVersion 23.2.8568313` (`build.gradle:19–27`), and AAudio is API 28+. Link `-laaudio`.
  (**Built differently**: `libscummvm.so` targets android21, where `libaaudio.so` is not on the link
  path, so `backends/platform/android/aaudio-output.cpp` `dlopen`s it at runtime instead — the VR
  runtime (API 29+) always has it. A missing library leaves the unit inert.)
- **The microphone needs two things and only one exists today**:
  `<uses-permission android:name="android.permission.RECORD_AUDIO" />` in
  `dists/android-vr/AndroidManifest.xml` (`INTERNET` is already line 9), **plus** a runtime
  grant. With `targetSdk 32` a dangerous permission must be requested at runtime, and
  `backends/platform/android/org/scummvm/scummvm/ScummVMVR.java` contains no
  `requestPermissions` call at all — without adding one, the mic hands you silence and no
  error worth debugging.
- **Do not add a native TLS stack.** TLS already lives in the Java layer (§2), and the relay serves
  HTTPS itself when `TLS_*_FILE` is set, so a LAN test needs no proxy. Production should still put a
  proxy with a public certificate in front of the control plane.
- **Already wired (2026-10-24)**: `coop-wire.o`, `coop-seal.o`, `coop-transport.o`, `coop-session.o`
  and `coop-selftest.o` are entries in `VR_OBJS`, inside the `ifdef USE_OPENXR` block of
  `backends/platform/android/module.mk`. `coop-session.cpp` compiles with or without `USE_HTTP` —
  without it, the calls report that the relay is unreachable instead of failing to link. Sealing pulls
  in no library, so nothing in the seal path needs a new link flag or a gradle change.
- **`common/forbidden.h` blocks the standard headers this layer needs.** `<atomic>`, `<chrono>` and
  `<thread>` pull in `<time.h>`/`<stdio.h>`, whose declarations collide with the forbidden-symbol
  macros. That is why `coop-transport.cpp` and `coop-session.cpp` open with
  `#define FORBIDDEN_SYMBOL_ALLOW_ALL` before `common/scummsys.h`, exactly as `openxr-session.cpp`
  does. `coop-wire.cpp` and `coop-seal.cpp` deliberately do **not**: each needs only `<cstring>`, so
  they stay inside the check and compile standalone.
- **No `snprintf` in the wire path.** Header lines are formatted by a small hand-written buffer writer
  (`Writer` in `coop-wire.cpp`): it runs once per fragment, the decimal conversion is a handful of
  instructions, and overflow is detected per field rather than by re-reading a return value.
- **`configure` regenerates `config.mk`, and two build-critical things are not in `configure`.**
  The OpenXR include path and the OpenXR/GLES link flags are a manual append to `config.mk`
  (`README-ScummVR.md`), so any configure run wipes them and the next build fails at
  `openxr/openxr.h` not found — re-apply that block after every configure. And the configure
  invocation must carry the cross-compiled library prefix **and** the format flags; this fork's
  deployed feature set comes from exactly:

  ```bash
  CXXFLAGS="-I$HOME/android-libs/include" LDFLAGS="-L$HOME/android-libs/lib" \
  LIBS="-lvorbisfile -lvorbis -logg" \
  ./configure --host=android-arm64-v8a --enable-openxr --enable-vorbis --enable-flac \
    --disable-all-engines --enable-engine=scumm
  ```

  Drop the env prefix and FLAC passes its configure *check* but fails at `FLAC/export.h` at
  compile time; drop `--enable-flac` and the feature silently disappears from `config.mk`. The
  video path needs `-lmediandk`; that one *was* fixable properly, so `configure`'s android branch
  now links it (with `-llog -landroid …`) and config.mk no longer needs a manual NDK-library line
  for it.
- **Verifying without a headset.** Two layers. Syntax/unit checks on the host:
  `clang++ -std=c++11 -Wall -Wextra -DHAVE_CONFIG_H -I .` against the tree's generated `config.h`,
  plus the harnesses below. Then the real cross build, which is the one that matters for shipping:

  ```bash
  export ANDROID_SDK_ROOT=$HOME/android-sdk
  export ANDROID_NDK_ROOT=$HOME/android-sdk/ndk/23.2.8568313
  make libscummvm.so                                   # links all five coop units
  cp libscummvm.so android_project_vr/lib/arm64-v8a/   # android-vr.mk does this for you
  (cd android_project_vr && ./gradlew assembleDebug)
  ```

  A single unit can be checked faster, without the whole make:

  ```bash
  $ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang++ \
    --target=aarch64-linux-android29 -fPIC -O2 -std=c++11 -Wall -Wextra \
    -DHAVE_CONFIG_H -I . -c backends/platform/android-vr/coop-seal.cpp -o /dev/null
  ```

  Do not skip that step for "portable" code. Linking found `Session::pending()` and six sibling
  accessors declared but never defined — a host harness that only calls `readResponse()` cannot see
  it, and `-Wall` cannot either.

  Beyond the cross build, five host harnesses exercise the same sources. They live outside both repos
  (they are scaffolding, not shipped code, and whether they belong in the tree is undecided), and each
  is a small binary compiled straight from the fork's sources:

  | Harness | Links | Proves |
  |---|---|---|
  | wire check | `coop-wire.cpp` + `<cstring>` | budgets, header formatting, `SEQ`, fragmentation and reassembly edge cases, the `P` pose slot and its per-type counter independence, and the constants it compiles to (58 checks) |
  | byte-parity check | wire check output vs `coop-relay/src/protocol.ts` | C++ header lines are byte-identical to `headerBytes()`; the relay's parser accepts every C++ datagram; frame-type set, protocol version and the `SEQ` ceiling agree in both directions (25 checks) |
  | seal check | `coop-seal.cpp` + `coop-wire.cpp` | `Context::selfCheck()` passes on the real key, then the vendored AES-256-GCM matches the relay's `sealPayload` for 12 vectors (303 assertions); a context with no key, or one cleared after `close()`, refuses to seal (303 assertions) |
  | seal reverse-check | seal check output + Node crypto | the relay's `openPayload` opens every C++ ciphertext, and rejects every byte-flipped one (105 checks) |
  | transport probe | `coop-transport.cpp` + `coop-seal.cpp` + `coop-wire.cpp` + `debug()`/`warning()`/`error()` stubs | two processes against a live relay: `HELLO`/`WELCOME`, keepalive, `X` both ways, sealed and plaintext 4 KB video byte-for-byte, guest-role refusals, `DENY`, the wrong-key guest that authenticates nothing. Flags: `--media-key`, `--expect-no-video` (proves video is not in the clear), `--send-text`, `--join-twice` (second `join()` refused), `--no-plaintext-optin` (`join()` refused with neither key nor opt-in) |
  | session parse check | `coop-session.cpp` + ScummVM's JSON/str units + host stubs | the real relay JSON maps into `Participant`/`OwnerCredentials` field-for-field, `maxSeq` survives the trip without being truncated to `int`, and the gate refuses a wrong version, wrong caps, a missing or mismatched `maxSeq`, and a malformed response (30 checks) |

  `coop-session.cpp`'s parser needs `common/formats/json.cpp`, `common/hashmap.cpp`,
  `common/str*.cpp`, `common/memorypool.cpp`, `common/mutex.cpp` and an `OSystem *g_system` stub, linked
  with `-ffunction-sections -Wl,-dead_strip` so the unused networking calls fall away.

  That is what caught the two `sendMedia`/reassembler bugs, what proves the header bytes match the
  relay's, and what pins the sealing port to OpenSSL's output. **None of it runs on the shipping
  ABI**: the cross build and the APK are proven, the device run is not.

### Running it on a headset

There is no UI entry point, deliberately: the first device test is a probe, not a feature. The
marker file is the switch.

```bash
# once per install: the app reads /sdcard with the app-op its manifest documents
adb shell appops set org.scummvm.scummvm.vr.debug MANAGE_EXTERNAL_STORAGE allow

adb install -r android_project_vr/build/outputs/apk/debug/ScummVM-debug.apk
adb push coop-relay/scripts/coop-test.example.txt /sdcard/scummvm-coop-test.txt   # edit relay= first
adb logcat | grep -i coop
```

Reading the output has two traps. `debug()`/`warning()` reach logcat under the tag **`ScummVM`**, not
the unit's name, so `grep -i coop` on logcat only matches the message text; and the same text goes to
`files/scummvm.log`, which outlives a crash — from a debug build that file is the better source:

```bash
adb shell "run-as org.scummvm.scummvm.vr.debug cat /data/data/org.scummvm.scummvm.vr.debug/files/scummvm.log"
# truncate: nested quoting, and run-as does not resolve a relative path
adb shell "run-as org.scummvm.scummvm.vr.debug sh -c 'cat /dev/null > /data/data/org.scummvm.scummvm.vr.debug/files/scummvm.log'"
```

A native abort leaves a tombstone: `adb logcat -d -b crash`, where `Abort message:` is the line that
names the JNI problem and the first `#NN pc` frames name the C++ function. And a VR-only activity will
not start while the headset is unworn — `am start` waits until `VolumetricWindowPlacementRequest` times
out — so every run needs someone to put the thing on.

Start the relay with TLS material for the address the headset will dial (`coop-relay/README.md` has
the two-command version). The self-test sleeps 5 s so the backend and the Java HTTP layer are up, then
reports, in order: whether the control-plane request landed at all (a TLS or cleartext failure looks
like a 15 s timeout, and the Java layer logs the reason), whether the version gate accepted the relay,
the room code / `peerId` / relay address / `maxSeq` / encryption scheme it read back, the room code and
**invite to type into the second headset's marker file**, seal cost in µs per seal+open pair with the
real room key, `WELCOME` latency, then one `X` kind 7 and one 4 KB probe per second with every inbound
frame, roster push and control line reported, ending with the counter summary
(`droppedEvents` / `badPackets` / `cryptoDrops` / `HELLO written` / `datagrams in` — the same line is
printed, with the same counters, on the no-`WELCOME` path).

Three `video` marker keys drive the codec path without inventing a second entry point:

- **`video = bench`** runs the encoder only — synthetic 640×480@30 content (blocky edges plus one
  moving region, because a static scene flatters every rate controller), for up to 10 s, no room and
  no credentials. It reports submit-to-AU p50/p95/max, achieved bitrate, observed IDR interval, and
  encoder input drops. It deliberately runs **before** the control plane: the encoder cost is M1's
  gate item and a broken network must not hide it.
- **`video = send`** (host role only — the relay refuses a guest's `V` frames) encodes while joined
  and puts every access unit on the wire as a real `'V'` media frame at 30 fps, replacing the 4 KB
  probe. Any frame larger than 64 fragments is dropped whole and counted, because a partial frame
  poisons the guest's decoder. This is the host half of the same proof the §14 guest run made from
  the other direction.
- **`video = recv`** (guest role only) opens the decoder and feeds every `'V'` fragment the room
  sends through the reassembler and the decoder, reporting fragments in, AUs complete, frames decoded,
  the codec's published output format, and the AU-complete-to-decoded percentiles. The self-test
  thread itself has no render context; with **`display = true`** on top, decoded frames are also
  handed to the VR thread through a single-slot mailbox and the virtual screen quad shows them, and
  the run gains a second report line — `guest render:` frames actually uploaded to the quad, and
  the decode-callback-to-upload delay. The other half of the room can be a second headset or the Mac
  harness's `--h264-file` mode.

Keeping that failure-mode honesty in mind, here is what the device has actually proven so far
(2026-10-23, Quest 3 / Android 14 / arm64, one headset, LAN). Host-verified numbers stay in §12;
this is the run-on-device column:

- **Control plane over TLS works end to end on the headset.** The gate accepted the relay, the room
  was created, and the self-test logged `peerId`, protocol v3, the seq ceiling
  (`9007199254740991`), `aes-256-gcm`, and the invite. This resolved the last unverified TLS
  assumption: the LAN CA generated by `scripts/lan-cert.sh` **is** trusted through the fork's
  `HTTPManager → TLSSocketFactory → LETrustManager` chain.
- **arm64 seal bench, real room key, on device:** 560 µs per seal+open pair of a 1083-byte fragment,
  3.7 MB/s, 2000 of 2000 opened (the cost reading is in §12).
- **Host role joins and holds:** `WELCOME after 175 ms`, `sealed=1`, 20 s joined with PING/PONG
  keepalive, 20 `X`-kind-7 frames and 20 4 KB frames out, and a clean summary — `bad packets 0,
  crypto drops 0, dropped events 0`. The 175 ms is dominated by Wi-Fi power save: LAN ping RTT to the
  headset measured 6–320 ms during the same session. It is not a protocol cost.
- **A Quest hosting real H.264 to a real receiver (`video = send`, 2026-10-23).** The device encoded
  and sent for 40 s while the Mac joined as guest over the TLS relay's data plane: host side `1200
  frames fed, 1200 AUs out (27 key, max 18373 B), 3.97 Mbit/s, 0 send refusals, 0 oversized drops`;
  guest side `10508 fragments → 657 complete Annex-B frames (14 key), 0 bad, 0 crypto drops`, and the
  harness verified each reassembly is Annex-B-framed with a known NAL type before printing `PROBE OK`
  (a length check cannot validate produced video — every AU is a different size). One honest delta:
  under send load the encoder stage measured **p50 37.0 ms but p95 73.2 ms**, versus 35.4/37.8 in the
  dedicated bench — the feed loop shares its thread with the room loop, and roughly one frame in ten
  slips by a full frame interval. A production capture path must not share a thread that can stall it;
  the gate needs the loaded number, not the solo one. And the trap around this run was my own: the
  first guest attempt dialed `127.0.0.1:19091` and heard nothing back, because the TLS relay's UDP is
  bound to the advertised address (`UDP_BIND`) and a connected socket discards the differently-sourced
  reply — exactly the failure §14 documents for the headset. A harness must dial what the session
  response advertises.
- **A Quest decoding real H.264 from a real host (`video = recv`, 2026-10-23).** The mirror of the
  run above: the device joined as guest while the Mac streamed an actual 640×480@30 H.264 stream —
  produced by a `swiftc`/VideoToolbox fixture generator (`mk_h264`, same content recipe as the bench:
  blocky base, grain, moving bar; 300 AUs, IDR every 45 frames, `AllowTemporalCompression=false` so
  there are no B-frames) and sent by the extended harness (`coop_probe --h264-file`, paced at 30 fps,
  `960 AUs sent … PROBE OK`). Device side: `10458 fragments in, 950 AUs complete, 949 frames decoded`
  (the one missing frame is the last AU, still in the codec when the room closed — a 1 s drain cannot
  flush a stream that ends at the same moment), output `640 x 480 (stride 640 slice 480, color 21)` —
  NV12, unpadded, read back from the codec rather than assumed. Guest decode stage, AU-complete to
  decoded callback: **p50 23.7 ms, p95 116.6 ms, max 310 ms**. Two lessons rode along. The c2.*
  decoder family **rejects a mime-only configure outright (`-10000`)** — width and height must be in
  the configure format, unlike the encoder half — so `VideoDecoder::open` takes the expected
  dimensions while the first frame's format change remains the actual description. And the
  poll-cadence trap from the encoder bench applies on the receive side too: the first run observed
  decoder outputs only at feed ticks and reported p50 63.5 ms; letting the room loop's idle sleep be
  the decoder drain instead measures 23.7 ms. The remaining p95 tail is Wi-Fi burstiness, and it is
  the jitter buffer's problem to absorb, not the codec's.
- **Quest → relay → Quest, two headsets in one room (`video = send` + `video = recv`, 2026-10-23).**
  The first end-to-end host-to-guest run with no laptop in the game: a Quest 3 created room `VBAR4`
  and sent 1800/1800 AUs at 3.98 Mbit/s with 0 refusals, and a **Quest 2** (Android 14, Wi-Fi 5 —
  a second device family) joined as peer 2 and reported `20973 fragments in, 1273 AUs complete, 1271
  frames decoded (640 x 480, color 21, stride 640 slice 480)`. The decode stage on the Quest 2 measured
  **AU-complete-to-decoded p50 2.5 ms, p95 3.4 ms, max 125 ms** — an order of magnitude better than
  the Mac-hosted measurement above, which says that run's p50 was the paced-file feed, not the Quest
  decoder. Same binary, same code path, no per-vendor codec branch: the c2.* configure and output
  findings hold unchanged on the older family. Three operational facts came with it: a Quest 2 refuses
  to launch a VR activity while its controllers are off or unpaired (the system's
  `LaunchCheckControllerRequiredDialog` — a blocked `am start` produces no process, not a slow one);
  the self-test marker's real schema is `role`/`relay`/`code`/`invite` at
  `/sdcard/scummvm-coop-test.txt`, and a marker with unknown keys is refused before the thread even
  starts; and with one physical headset to share, the guest must be `am start`-ed within seconds of
  the room code printing, because the 60 s window runs from room creation while the guest app needs
  ~35 s to boot.
- **The host captured its own game framebuffer (`capture = true`, `game =` autolaunch,
  2026-10-23).** First run with real content end to end: DOTT autolaunched on the Quest 3 by
  the marker's `game = tentacle` key (a target that app already has registered — and the
  graphics manager now *remembers* the game's video mode from `loadVideoMode`, which is what
  tells the capture which region of the backbuffer holds the game instead of guessing). The
  capture unit (`coop-capture.{h,cpp}`) reads that region back asynchronously: two PBO slots,
  a zero-timeout fence poll — the graphics thread is never blocked on the GPU — and a fixed
  640×480 capture FBO, which for a 640×480 game is a 1:1 copy and for any other mode is one
  GL scale. The sending thread converts RGBA→NV12 (integer BT.601 limited range, the same
  matrix the guest's shader uses, so packer and unpacker describe one picture; the GL
  bottom-first orientation is handled inside the conversion walk for free). Fixed run:
  **`555 readbacks issued, 554 collected, 0 not-ready, 0 map failures`**, and the Quest 2 took
  all of it: 195 AUs complete, 195 decoded, **195 uploaded** — the first 100% render ratio
  (counters; no one was wearing the headset during this run, so seeing DOTT on the guest quad
  is the next thing to do, not the last).
  Two bugs were caught by the counters before any pixel could have complained, both worth
  knowing: the first run reported `2 collected, 574 map failures` because `glUnmapBuffer`
  called after its buffer was unbound is an invalid operation that **leaves the PBO mapped**,
  and after two maps this driver refused every one — unmap while bound, and they all succeed;
  and a consumer that does not track the mailbox seq will happily "capture" one frame
  thousands of times (the first report claimed 1650 fed from 2 collected). What only *real*
  content could say: **max AU 86519 B**, 4.7x anything the synthetic scene produced — a real
  game IDR is a much bigger object than a synthetic one — and the sender's 64-fragment guard
  fired on exactly one of them: `1 AUs dropped for exceeding 64 fragments`. The largest real
  keyframe does not fit the M1 frame budget (68800 B at 1075 B of NAL per fragment), and a
  dropped keyframe is a hole in the guest's GOP, so this is a design item, not a counter
  curiosity. The real scene also moved
  the encoder's measured latency: **submit-to-AU p50 201 ms**, six times the synthetic bench,
  for the same codec and settings — that is the 1-frame pipeline's flush cost at the game's
  own redraw cadence (SCUMM titles repaint at ~10-12 fps; each AU only leaves when the *next*
  frame walks in), pacing rather than codec, and it is now the dominant term against the M1
  gate. Guest-side per-frame costs stayed exactly where the two-headset run put them (decode
  p50 2.5 ms, render hop p50 11.4 ms).
- **The guest's screen quad showed received video (`video = recv` + `display = true`,
  2026-10-23).** The decoder's callback now hands NV12 frames to the VR render thread through a
  single-slot mailbox (`coop-video-display.h`): the VR thread uploads them as two textures (Y as
  `GL_R8`, interleaved UV as `GL_RG8`, with `UNPACK_ROW_LENGTH` carrying the codec's stride — the
  640×480 stream is unpadded, verified rather than assumed) and draws the same quad with a BT.601
  fragment shader. Two back-to-back runs, Mac video host → relay → Quest 2, first device-proven
  render of received video: `guest render: 1098 frames uploaded to the screen quad (of 1173
  decoded)` and `1108 (of 1183)` — decode-callback-to-texture-upload **p50 10.9 ms, p95 13.1 ms,
  max 26 ms**, which is the frame waiting for the next vsync (33 ms period) plus ~2 ms, not pipeline
  work. The missing uploads are the run's final-second tail plus the vsync race. And the proof the
  counters cannot give: **the person wearing the headset watched it** — white bar sweeping, chroma
  tint flashes, 30 fps, on the virtual screen. Colour correctness by eye, not just by byte layout.
  Two harness traps rode along: `coop_probe --count-video` validates frames the probe *receives*, so
  passing it on the host side made it exit early at 90 AUs (a host validates with `--seconds` pacing
  instead); and a room's per-peer SEQ high-water spans the whole room life, so a second probe joining
  as the same `--peer` of the same room is replay-kicked (`KICKED … seq_regression`) — one probe
  attempt per room, new room on a retry.
- **The sealed media path is proven at both ends — with one headset.** The Mac ran the host harness
  (`coop_probe --role h --video 4000 --repeat-ms 1000 --media-key …`) against the same UDP port
  while the Quest ran the self-test as **guest**. The device joined as peer 2 and received the
  reassembled fragments of the host's 4 KB video — `V` fragments `1083/1083/1083/783`, the expected
  sealed form — plus `X` frames from peer 1: `205 frames in (195 data, 1 roster, 9 control), 0 bad,
  0 crypto drops`. The Mac in turn received the guest's `M` fragments. That is a sealed frame crossing
  a real relay in both directions, one end a shipping-ABI headset.
- **Host encoder stage measured on device (`video = bench`, 2026-10-23, same Quest).**
  `c2.qti.avc.encoder` at 640×480@30, 4 Mbit/s, IDR 1500 ms, grain-loaded synthetic scene:
  **submit-to-AU p50 35.4 ms, p95 37.8 ms, max 39.4 ms**, 300/300 AUs out, 0 input drops, achieved
  **3.89 Mbit/s** (the cap is honored), largest AU 18249 B ≈ 27 fragments (well inside the 64 limit),
  IDRs every ~1500 ms exactly as asked. The codec claims 1 frame of latency and measures that way:
  ~33.3 ms of pts pacing plus 2–6 ms of actual work. Two earlier readings of exactly ~67 ms were
  **instrumentation**, not hardware — the first bench only looked for outputs at submit ticks, and a
  frame completing 2 ms after a tick was then reported 31 ms late. Anyone repeating this measurement
  must poll during the frame gap, not just at the tick. Against §5 this puts the encode stage at
  ~35 ms rather than the budgeted 8–25: still workable, but the jitter buffer gets correspondingly
  less room, and this is the number the M1 gate runs on. The Quest codec also refuses byte-buffer
  input unless the format is **requested at configure** (left alone it answers a vendor format and a
  buffer smaller than the computed frame); NV12 requested explicitly settles at stride 640/slice 480,
  unpadded. The `latency`/`low-latency.enable` format keys were accepted but changed nothing.

Not yet proven, in order of what is missing: everything gated on code that does not exist
yet — mixer tap, `RECORD_AUDIO`, pose publisher, A/V sync. The capture path and the guest render
half are no longer on this list: a real game scene (DOTT) has been captured on the host and
watched its counters land on the guest (above) — visual confirmation of the game content itself
on the guest's quad is the one thing the counters cannot substitute, pending the next run with a
wearer present.
The **`video = send` wire path**
has been proven in the host direction (Quest → relay → Mac guest, above), and the guest's
decode half has been proven in the reverse direction (Mac host → relay → Quest decoder) **and**
host-to-guest between two real headsets (Quest 3 → relay → Quest 2, above).

"No WELCOME" has **three** causes, and they used to be indistinguishable from the game side. They are
not any more — the transport counts `HELLO` writes and inbound datagrams, and the relay now logs every
data-plane handshake (`participant bound`, `HELLO refused`). Read them together:

| symptom (client summary) | meaning |
|---|---|
| `HELLO written 0` | `join()` never wrote; look above it in the log |
| `HELLO written N, datagrams in 0` | the datagram left, nothing came back: `RELAY_HOST` wrong, firewall, or — see below — the reply carried a source address this socket discards. The relay's own log separates "never arrived" (nothing logged) from "answered" (`participant bound` repeated) |
| `datagrams in N, unparseable N` | the relay answered and this build could not read it |

The third one is a LAN-host trap, and it bit on 2026-10-23. A development Mac had **two interfaces on
the same subnet** (`en0` .30, `en6` .25); the relay was bound to `0.0.0.0` and advertised `.30`. Its
replies left with a source address chosen per route — sometimes `.25` — and the client's **connected**
UDP socket (connected deliberately: only the address it dialled may feed the parser, because `HELLO`
carries the session secret) had those datagrams discarded by its kernel. The client's log was truthful
and useless: `datagrams in 0` — not even a bad packet. The relay's log showed five clean binds. Both
ends healthy, each blind to the other. The fix is operational, not client-side: start the relay with
`UDP_BIND=<RELAY_HOST>` so the advertised address and the reply's source are the same one (the relay
now warns at startup when its host has that shape). Do **not** "fix" it by making the client's socket
unbound — that reopens the parser to unsolicited sources.

**Bidirectional cursor regression, 2026-10-29: binding an IP is not proof that its interface
matches the route.** With `UDP_BIND=PUBLIC_HOST=192.168.0.20` (Mac Wi-Fi, `en0`),
`route -n get 192.168.0.55` and `route -n get 192.168.0.32` both selected `en6`
(Ethernet, `192.168.0.25`). Enabling guest → host cursor traffic reproducibly devastated
guest video/audio. This was real, not an acceptable M2/performance tradeoff:

| Same installed build; 75 s guest runs | Guest frames displayed | Audio frames received |
|---|---:|---:|
| Cursors off (`VWDQH`) | 1861 | 4063 |
| Host → guest only (`RPXJK`) | 1838 | 4036 |
| Both directions (`MSASW`) | 135 | 1827 |
| Off again (`43YWK`) | 1932 | 3987 |
| Guest → host only (`777T8`) | 250 | 1932 |

Isolation: host cursor drawing disabled at 30 Hz still failed (`VXHCC`: 92 frames);
1 Hz sending recovered (`82XRV`: 1873 frames). Kernel `SO_RXQ_OVFL` reports and the app
event queue both showed zero drops; maximum guest packet-handler time was about 3 ms.
External UDP traffic from a separate process did not reproduce the severe collapse.
Crucially, temporarily sinking cursor packets at the relay while the guest continued
sending at 30 Hz recovered normal media (`R7XEK`: 1918 video / 4078 audio). Host audio
ingress at the relay was nearly intact even in a failing run (`CHCWS`: 4800 packets
across 4833 sequence numbers). Do not infer a Quest 2 sender bug from the initial A/B alone.

**Operational correction:** keep HTTPS at `https://192.168.0.20:19091` (the existing TLS
SAN), but start UDP with `UDP_BIND=192.168.0.25 PUBLIC_HOST=192.168.0.25`. The control-plane
ticket advertises the UDP endpoint separately; the marker's HTTPS URL need not change.
Use the current interface addresses and inspect routes again after network changes.

Device verification: Ethernet-bound reverse-only run `53YQ2` recovered 1939 video / 4063
audio frames. Full bidirectional, drawing-enabled `XKBCT` then recovered **1938 displayed
frames / 4072 received and decoded audio frames in 75 s**, 7 audio gaps recovered by FEC,
4 PLC frames, zero inserted silence. Host received all 2249 guest cursor frames; guest
stored 2248 host cursor frames. Both completed their actual durations with zero socket,
event-queue, parse, or crypto drops. This final build restored the original **PONG-based
15 s timeout**: the earlier any-datagram liveness workaround masked disconnects but did
not cure media loss, and was removed. Selftest summaries now report elapsed milliseconds
separately from configured duration.

This establishes a tested interface/configuration remedy, **not a packet-capture proof of
the precise OS/AP drop mechanism**. User visual/audio acceptance of the corrected setup
is still pending. The prior claim that only small datagrams were being lost was not proven.
Proximity `prox_close` overrides were sent on both headsets before each run. Diagnostic
marker keys `cursor_draw=false` (suppress remote drawing) and `cursor_hz=1..60` (default 30)
support repeating the isolation. Temporary relay sink/ingress probes were removed.
Evidence directories under the OpenCode temp directory: `cursor-controlled-222732`,
`cursor-isolate-224751`, `cursor-isolate-230034`, `cursor-isolate-230406`,
`cursor-isolate-230939`, and `cursor-isolate-231410`.

One wire ordering worth knowing, from the relay's own log on the first successful handshake: a
successful `HELLO` produces **two** datagrams, and the roster push arrives **before** `WELCOME`. A
client that treats "the first datagram after HELLO" as the answer would be wrong; the transport matches
control lines by content, so it does not care.

And the one *expected* silent anomaly stays expected: a nonzero `cryptoDrops` is only surprising when
no other participant is in the room — one room key means any participant can produce frames every
other one accepts.
- **Polishing real-game video: what a wearer's complaints actually traced to (Quest 3 → Quest 2,
  2026-10-23).** The first runs with a person watching DOTT on the guest produced quality complaints,
  and every one of them resolved to a specific bug rather than to "network video looks like that".
  The method that worked was a **frame probe**: the capture path can write its current frame as a
  BMP into the app files dir (`coop-frame-N.bmp`, pulled over `adb exec-out run-as … cat`). Three
  findings, in order of severity:
  1. **The capture was reading the wrong rectangle.** The old assumption — the game bitmap sits at
     the origin of the backbuffer, so read 640×480 from (0,0) — was false: the VR backend's scene
     buffer is 1920×1080 and the composed game sits inside it wherever the VR layout put it. The
     probe BMP showed the actual garbage directly; the fix is to capture the whole composed
     backbuffer and scale it down with one `GL_LINEAR` blit (the user decided the shader question
     was moot: capture what the host wearer sees, not pre-shader pixels). A `_coopGameWidth/Height`
     plumbing shortcut that this discovery invalidated was removed.
  2. **Chroma was packed at the wrong unit.** The integer BT.601 converter shifted chroma sums by
     `>>10` where the fixed-point recipe wants `>>8`: correct matrix, values at 1/4 amplitude —
     washed, hue-shifted colour that counters could never see. Same class of bug landed on the
     guest side: the UV upload set `UNPACK_ROW_LENGTH` to the stride in **bytes** where texel units
     are needed — an NV12 UV row is stride/2 `RG8` texels — so the upload read past the buffer end
     and the wearer saw "a double image of colors and black and white". Both fixed; the probe BMP
     and the wearer's description were the evidence for each.
  3. **Loss, which is the real hard part.** Counters, in order of what they caught: a real DOTT
     IDR measures **144304 B = 135 fragments**, so the 64-fragment frame budget dropped whole
     keyframes — 98 of the first 100 real IDRs never went out. The budget is now **256 fragments**
     (275 KB) across reassembler, transport pre-check, and sender. Then measured end-to-end
     delivery: a 240 s run reported `frameIds through 7403, 6670 AUs complete, 211 whole-frame
     gaps` — ~3% whole-frame loss, concentrated on the big bursts; an earlier count had suggested
     ~45% datagram loss, which was **wrong**: it compared windows of different runs, and reordered
     packets look like gaps. (The guest's arrival counters are deliberately named for what they
     observe — "forward-gap IDs (may reorder)" — not for a conclusion.) Two receive-side bugs sat
     under that number: OS-default UDP socket buffers (now requested at 4 MB/2 MB and **logged at
     their effective values** — Android grants 8388608/4194304), and the transport's own event
     queue at 64, smaller than one keyframe burst, which dropped 325 events in that run while the
     counters upstream stayed clean. It is now `2 * kMaxFragments + 32` and reported zero drops.
- **Paced sending measured clean on a healthy link: the artifact hunt ends (2026-10-23, room
  `MDM8A`).** A full 290 s host run at the complete 30 fps re-feed cadence (**8700/8700 AUs,
  194 keyframes, max 144302 B** — denser IDR traffic than any earlier run) with 250 µs fragment
  pacing, then 240 s of it watched by a person: **`frameIds through 8700, 943 AUs complete,
  1 forward-gap ID` — and that one gap is the host leaving the room at the end of its timer.
  Zero frames arrived missing fragments; the guest asked for retransmission 5 times, all after
  the host was gone. The wearer's verdict: no artifacts noticed at all.** Two conclusions:
  fragment pacing plus the 256-fragment budget plus the buffer/queue fixes take delivery to
  zero loss on a healthy Wi-Fi path; and the earlier "11-16% loss" readings from `ST5GA`/`ZD8DB`
  were the **Quest 3's flapping link** — its log shows the outbound interface dying (frames
  silently stop, `ENETUNREACH` 10 s later), which is also what ended `29FHX` early. Check the
  host's Wi-Fi health (`ip route`, or its `UDP send failed` lines) before believing any loss
  measurement. The retransmit path (X kind 9, §11) is now built and plumbed on both sides;
  this run proved it stays quiet when it has nothing to fix, and it has still never answered a
  real request on-device — that measurement needs a lossy path or a longer clean run with
  deliberate loss injected.
- **Recovery after loss, and what it costs (2026-10-23).** The guest now refuses to feed the
  decoder across a reference discontinuity: on any gap it holds the last good picture until a
  genuine IDR (Annex-B NAL type 5 scanned from the AU — the wire carries no codec flags) resumes
  the chain, and never feeds a late AU behind a newer one (no B-frames in this stream). The wearer
  described the result exactly as designed: "entirely freezing then catches up" — ghosting became
  correct-but-frozen instead of corrupted motion. The counters separate the two: pre-guard runs
  ghosted through ~45%-of-burst loss; the guarded run reported `19 reference discontinuities,
  1364 AUs skipped, 19 IDR resumes` in 240 s, i.e. recovery is working but gaps remain, so the
  open work is delivery (send pacing, then retransmission), not decode. Fragment pacing (≥250 µs
  apart at the sender) is the first delivery attempt; its measured effect is the next record.
- **A run is only data if its overlap is proven (2026-10-23, repeatedly).** Three orchestrated
  attempts produced unusable comparisons, and each failure taught a different rule.
  1. `am start` on a Quest can queue the activity launch by **minutes** — the process may not
     exist for 3-4 minutes after the command, and `am start -W` ("Status: ok") does not mean it
     ran. Treat a launch as real only when a *new* `--- Log opened` appears in the device log.
  2. A fresh host room line must be matched after the newest `--- Log opened` (count them
     first), or extraction returns a stale code from appended history — and a guest whose
     marker holds a dead room gets `HTTP 410 room_closed` from the control plane, which is the
     relay being correct while the harness is wrong.
  3. Overlap is bounded by the host selftest's joined phase, which starts at the host's own
     `WELCOME` and runs for the marker's `seconds`. The check that works: read the *host log's*
     WELCOME timestamp and compute `end = that time + seconds`; a guest join is only useful if
     `now < end` with meaningful margin. (An earlier note here claimed `PONG <code> <n> <ms>`
     carried seconds-since-join in `<n>` — it does not; that field is a peer count and the
     third is the relay's wall clock. The claim was made from one coincidental reading and
     broke the first time it was used, which is why the check above reads timestamps instead.)
  And a run whose *video* window is short fails even when the room overlap is long: the host's
  feed loop starts only once the capture produces real frames (the `coop-frame-1.bmp` probe
  line is the marker of that), which sits minutes behind room creation when a game is
  autolaunched. Validate against the probe line, not the room open.
- **The latency build crashed the host at the first captured frame (2026-10-29, room `N6K5T`
  and the `RECPD` first attempt).** Both zero-video runs had the same shape: host log stops
  right after the `coop-frame-1.bmp` line, guest times out its relay liveness 15 s later.
  The tombstone said `SIGSEGV @ 0x0` in the selftest thread, and `llvm-symbolizer` against
  the exact APK put frame #00 at the re-feed site: the instrumentation added a per-frame
  `captureAtUs` vector but its `assign()` next to `submitAt`'s never applied, so `operator[]`
  on an empty vector wrote through a null `data()`. Two lessons: a new device-only path is
  untested until a run reaches it, and **symbolize the tombstone before theorizing** —
  `logcat -b crash` plus the NDK symbolizer named the exact source line in one step. Fixed
  in `bc29564b`. The same reading retroactively explains the `2U9WG` host log ending at its
  probe line two nights earlier — that was this crash too, not a flaky launch.
- **Orchestrating around two measurement-blindness bugs (2026-10-29, rooms `KM57X`/`YAYF5`).**
  (a) The device log file is not append-forever: a fresh launch recreated it, so the "count
  `--- Log opened` and wait for more" rule went backwards (6 → 1) and a perfectly healthy host
  session looked like it never started. The robust check is "the *newest* Log-opened stamp
  differs from the one captured before launch", which survives both append and truncate.
  (b) The guest's `uploaded` stamp used `LOGI` (logcat) while every other sample line uses the
  debug channel (`scummvm.log`): the join came up upload-blind unless logcat was also pulled,
  and the logcat ring had already dropped a third of the lines. One log channel for all five
  stamps; `debug()` is thread-safe, the VR thread may call it.
- **The offline join needs the same section discipline as the orchestrator (2026-10-29).**
  Every run restarts frameIds at 0, so parsing the *whole* appended log file silently mixes
  runs: one join "matched" 336 guest samples that were entirely from the previous room, and
  their timestamps disagreed by 634 s — the real wall-clock gap between the runs, misread as
  a clock-estimation failure. The join must (a) parse only the newest `--- Log opened`
  section on each side, (b) require both sections to name the same room code, and (c) refuse
  to report anything if a section contains `liveness lost`. A run is also worth a pre-flight
  on the guest: watch its keepalive for 60 s before the video window starts — `YAYF5` was a
  dud because the Quest 2's uplink flapped ~30 s in (PONGs stop, inbound video keeps
  flowing, liveness dies 15 s later), which is the same link-flap family as the Quest 3's
  `ENETUNREACH` episodes.
- **Two silent launch/package failures on the desk rig (2026-10-29, cursor-hide work).** (a) A
  Quest whose controllers have all gone to sleep refuses adb-launched VR apps: `am start` reports
  success, no process ever appears, and the truth is a `LaunchCheckControllerRequired` dialog from
  vrshell - `logcat | grep LaunchCheck` names it in one line, and nothing over adb wakes a
  sleeping controller (a synthetic `-dsources 2 tap` does not either). A human presses a
  controller button. Verify a launch by the process existing, not by `am start` output.
  (b) Packaging a new `libscummvm.so` into the APK looked fine (`:packageDebug` ran, no
  UP-TO-DATE) but verifying freshness by hash is a trap: the APK stores a *stripped* `.so`, so
  its bytes never match the build output. `nm` for a new symbol (there `preCursorHook`) proves
  presence; better, design the test so the new code produces an artifact the old binary cannot -
  the third capture probe file appearing was the airtight check. And count the right thing when
  gating a timed probe: the selftest's captured-frame counter advances only on game redraws
  (~11/s, on ~a third of encode ticks for SCUMM titles), while the encode tick counter runs at a
  steady 30/s - gate on ticks with `>=` and a done flag. Two probe rounds lost to those two
  mistakes before the 90 s frame landed.
- **One "latency" number is not a pipeline number (2026-10-29, `RECPD`).** The first joined
  run showed capture→sent p50 126 ms / p95 926 ms and looked like a gate failure — but that
  segment silently includes the *re-feed carry*: a re-fed frame honestly carries the capture
  time of the older pixels it duplicates, so its captured→sent time contains the game's
  repaint cadence (~10-12 fps, up to seconds on a static screen), not the pipeline's work.
  The pipeline stages measured in that same run were fast: wire+reassemble p50 16.6 ms,
  decode p50 3.4 ms, sent→uploaded p50 34 ms. The fix is to stamp the encoder-submit time on
  the host line too and report **pixel age** (captured→submitted) and **pipeline**
  (submitted→…→uploaded) as the two different quantities they are. The gate is about the
  second one; the first is a property of the game's redraw rate and the re-feed design.
  (Also learned: with adb moved to USB, the clock estimate's worst half-RTT dropped from
  77-164 ms to 2-37 ms — the previous Wi-Fi adb traffic was competing with the Quests' radio.
  Keep adb on USB for any latency work.)
- **Game audio proved end-to-end with two faults in front of the network (2026-10-29, room
  `6JW2X`).** The first audio run got real sound out of a Quest 2, and then two findings showed
  that neither end had ever been honest about audio. (a) **VR mode had no device-output path at
  all**: the platform backend's `writeAudio` is a stub in VR and `createVR` creates no AudioTrack,
  so a headset had never been able to hear its own mixer. `aaudio-output.{h,cpp}` adds an AAudio
  stream driven from the platform audio thread exactly where the AudioTrack write used to be.
  (b) The relay **kicked the host** mid-run (`seq_regression` on an `'A'` frame): two consecutive
  20 ms audio datagrams were reordered en route, and the old high-water-only rule read any
  regression as a possible AEAD nonce reuse. A regression is not a repeat — the relay now keeps a
  1024-seq sliding window per (peer, type) and faults only a *seen* `(type, seq)` pair or a
  restart far below the window (§13 and the relay README).
- **`mixCallback()` returns sample frames, not bytes — the tap believed it (2026-10-29, rooms
  `F6A9M`/`VWWGV`).** The host tap seam passed the return value through as a byte count, so every
  captured chunk was **one quarter of the mix buffer**, skipping three quarters of the sound. The
  symptom was "garbled and staticy" with the network innocent. The isolation that found it is worth
  keeping: an **audio-only run** (marker `audio = send`/`audio = recv`, no `video` key) reproduced
  the garble with zero video traffic and low loss, which moved the suspicion off the downlink and
  onto the capture. The fix taps the full zero-initialized buffer at `buf_size`, silent tail
  included — the device timeline must survive quiet passages.
- **The host's audio follows the headset's wear state (2026-10-29).** Removing a Quest pauses the
  platform audio thread (`JNI::pause` → `sem_wait`), and with it the tap: the first audio run
  showed 7138 starved ticks and ~28 s of real audio in a 115 s room because the host headset sat
  on the desk. That is normal Android behavior, not a transport bug. For desk tests, broadcast
  `am broadcast -a com.oculus.vrpowermanager.prox_close` on both headsets before launching — the
  starved-ticks counter is then the honest witness (near zero).
- **Back-to-back tiny datagrams invite reordering (2026-10-29).** `GameAudio::pump` had a
  3-frame catch-up cap, which put consecutive ~80-byte `'A'` datagrams on one socket — the exact
  traffic the old relay rule made fatal. The pump now emits at most one frame per call: the room
  loop ticks every ~10 ms while audio frames are 20 ms apart, so catching up never needs a burst.
  The relay then saw zero reordering events for this traffic across a full run (room `VWWGV`).

## 15. What the relay will not do

No retransmission, no NAK, no keyframe regeneration, no bitrate shaping or transcoding, no mute
enforcement, no encryption or decryption on the data path, no TLS termination, no NAT traversal or
hole punching (clients only send outbound UDP), no per-recipient addressing on UDP, no game state or
anti-cheat beyond role and bandwidth policy, and no application-level capability negotiation.

Also worth knowing operationally: `GET /rooms` is unauthenticated in the current build, so a room
code plus its invite is the only thing standing between a stranger and your room. Do not print codes
in public channels.

And do not describe the sealing as if the relay were a stranger to your traffic. It mints the room
key, stores it recoverably, and hands it to every participant, which means:

- the relay operator (or anyone who roots the relay) can decrypt **and forge** frames in any room;
- participants can decrypt each other, and can produce frames every other participant accepts — there
  is no participant-to-participant authentication, only the relay's role policy;
- `HELLO` carries the `sessionSecret` in clear UDP, so someone on your network segment can capture it
  and join as that participant until the session is revoked or expires. The planned fix is a
  short-lived HTTPS-issued UDP token rather than the long-lived secret; it is not built.

Sealing is worth having — it keeps plaintext off the wire to the relay, and it makes the identity
header unforgeable, so nobody can rebadge themselves as the host. It is not confidentiality *from the
server*, and it is not end-to-end encryption.

## 16. Milestones, and the exit criterion for each

| Milestone | Deliverable | Exit criterion |
|---|---|---|
| M0 | version gate + `POST /rooms` + `HELLO` from the headset | `WELCOME` on a Quest, and the app refuses a non-v3 relay (including a v3 relay that does not advertise `maxSeq`) |
| M1 | host→guest video + game audio, one guest, **cursor not in the picture** | guest renders the host's screen; ~2 s of IDR interval; one-way latency **measured** and inside the 35–110 ms gate |
| M2 | `C` cursor overlay: host sends its pointer position, guest draws it on the video quad | guest sees the host's pointer move live, landing correctly on the letterboxed quad |
| M3 | `I`/`B`/`K` + policy levels + release-on-revoke | a revoked guest produces zero input; unplugging a guest leaves no stuck buttons |
| M4 | voice `M`, PTT, host mixing | two Quests converse; PTT-to-hear ≤ ~250 ms; a muted guest's uplink drops to zero |
| M5 | chat, 4 guests, abuse resilience | a flood from one guest does not take the room down. Sealing, which this milestone used to carry, is already implemented (§12) |

M1's latency measurement is a gate on the whole effort, not a checkbox. Do it before M2. The host
frame must not bake in the VR pointer circle — M2 puts a pointer back on the guest's screen as an
overlay from `C` live position data. Bake it into the video and the guest's pointer position
arrives a full RTT stale and rescaled, which defeats the `C` channel's purpose. (The in-game
cursor is exempt from this concern: the owner ruled its presence in the stream acceptable; the
capture hook excludes it anyway — §6.)

**M0 status**: the code half exists and works — `Coop::Session` does the create/join/close calls with
the gate, `Coop::Transport` does the connected UDP path with `HELLO`/`PING`/`BYE`, and both were run
against a live relay from a host build (`WELCOME`, `PONG`, `DENY` on a bad secret, `X` both ways, a
fragmented video frame reassembled byte-for-byte). Sealing was pulled forward out of M5 and is done to
the same standard: `Coop::Seal` is byte-identical to the relay's own implementation and sealed frames
cross a live relay intact in both directions. The relay side is ready too: it serves the control plane
over TLS itself, so a headset is not blocked by a cleartext policy or a reverse proxy.

What is left for M0's exit criterion is one thing, not three: **run it on a headset.** The NDK build
exists (all five units compile for `aarch64-linux-android29` and `libscummvm.so` links and packages
into a debug APK), and the trigger point exists as `Coop::startSelfTestIfRequested()` — a marker-file
self-test rather than a UI decision, which keeps it out of the feature design until that design is
made (§17). Everything above is host-verified and arm64-*compiled*; the device run is the first time
the crypto executes on the ABI it will ship on, and the self-test prints the seal cost per frame so
that number is measured rather than assumed. That exit criterion was met on 2026-10-23; the device
record is §14.

**M1 status** (in progress): both codec halves exist and **both have been measured on the Quest**.
The host side is `Coop::VideoEncoder` (`backends/platform/android-vr/coop-encode.{h,cpp}`); its
`video = send` run put 1200 real H.264 access units on the wire at the 4 Mbit/s cap and the Mac
guest reassembled every one (§14 has the numbers, including the p95 that only shows up under send
load). The guest side is `Coop::VideoDecoder` (`backends/platform/android-vr/coop-decode.{h,cpp}`),
the mirror unit: buffer-queue MediaCodec, fed by the wire reassembler, correlating decoded frames back
to their wire frameId for the latency number. Its `video = recv` run decoded a real Mac-hosted
stream: **p50 23.7 ms / p95 116.6 ms decode stage**, output format read back (c2.* decoders reject a
mime-only configure — dimensions must be requested, the bitstream still decides). The two halves then
ran **against each other on real hardware**: Quest 3 host → relay → Quest 2 guest — two device
families, one binary, no per-vendor codec branch — 1800 AUs sent,
1271 decoded on the Quest 2, decode stage **p50 2.5 ms / p95 3.4 ms** there — the Mac-hosted p50 was
the file feed, not the codec. Both units are
configured the way §6 specifies, and both carry their device's codec quirks as comments with dates.
The guest render is now built and **watched**: `display = true` hands decoded frames to the VR thread
through a mailbox and the virtual screen quad shows the host's video — two runs, ~1100 uploads each,
decode-to-texture p50 11 ms, human-confirmed (§14). The host capture is built too, and proven on a
real game scene: `capture = true` + `game =` autolaunch ran DOTT on the Quest 3, captured via the
§6 PBO path, and the Quest 2 decoded and uploaded every frame that reached it (§14). What remained
for M1 at that point: the mixer tap plus Opus and `RECORD_AUDIO`,
and the cursor hide for the captured copy (§6); the **end-to-end** latency gate has since been
**measured on-device** (the measured record follows) — with the real scene in the picture, the
gate's biggest term is not the network:
it is the encoder's 1-frame pipeline fed at the *game's* redraw rate (~10-12 fps), which measured
**p50 201 ms** submit-to-AU against 35 ms at the synthetic 30 fps pace. Feeding duplicate frames at
the encode cadence (nearly free for the codec: zero-residual P-frames) turns that pacing cost back
into the 35 ms the bench showed; decode stays ~2.5 ms and the render hop ~11 ms. The frame-budget
question is settled (256 fragments; §17), and the video has been **watched clean by a person**: a
290 s host run at full 8700-AU / 194-keyframe density reached the guest with zero lost frames and
the wearer noticed no artifacts at all (§14, room MDM8A). Each earlier artifact had traced to a
named bug — capture region, two chroma unit bugs, receive-buffer and queue sizing — and the earlier
loss readings were the Quest 3's flapping Wi-Fi, not the pipeline. Paced sending, the recovery
guard, and whole-frame retransmission (X kind 9) are all built and measured on-device, and the
**latency gate has been measured** (2026-10-29, room `Z7DDZ`, 373 joined frames). Pipeline,
encode-submit to texture-upload, on the relay's clock: **p50 100 ms / p95 169 ms**, split as
encode+send 69 ms, wire+reassemble 14 ms, decode 3 ms, decode-to-upload 12 ms. The median is
inside the 35-110 ms budget and the tail is not, and the tail is dominated by the encoder's
1-frame pipeline (69 ms of the 100 ms) — which is also the term a real game adds to, because the
scene repaints at 10-12 fps rather than 30. That 69 ms is where any further work belongs; the
network and codec legs are not the problem. Pixel age is reported separately and must not be
mixed in: captured-to-submitted is p50 52 ms / p95 806 ms, i.e. how long the game's newest
pixels waited for the next encode tick, which is a property of the game's redraw rate and the
re-feed design, not of the transport. Both clocks were within a few tens of ms (worst half-RTT
3-35 ms) and the measurement keeps adb on USB, which cut clock error bars by an order of
magnitude over Wi-Fi adb. **Game audio is now built and audible** (2026-10-29, rooms
`6JW2X`→`F6A9M`→`VWWGV`). Host: `coop-gameaudio.{h,cpp}` taps the mixed device output at the
platform write seam (`audio-tap.h`), resamples 44.1→48 kHz with a continuous linear resampler,
and encodes fixed 20 ms Opus frames at ~32 kbps (`OPUS_APPLICATION_AUDIO`) as `'A'` media frames
(`audio = send`). Guest: decodes into a PCM ring exposed as an `Audio::AudioStream`, started once
as a permanent stream through `Mixer::playStream` — silence until data, PLC for gaps up to 5
frames then silence-fill, drift-drop with a 480 ms latency bound (`audio = recv`). Three faults
had to die before it sounded right, each recorded in §14: VR mode's missing device output (now
`aaudio-output.{h,cpp}`), the `mixCallback` return-unit tap bug (the garble: the tap was seeing a
quarter of every buffer), and the relay's `seq_regression` kick on reordered audio datagrams (now
a windowed replay defense). The clean run — `VWWGV`, 290 s video+audio, no kicks, room closed
`idle`, zero relay-observed reordering: the one-per-call pump pacing removed the bursts that got
reordered — sent 15,735 Opus frames with 0 send failures; the guest received 12,846 (**~82 %**;
63 gap events, 196 frames concealed by PLC/silence, 364 k samples drift-dropped, 1200 underruns),
and the person wearing the Quest 2 called the audio *"sounds good"* alongside video that *looks
good*. The listener verdict is the gate; the 18 % audio datagram loss with video in flight is not
signed off (§17). A follow-up run with inband FEC (`ECB8G`) reconstructed every single-frame
gap exactly and the listener called the audio better; the loss rate itself remains open (§17).
The last M1 item — keeping the host's pointer out of the captured copy (§6) — is **done**:
`preCursorHook` in the base OpenGL compositor moves the capture point before the cursor is drawn.
The VR pointer circle was already never in the capture (separate buffer, separate thread), and the
wearer re-confirmed it live: no circle in the streamed picture (room `ERADD`). The in-game cursor
is out of the copy too — capture probes at 0/10/90 s of DOTT, the 90 s interactive-scene frame
carries none (§14) — though the owner ruled that cursor explicitly not worth chasing either way,
so no human A/B was run on it. **M1's exit criteria are now all met.** `RECORD_AUDIO`
belongs to voice (M4), not to this path — the game-audio tap needs no capture permission.

## 17. Open problems to settle in code, not in a doc

- ~~A real game's keyframe can outgrow the client frame budget.~~ **Settled 2026-10-23 with
  scene data**: real DOTT IDRs measure up to 144304 B (135 fragments), so the 64-fragment cap
  dropped ~98 of the first 100 keyframes whole; the cap is now 256 fragments (275 KB) on both
  reassembler and sender pre-check, and a 240 s real-scene run sent 3181 AUs with 0 refusals and
  0 oversized drops. The encoder's own IDR sizing is left alone — the bitstream is already at
  the 4 Mbit/s cap, and a bigger budget costs RAM, not quality.
- **The pipeline's dominant term is now the host's encode+send leg** (p50 69 ms of the measured
  100 ms total, §16 `Z7DDZ`). It decomposes honestly as the codec's claimed 1-frame pipeline
  (~33 ms) plus the 250 µs fragment pacing a keyframe needs (~33 ms for a 144 KB IDR) — which
  suggests two cheap untried experiments: pacing only after the socket's send buffer shows
  queueing instead of a flat interval, and starting the next AU's fragments while an IDR's last
  ones are still paced. Do these only if the p95 tail matters for M2 input feel; the median is
  inside the M1 budget as measured.
- Loss recovery has a floor: the guest's IDR-resume rule turns any surviving gap into a freeze
  until the next keyframe (up to 1.5 s). Pacing plus buffer/queue sizing took a healthy link to
  zero measured loss (§14, MDM8A), and whole-frame retransmission (X kind 9) is built on both
  sides — and **it has now answered real requests on devices** (demo run `C87CK`, 2026-10-29:
  102 requests, 494 frames re-sent from history, 0 past the history window). It still could not
  hide the run's ~30 loss events (998 AUs skipped, 31 IDR resumes) — recovery is opportunistic
  and the floor is unchanged. If loss shows up on worse paths,
  the untried options are FEC on keyframes and a deliberate-loss test to exercise the retransmit
  numbers (`host retransmit:` / `guest retransmit:` counters exist for exactly that).
- **Game audio loss rides the shared downlink.** On the first clean video+audio run (room
  `VWWGV`) ~18 % of the host's `'A'` datagrams never reached the guest against ~4 % video loss,
  in 63 small gaps — the listener still rated the audio good, because one-frame-per-call pacing
  plus PLC plus the 480 ms buffer absorbs that shape. The first mitigation shipped: the encoder
  now sets inband FEC + `PACKET_LOSS_PERC` at the measured rate, and the guest takes the last
  lost frame of every gap from the arriving packet instead of concealing it. Measured (room
  `ECB8G`): 11 of 11 gap events FEC-recovered exactly — single-frame loss became inaudible, and
  the listener called the audio better. What remains is the loss rate itself: radio-side loss
  that hits both streams (C87CK: ~10 % of video AUs vs ~13 % of audio datagrams, and audio
  reached 18-23 % on other runs), which video partially reclaims through retransmission and
  audio cannot by design.
  The `host audio:` / `guest audio:` counters are the instrument. Multi-frame gaps still lean
  on PLC.
- Host is a single point of failure for the whole room, including for voice.
- No A/V sync signalling exists; buffers plus frame/audio dropping is the only mechanism.
- No per-peer quality: the host encodes one stream for everyone, so the worst path sets everyone's
  quality. Kind 2 statistics only let the relay's failsafe and the host pick the lowest common
  denominator.
- Sealing gives path-independent integrity and confidentiality *from the network*, but every
  participant can decrypt every other participant's stream by construction, and the relay can recover
  any room's key. There is no participant-to-participant authentication anywhere in the stack.
- No media key rotation, and no way to re-issue one without recreating the room.
- `HELLO` carries the `sessionSecret` in clear UDP: safe from anyone off-path, not from someone on
  your own network segment. Short-lived HTTPS-issued UDP tokens are the fix and are not built.
- Reconnect is not implemented in `Coop::Transport`, and it cannot be implemented *inside* the object:
  one instance is one identity, so reconnecting correctly means a new `Transport` **and** a new
  control-plane session. The game layer has to own that, and nothing currently does.
- Pose (`P`) is on the wire, routed, sequenced and spec'd, and **no client publishes or consumes it
  yet**. The avatar-side decisions are still open: whose origin a received pose is relative to (each
  participant's own stage), how to extrapolate `sentAtMs` into the receiver's frame clock, and where
  avatars are placed in a game that has no shared world.
- Voice has no jitter buffer, no echo handling, and no permission flow in this repo; the `RECORD_AUDIO`
  gap in §14 means the mic currently yields silence rather than an error.
- Input policy levels (`X` kind 3) are advertised by the host and enforced nowhere: the relay has no
  idea what an input event means, and the game code does not yet consult the policy before applying
  one.
- The device entry point is a **marker-file self-test, not a feature**. It logs through `g_system` from
  its own thread, so quitting the app mid-run races the backend teardown; it is capped (5 s warmup,
  15 s control plane, 10 s `HELLO`, ≤60 s joined) and meant to come out of the tree when the real
  lobby UI exists.
- The LAN trust anchor is a debug-build affordance: `debug-overrides` in
  `dists/android/res/xml/network_security_config.xml` trusts whatever `coop-relay/certs/coop-ca.key`
  signs, and whether the fork's `LETrustManager` indirection honours that is device-unverified (§2).
  A release build gets neither.
