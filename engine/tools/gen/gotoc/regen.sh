#!/bin/sh
# Regenerates gen_types.h/gen_consts.h and the translation of the whole game
# (src/ik/ik_sim.c/.h, ik_reflect_stubs.c, ik_unsupported.c) from the patched
# Ikemen GO tree. See docs/DEVSETUP.md.
#
# Usage: IKEMEN_DIR=/path/to/Ikemen-GO tools/gen/gotoc/regen.sh
# (the tree comes from tools/gen/setup_ikemen.sh)
#
#   IKEMEN_DIR    Ikemen GO checkout (commit 6a09392 + tools/ikemen_headless.patch),
#                 modules downloaded (go mod download); required
#   SDL2_INCLUDE  directory containing SDL.h (default: pkg-config/sdl2-config,
#                 else Homebrew's include/SDL2)
#   GEN_BIN       where gotoc/gencdefs are built (default: engine/build/gen)
#   ALLOW_NEW_PLACEHOLDERS=1  accept new library placeholders (ik_unsupported.c)
#                 and rewrite tools/gen/unsupported_allowed.txt; without it a
#                 new placeholder fails the run
#   GOTOC_GOOS / GOTOC_GOARCH  target the Go sources are type-checked for
#                 (default linux/amd64, the configuration the committed C was
#                 generated with; the C itself decides the OS at compile time)
set -e
E=$(cd "$(dirname "$0")/../../.." && pwd)   # engine/
G=$E/tools/gen/gotoc
: "${IKEMEN_DIR:?set IKEMEN_DIR to the patched Ikemen GO tree (see docs/DEVSETUP.md)}"
IKEMEN_DIR=$(cd "$IKEMEN_DIR" && pwd)
GEN_BIN=${GEN_BIN:-$E/build/gen}
CC=${CC:-cc}
if [ -z "$SDL2_INCLUDE" ]; then
  SDL2_INCLUDE=$(pkg-config --variable=includedir sdl2 2>/dev/null)/SDL2
  [ -f "$SDL2_INCLUDE/SDL.h" ] || SDL2_INCLUDE=$(sdl2-config --prefix 2>/dev/null)/include/SDL2
  [ -f "$SDL2_INCLUDE/SDL.h" ] || SDL2_INCLUDE=$(brew --prefix 2>/dev/null)/include/SDL2
fi
[ -f "$SDL2_INCLUDE/SDL.h" ] || { echo "SDL.h not found: set SDL2_INCLUDE" >&2; exit 1; }

# build configuration of the translated Ikemen tree (see cvf_engine_common.go)
export GOTOC_TAGS=${GOTOC_TAGS-cvf}
export GOOS=${GOTOC_GOOS:-linux} GOARCH=${GOTOC_GOARCH:-amd64}
# module mode type-checking against the already downloaded module cache
export GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0
. "$G/pkgs.sh"
export GENCDEFS_OVERRIDES="$E/tools/gen/gencdefs/overrides.txt"

mkdir -p "$GEN_BIN"
(cd "$E/tools/gen/gotoc" && GOOS= GOARCH= go build -o "$GEN_BIN/gotoc" .)
(cd "$E/tools/gen/gencdefs" && GOOS= GOARCH= go build -o "$GEN_BIN/gencdefs" .)

(cd "$IKEMEN_DIR" && "$GEN_BIN/gencdefs" ./src "$E/src/core/gen_consts.h" "$E/src/core/gen_types.h" ALL)
GOTOC_BIN="$GEN_BIN/gotoc" GOTOC_HDR="$E/src/ik/ik_sim.h" GOTOC_GEN_TYPES="$E/src/core/gen_types.h" \
  "$G/run_reach.sh" "$IKEMEN_DIR/src" "$E/src/ik/ik_sim.c"
python3 "$E/tools/gen/gen_reflect_stubs.py" "$E/src/ik/ik_sim.h" "$E/src/ik/ik_reflect.c" "$E/src/ik/ik_reflect_stubs.c"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$E"
clang -std=gnu11 -fsyntax-only -ferror-limit=0 -w -Isrc -I"$SDL2_INCLUDE" src/ik/ik_sim.c 2> "$T/err_ik_sim.txt" || true
echo "ik_sim.c syntax errors: $(grep -c error: "$T/err_ik_sim.txt")"
# placeholders for library functions the runtime does not implement yet
# (paths relative to engine/, so directories with spaces work)
for f in src/ik/ik_*.c src/platform/sdl/*.c src/platform/gles/*.c; do
  case $f in *ik_sim.c|*ik_unsupported.c) continue;; esac
  $CC -std=gnu17 -O0 -w -Isrc -I"$SDL2_INCLUDE" -c "$f" -o "$T/$(basename "$f" .c).o"
done
for f in src/core/*.c; do $CC -std=gnu17 -O0 -w -Isrc -c "$f" -o "$T/core_$(basename "$f" .c).o"; done
# fails on placeholders not listed in tools/gen/unsupported_allowed.txt;
# ALLOW_NEW_PLACEHOLDERS=1 accepts them and rewrites the list
UPD=; [ "$ALLOW_NEW_PLACEHOLDERS" = 1 ] && UPD=--update
python3 tools/gen/gen_unsupported.py --allow tools/gen/unsupported_allowed.txt $UPD \
  src/ik/ik_sim.h src/ik/ik_unsupported.c "$T"/*.o
