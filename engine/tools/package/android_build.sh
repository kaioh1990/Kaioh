#!/bin/bash
# Builds the native engine for Android arm64-v8a with the NDK (ANDROID_PORT.md):
#   OUTDIR/jniLibs/arm64-v8a/libmain.so, libSDL2.so   (what the APK ships)
#   OUTDIR/test/cvf_headless, cvf_tests                 (static test binaries:
#       same objects as libmain.so; run on arm64 Linux or under qemu-aarch64)
#
# Usage: tools/package/android_build.sh NDK_DIR OUTDIR
#   SDL2_SRC   SDL 2.32.10 source tree (default: cloned into OUTDIR/sdl2-src;
#              the app's SDLActivity.java checks for exactly 2.32.10)
#   API        minimum API level of the native code (default 26: getifaddrs
#              for Nearby needs 24; the Retroid Pocket 6 runs Android 13)
#   BUILD_TYPE (default Release)
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
NDK=${1:?usage: android_build.sh NDK_DIR OUTDIR}
OUT=${2:?usage: android_build.sh NDK_DIR OUTDIR}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
NDK=$(cd "$NDK" && pwd)
API=${API:-26}
BT=${BUILD_TYPE:-Release}
TC="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TC" ] || { echo "no NDK at $NDK" >&2; exit 1; }
GEN=()
command -v ninja > /dev/null && GEN=(-G Ninja)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)}

SDL=${SDL2_SRC:-$OUT/sdl2-src}
if [ ! -f "$SDL/include/SDL.h" ]; then
  git clone -q --depth 1 -b release-2.32.10 https://github.com/libsdl-org/SDL.git "$SDL"
fi
grep -q '#define SDL_PATCHLEVEL *10' "$SDL/include/SDL_version.h" &&
  grep -q '#define SDL_MINOR_VERSION *32' "$SDL/include/SDL_version.h" ||
  { echo "$SDL is not SDL 2.32.10 (SDLActivity.java requires it)" >&2; exit 1; }

# SDL2 as the app's libSDL2.so (as Ikemen GO's build.sh builds it)
B="$OUT/build/sdl2"
mkdir -p "$OUT/build"
cmake -S "$SDL" -B "$B" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
  -DSDL_TEST=OFF -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" > "$B.cmake.log"
cmake --build "$B" -j "$JOBS"
SDLSO=$(ls "$B"/libSDL2.so)
# headers: the source tree's plus the configured SDL_config.h / SDL_revision.h
SDLINC="$OUT/build/sdl2-include"
rm -rf "$SDLINC" && mkdir -p "$SDLINC"
cp "$SDL"/include/*.h "$SDLINC/"
cp "$B"/include/*.h "$SDLINC/" 2>/dev/null || true
cp "$B"/include-config-*/*.h "$SDLINC/" 2>/dev/null || true

# the engine
B="$OUT/build/engine"
cmake -S "$E" -B "$B" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=$BT \
  -DCVF_SDL2_INCLUDE_DIR="$SDLINC" -DCVF_SDL2_LIBRARY="$SDLSO" > "$B.cmake.log"
cmake --build "$B" -j "$JOBS" --target main cvf_headless cvf_nearby_test cvf_tests

LIB="$OUT/jniLibs/arm64-v8a"
mkdir -p "$LIB" "$OUT/test"
STRIP="$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/bin/llvm-strip"
"$STRIP" --strip-unneeded -o "$LIB/libmain.so" "$B/libmain.so"
"$STRIP" --strip-unneeded -o "$LIB/libSDL2.so" "$SDLSO"
cp "$B/cvf_headless" "$B/cvf_nearby_test" "$B/cvf_tests" "$OUT/test/"

# exports: only the JNI entry points, JNI_OnLoad and SDL_main
NM="$(dirname "$STRIP")/llvm-nm"
"$NM" -D --defined-only "$LIB/libmain.so" | awk '{print $3}' | sort > "$OUT/build/exports.txt"
n=$(wc -l < "$OUT/build/exports.txt" | tr -d ' ')
[ "$n" = 7 ] || { echo "libmain.so exports $n symbols, expected 7:" >&2; cat "$OUT/build/exports.txt" >&2; exit 1; }
echo "Android engine ready in $OUT (libmain.so $(wc -c < "$LIB/libmain.so") bytes)"
