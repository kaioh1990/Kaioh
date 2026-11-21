#!/bin/bash
# Builds the assets of the CvF Native Android app (ANDROID_PORT.md): the
# engine's own Lua scripts (Ikemen GO 6a09392's external/script +
# patches/ikemen-cvf-online.patch, as tools/package/ios_assets.sh), SDL's
# game controller database, and the default save/config.ini (cvf-files with
# RenderMode = OpenGL ES 3.2; the app never overwrites an existing one), plus
# external/.cvf-engine (engine revision). The app copies them into the game
# folder when the scripts differ (AssetExtractor, manifest.txt).
#
# Usage: tools/package/android_assets.sh IKEMEN_GIT_TREE GAMECONTROLLERDB OUTDIR
#   IKEMEN_GIT_TREE   any Ikemen GO clone that has commit 6a09392
#   GAMECONTROLLERDB  gamecontrollerdb.txt (SDL_GameControllerDB)
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
R=$(cd "$E/.." && pwd)
IK=${1:?usage: android_assets.sh IKEMEN_GIT_TREE GAMECONTROLLERDB OUTDIR}
DB=${2:?usage: android_assets.sh IKEMEN_GIT_TREE GAMECONTROLLERDB OUTDIR}
OUT=${3:?usage: android_assets.sh IKEMEN_GIT_TREE GAMECONTROLLERDB OUTDIR}
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
(cd "$OUT" && git init -q && mkdir -p external/script save &&
  for f in debug default main menu options start; do
    git -C "$IK" show 6a09392:external/script/$f.lua > external/script/$f.lua
  done &&
  git apply --include='external/script/*' "$R/patches/ikemen-cvf-online.patch")
rm -rf "$OUT/.git"
cp "$DB" "$OUT/external/gamecontrollerdb.txt"
sed -e 's/^RenderMode\( *\)= OpenGL 3.3/RenderMode\1= OpenGL ES 3.2/' "$R/cvf-files/save/config.ini" > "$OUT/save/config.ini"
grep -q '^RenderMode *= OpenGL ES 3.2' "$OUT/save/config.ini"
git -C "$R" rev-parse --short HEAD > "$OUT/external/.cvf-engine" 2>/dev/null || echo unknown > "$OUT/external/.cvf-engine"
find "$OUT" -type f | sort
