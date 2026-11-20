#!/bin/sh
# Menu smoke test of the native game (see smoke.lua): title -> Versus
# character select -> Online > Host Nearby (found by cvf_nearby_test over
# UDP 7520 from a second process) -> Online > Join Nearby -> "SMOKE OK".
#
#   tests/smoke/run_smoke.sh GAMEDIR WORKDIR BUILDDIR IKEMEN_DIR
#
# GAMEDIR    the full game (used read-only: WORKDIR links to its files)
# WORKDIR    created; gets its own save/, data/cvf_online.ini, options11.def
#            and the current Lua scripts
# BUILDDIR   CMake build with cvf and cvf_nearby_test
# IKEMEN_DIR an Ikemen GO git tree (tools/gen/setup_ikemen.sh): the scripts
#            are 6a09392's external/script + patches/ikemen-cvf-online.patch
# Needs a display (the game opens its window); about 75 s. With an engine
# that lacks the bindings the current scripts call, the log shows the Lua
# error ("attempt to call a non-function object" at character select).
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
R=$(cd "$E/.." && pwd)
G=$(cd "${1:?GAMEDIR}" && pwd)
W=${2:?WORKDIR}
B=$(cd "${3:?BUILDDIR}" && pwd)
IK=$(cd "${4:?IKEMEN_DIR}" && pwd)

mkdir -p "$W"
W=$(cd "$W" && pwd)
rm -rf "$W/data" "$W/external" "$W/save" "$W/smoke-hosting" "$W/smoke-scanned" "$W/scan"
mkdir -p "$W/data" "$W/external/script" "$W/save"
for f in "$G"/*; do
  n=$(basename "$f")
  case "$n" in save|data|external|*.app|*.exe|*.dll) ;; *) ln -sfn "$f" "$W/$n" ;; esac
done
for f in "$G"/data/*; do ln -sfn "$f" "$W/data/$(basename "$f")"; done
for f in "$G"/external/*; do
  [ "$(basename "$f")" = script ] || ln -sfn "$f" "$W/external/$(basename "$f")"
done
# current game data and rules (replace the links, never write through them)
for f in cvf_online.ini options11.def; do
  rm -f "$W/data/$f"
  cp "$R/cvf-files/data/$f" "$W/data/$f"
done
# current Lua scripts
T=$(mktemp -d)
(cd "$T" && git init -q && mkdir -p external/script &&
  for f in debug default main menu options start; do
    git -C "$IK" show 6a09392:external/script/$f.lua > external/script/$f.lua
  done &&
  git apply --include='external/script/*' "$R/patches/ikemen-cvf-online.patch")
cp "$T"/external/script/*.lua "$W/external/script/"
rm -rf "$T"
cp "$E/tests/smoke/smoke.lua" "$W/external/script/smoke.lua"
# config: the game's own, with the smoke module; a lobby name (no prompt)
if [ -f "$G/save/config.ini" ]; then cp "$G/save/config.ini" "$W/save/config.ini"; fi
if [ -f "$W/save/config.ini" ]; then
  sed -i.bak 's|^Modules *=.*|Modules = external/script/smoke.lua|' "$W/save/config.ini"
  rm -f "$W/save/config.ini.bak"
  grep -q 'smoke.lua' "$W/save/config.ini" || { echo "no Modules line in config.ini" >&2; exit 1; }
else
  printf '[Common]\nModules = external/script/smoke.lua\n' > "$W/save/config.ini"
fi
printf '{"name":"Smoke"}' > "$W/save/cvf_online_profile.json"
TAG=$(sed -n 's/^BuildTag *= *//p' "$W/data/cvf_online.ini" | tr -d '\r')
PORT=$(sed -n 's/^ListenPort *= *//p' "$W/save/config.ini" | head -1 | tr -d '\r ')
PORT=${PORT:-7500}

cd "$W"
"$B/cvf" > smoke.log 2>&1 &
PID=$!
# a Lua error leaves the engine's error dialog open: give up after 5 minutes
( i=0; while [ $i -lt 300 ] && kill -0 $PID 2>/dev/null; do sleep 1; i=$((i + 1)); done
  kill -9 $PID 2>/dev/null ) > /dev/null 2>&1 &
# while the game hosts Nearby, find it from another process
i=0
while [ ! -f smoke-hosting ] && kill -0 $PID 2>/dev/null && [ $i -lt 1200 ]; do sleep 0.1; i=$((i + 1)); done
SCAN=skipped
if [ -f smoke-hosting ]; then
  mkdir -p scan
  if "$B/cvf_nearby_test" "$W/scan" scan "$PORT" Smoke "$TAG" > scan.log 2>&1; then SCAN=ok; else SCAN=FAILED; fi
  touch smoke-scanned
fi
wait $PID || true
grep '^SMOKE' smoke.log || true
echo "outside Nearby scan: $SCAN"
grep -q '^SMOKE .*SMOKE OK' smoke.log && [ "$SCAN" = ok ] && echo "smoke: PASS" && exit 0
echo "smoke: FAIL (see $W/smoke.log, $W/scan.log)"
exit 1
