# android_project_vr

The gradle project that packages the Quest APK. Its sibling `../android_project`
is the ordinary Android port's project and says nothing about VR; this one
carries the OpenXR activity, the VR manifest, and the native libraries.

## What is tracked and what is not

Tracked: the gradle scripts, the wrapper, and `mainAssets` (APK assets).
Ignored, per the repo `.gitignore`:

- `local.properties` - `sdk.dir=` is per-machine. Write your own:
  `echo 'sdk.dir=/path/to/android-sdk' > local.properties`
- `src.properties` - `srcdir=` points at the ScummVR checkout root for
  `mainAssets.gradle`. Write your own the same way.
- `build/` - gradle output.
- `lib/` - native libraries, see below.

## Recreating lib/ on a fresh checkout

`lib/arm64-v8a/` holds three libraries, none committed (the fork's convention
for native binaries, same as the ordinary port):

| file | where it comes from |
|---|---|
| `libscummvm.so` | built from this repo: configure for the android21 VR target, `make libscummvm.so -j8`, then `cp libscummvm.so android_project_vr/lib/arm64-v8a/` |
| `libopenxr_loader.so` | the OpenXR loader for Android/arm64-v8a, from the OpenXR SDK |
| `libc++_shared.so` | the NDK's `toolchains/llvm/prebuilt/<host>/sysroot/usr/lib/aarch64-linux-android/29/libc++_shared.so` |

## Building

    cd android_project_vr && ./gradlew assembleDebug

Output: `build/outputs/apk/debug/ScummVM-debug.apk`, installed with
`adb install -r`.

## Coop test runs

A debug build reads `/sdcard/scummvm-coop-test.txt` as a self-test marker if
the file exists; see `MULTIPLAYER-CLIENT-GUIDE.md` in
`backends/platform/android-vr/` for the keys and the relay's TLS trust setup
(`coop-relay/scripts/lan-cert.sh` writes the CA into `../dists/android/res/raw/`,
which **is** tracked, and the APK must be rebuilt after it changes).
