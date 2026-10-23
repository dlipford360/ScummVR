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

#ifndef BACKENDS_PLATFORM_ANDROID_VR_COOP_SELFTEST_H
#define BACKENDS_PLATFORM_ANDROID_VR_COOP_SELFTEST_H

namespace Coop {

// Bring-up test for the coop stack, meant to be run on a headset before any of
// the feature work exists. It is scaffolding, not a feature: nothing in the UI
// reaches it, and the point is to answer four questions on the shipping ABI,
// with no game running -
//
//   1. can this build reach the relay's control plane at all (Java TLS stack,
//      cleartext policy, DNS, firewall), and does the version gate accept it;
//   2. does HELLO get a WELCOME on a real network, and does PING come back;
//   3. what does sealing cost on this SoC, measured with the real room key;
//   4. do sealed frames survive a real relay in both directions.
//
// It runs when a marker file is present, on its own thread, and logs everything
// through debug()/warning() - i.e. to logcat. Call it once, just before
// scummvm_main: the thread waits a few seconds first, so the backend and the Java
// HTTP layer exist by the time it starts talking.
//
// Marker file format, `key = value` per line, '#' comments:
//
//   relay   = https://192.168.1.20:8443   required
//   role    = host | guest                default host
//   code    = ABCDE                       guest only
//   invite  = <invite secret>             guest only
//   seconds = 20                          how long to stay joined (capped)
//   plaintext = 1                         opt in to an unsealed dev room
//
// A guest run is how a two-headset test is driven: one headset hosts, the log
// prints the room code and invite, the other gets them typed into its marker
// file. Secrets are never logged; lengths are.
//
// Honest limitation: the thread logs through g_system, so it must finish before
// ScummVM tears its backend down. It always does on its own - 5 s warmup, 15 s for
// the control plane, 10 s for HELLO, then at most 60 s joined - but quitting the app
// mid-run still races that. This file is expected to come out of the tree once the
// real entry point exists.
bool startSelfTestIfRequested();

} // namespace Coop

#endif
