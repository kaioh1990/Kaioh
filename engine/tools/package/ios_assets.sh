#!/bin/bash
# Builds the iOS app's DefaultAssets.zip for the native engine
# (IOS_PORT.md 6): the engine's own Lua scripts (Ikemen GO 6a09392's
# external/script + patches/ikemen-cvf-online.patch, as run_smoke.sh uses),
# plus external/gamecontrollerdb.txt and save/config.ini from the Go app's
# DefaultAssets.zip, a manifest and external/.cvf-engine (engine revision).
# The app re-extracts the scripts when they differ from the bundled ones.
#
# Usage: tools/package/ios_assets.sh IKEMEN_GIT_TREE BASE_DEFAULTASSETS.zip OUT.zip
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
R=$(cd "$E/.." && pwd)
IK=${1:?usage: ios_assets.sh IKEMEN_GIT_TREE BASE.zip OUT.zip}
BASE=${2:?usage: ios_assets.sh IKEMEN_GIT_TREE BASE.zip OUT.zip}
OUT=${3:?usage: ios_assets.sh IKEMEN_GIT_TREE BASE.zip OUT.zip}
case $OUT in /*) ;; *) OUT=$PWD/$OUT ;; esac
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
(cd "$T" && git init -q && mkdir -p external/script save &&
  for f in debug default main menu options start; do
    git -C "$IK" show 6a09392:external/script/$f.lua > external/script/$f.lua
  done &&
  git apply --include='external/script/*' "$R/patches/ikemen-cvf-online.patch")
rm -rf "$T/.git"
unzip -q -o "$BASE" external/gamecontrollerdb.txt save/config.ini -d "$T"
printf 'external/script\nexternal/gamecontrollerdb.txt\nsave/config.ini\n' > "$T/manifest.txt"
git -C "$R" rev-parse --short HEAD > "$T/external/.cvf-engine" 2>/dev/null || echo unknown > "$T/external/.cvf-engine"
rm -f "$OUT"
(cd "$T" && zip -q -r -X "$OUT" manifest.txt external save)
unzip -l "$OUT"
