MODULE := backends/platform/android

MODULE_OBJS := \
	jni-android.o \
	asset-archive.o \
	android.o \
	aaudio-output.o \
	events.o \
	options.o \
	snprintf.o \
	touchcontrols.o

ifdef NEED_ANDROID_CPUFEATURES
MODULE_OBJS += \
	cpu-features.o
$(MODULE)/android.o: CXXFLAGS += "-I$(ANDROID_NDK_ROOT)/sources/android/cpufeatures"
# We don't configure a C compiler, use a C++ one in C mode
$(MODULE)/cpu-features.o: $(ANDROID_NDK_ROOT)/sources/android/cpufeatures/cpu-features.c
	$(QUIET)$(MKDIR) $(*D)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) -x c -std=c99 -c $(<) -o $@
endif

# We don't use rules.mk but rather manually update OBJS and MODULE_DIRS.
MODULE_OBJS := $(addprefix $(MODULE)/, $(MODULE_OBJS))
OBJS := $(MODULE_OBJS) $(OBJS)

# The VR app's minSdk is 29; that file's AAudio calls are checked against
# the 29 API instead of the global target, and the symbols are resolved at
# link time from libaaudio.so. (The NDK's unified sysroot carries it; the
# runtime on a Quest is API 29+.)
$(MODULE)/aaudio-output.o: CXXFLAGS += -target aarch64-linux-android29
MODULE_DIRS += $(sort $(dir $(MODULE_OBJS)))

ifdef USE_OPENXR
VR_MODULE := backends
VR_OBJS := \
	graphics/android-vr/android-vr-graphics.o \
	platform/android-vr/openxr-session.o \
	platform/android-vr/openxr-input.o \
	platform/android-vr/vr-screen-geometry.o \
	platform/android-vr/coop-wire.o \
	platform/android-vr/coop-seal.o \
	platform/android-vr/coop-transport.o \
	platform/android-vr/coop-session.o \
	platform/android-vr/coop-selftest.o \
	platform/android-vr/coop-encode.o \
	platform/android-vr/coop-decode.o \
	platform/android-vr/coop-capture.o \
	platform/android-vr/coop-gameaudio.o \
	platform/android-vr/coop-cursor.o \
	platform/android-vr/android-vr-main.o \
	platform/android-vr/android_native_app_glue.o
VR_OBJS := $(addprefix $(VR_MODULE)/, $(VR_OBJS))
OBJS := $(VR_OBJS) $(OBJS)
MODULE_DIRS += $(sort $(dir $(VR_OBJS)))

# android_native_app_glue comes from the NDK
backends/platform/android-vr/android_native_app_glue.o: $(ANDROID_NDK_ROOT)/sources/android/native_app_glue/android_native_app_glue.c
	$(QUIET)$(MKDIR) $(*D)
	$(QUIET_CXX)$(CXX) $(filter-out -std=gnu++11,$(CXXFLAGS)) $(CPPFLAGS) -x c -std=c99 -c $(<) -o $@

backends/platform/android-vr/android-vr-main.o: CXXFLAGS += -I$(ANDROID_NDK_ROOT)/sources/android/native_app_glue
endif
