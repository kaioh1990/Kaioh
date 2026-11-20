#!/bin/bash
# Builds CvfEngine.xcframework (iOS device arm64 + iOS Simulator arm64) from
# the engine sources, on a Mac with Xcode, CMake and Ninja (IOS_PORT.md 2.1).
#
# Usage: tools/package/ios_build.sh SDL2_HEADERS OUTDIR
#   SDL2_HEADERS  SDL2.framework/Headers of the app's SDL2.xcframework
#   OUTDIR        gets CvfEngine.xcframework; build trees in OUTDIR/build
#                 (BUILD_TYPE=RelWithDebInfo by default)
#
# Each slice: CMake (CMAKE_SYSTEM_NAME=iOS) builds cvf_core + cvf_ik with
# the determinism flags and -fvisibility=hidden; their objects are pre-linked
# into one object (ld -r) that exports only the host API
# (src/platform/ios/exports.txt), so the generated names (os_Args, url_Parse,
# sdl_*, gl_*...) cannot clash with SDL2, ZipArchive, zlib or vInputKit.
# The unstripped master object stays in the build tree for symbolication.
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
SDLH=${1:?usage: ios_build.sh SDL2_HEADERS OUTDIR}
OUT=${2:?usage: ios_build.sh SDL2_HEADERS OUTDIR}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
BT=${BUILD_TYPE:-RelWithDebInfo}
MIN=${IOS_DEPLOYMENT_TARGET:-17.0}
XCF_ARGS=()
for sdk in iphoneos iphonesimulator; do
  B="$OUT/build/$sdk"
  mkdir -p "$B"
  case $sdk in
    iphoneos) target=arm64-apple-ios$MIN ;;
    *) target=arm64-apple-ios$MIN-simulator ;;
  esac
  cmake -S "$E" -B "$B" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN \
    -DCMAKE_BUILD_TYPE=$BT -DCVF_SDL2_INCLUDE_DIR="$SDLH" > "$B.cmake.log"
  cmake --build "$B" --target cvf_ik
  rm -rf "$B/objs" && mkdir -p "$B/objs/core" "$B/objs/ik"
  (cd "$B/objs/core" && ar -x "$B/libcvf_core.a")
  (cd "$B/objs/ik" && ar -x "$B/libcvf_ik.a")
  SYSROOT=$(xcrun --sdk $sdk --show-sdk-path)
  xcrun --sdk $sdk clang -target $target -isysroot "$SYSROOT" -r -nostdlib \
    -Wl,-exported_symbols_list,"$E/src/platform/ios/exports.txt" \
    "$B"/objs/core/*.o "$B"/objs/ik/*.o -o "$B/cvf_engine.o"
  rm -f "$B/libcvf_engine.a"
  xcrun --sdk $sdk libtool -static -o "$B/libcvf_engine.a" "$B/cvf_engine.o"
  n=$(nm -gU "$B/cvf_engine.o" | wc -l | tr -d ' ')
  want=$(wc -l < "$E/src/platform/ios/exports.txt" | tr -d ' ')
  [ "$n" = "$want" ] || { echo "$sdk: $n exported symbols, expected $want" >&2; exit 1; }
  XCF_ARGS+=(-library "$B/libcvf_engine.a" -headers "$E/src/platform/ios/include")
done
rm -rf "$OUT/CvfEngine.xcframework"
xcodebuild -create-xcframework "${XCF_ARGS[@]}" -output "$OUT/CvfEngine.xcframework"
echo "CvfEngine.xcframework ready in $OUT"
