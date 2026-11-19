#!/bin/sh
# Builds a self-contained Linux x86_64 folder: cvf + bundled SDL2 + launcher.
# Usage: linux_bundle.sh BUILD_DIR OUT_DIR
# Copy the result's contents next to the game data (chars/, data/, ...).
set -e
BUILD=$1; OUT=$2
mkdir -p "$OUT/lib"
cp "$BUILD/cvf" "$OUT/cvf.bin"
SDL=$(ldd "$BUILD/cvf" | awk '/libSDL2-2.0.so.0/ {print $3}')
[ -n "$SDL" ] && cp -L "$SDL" "$OUT/lib/libSDL2-2.0.so.0"
cat > "$OUT/CvF Native (Linux x64).sh" <<'L'
#!/bin/sh
# Launches the CvF native engine from the game folder this script sits in.
cd "$(dirname "$0")"
LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec ./cvf.bin "$@"
L
chmod +x "$OUT/CvF Native (Linux x64).sh" "$OUT/cvf.bin"
echo "bundle: $OUT"
