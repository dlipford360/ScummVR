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

#ifndef BACKENDS_NETWORKING_BASIC_ANDROID_JNI_H
#define BACKENDS_NETWORKING_BASIC_ANDROID_JNI_H

#include <jni.h>

namespace Networking {

class NetJNI {
#ifdef USE_HTTP
	friend class ConnectionManagerAndroid;
	friend class NetworkReadStreamAndroid;
#endif
	friend class AndroidSocket;
	friend class AndroidURL;

public:
	/**
	 * Install the class loader used to resolve the org.scummvm.* classes in init().
	 *
	 * JNIEnv::FindClass resolves a class with the class loader of the *caller's*
	 * frame, so on a thread ScummVM created itself it returns null and throws for
	 * application classes - the platform ones (java.net.URL, java.lang.ClassLoader)
	 * still resolve. The classic port does not need this because it bootstraps from
	 * Java; the VR port bootstraps from native code and does. Call it from a thread
	 * that can already see an application object (any object's class knows its own
	 * loader) before init().
	 */
	static void setClassLoader(JNIEnv *env, jobject loader);

	static void init(JNIEnv *env);

private:
	/**
	 * FindClass, with the fallback described on setClassLoader(). Returns a local
	 * reference, like FindClass, or null when neither route found the class.
	 */
	static jclass findClass(JNIEnv *env, const char *name);

	static jobject     _classLoader;
	static jmethodID _MID_classLoader_loadClass;

	static jclass    _CLS_URL;

	static jmethodID _MID_url_init;
	static jmethodID _MID_url_getProtocol;
	static jmethodID _MID_url_getHost;
	static jmethodID _MID_url_getPort;
	static jmethodID _MID_url_getDefaultPort;

	static jclass    _CLS_Socket;

	static jmethodID _MID_socket_init;
	static jmethodID _MID_socket_ready;
	static jmethodID _MID_socket_send;
	static jmethodID _MID_socket_recv;
	static jmethodID _MID_socket_close;

#ifdef USE_HTTP
	static jclass    _CLS_HTTPManager;

	static jmethodID _MID_manager_init;
	static jmethodID _MID_manager_startRequest;
	static jmethodID _MID_manager_poll;
	static jfieldID  _FID_manager__empty;

	static const JNINativeMethod _natives_request[];

	static jclass    _CLS_HTTPRequest;

	static jmethodID _MID_request_bufinit;
	static jmethodID _MID_request_forminit;
	static jmethodID _MID_request_cancel;
	static jmethodID _MID_request_getURL;
#endif

	static bool _init;
};

} // End of namespace Networking

#endif

