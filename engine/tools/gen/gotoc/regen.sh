#!/bin/sh
# Regenerates gen_types.h/gen_consts.h and the simulation translation.
set -e
# build configuration of the translated Ikemen tree (see cvf_engine_common.go)
export GOTOC_TAGS=${GOTOC_TAGS-cvf}
S=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad
E=/home/claude/kaioh/engine
. $S/gotoc/pkgs.sh
export GENCDEFS_OVERRIDES=$E/tools/gen/gencdefs/overrides.txt
(cd $S/iktg && PATH=$S/goroot/bin:$PATH GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0 $S/gencdefs/gencdefs ./src $E/src/core/gen_consts.h $E/src/core/gen_types.h ALL)
GOTOC_HDR=$E/src/ik/ik_sim.h $S/gotoc/run_reach.sh $S/iktg/src $E/src/ik/ik_sim.c
python3 $E/tools/gen/gen_reflect_stubs.py $E/src/ik/ik_sim.h $E/src/ik/ik_reflect.c $E/src/ik/ik_reflect_stubs.c
cd $E && clang -std=gnu11 -fsyntax-only -ferror-limit=0 -w -Isrc -I/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/iktg/third/sdl2/_libs/include/SDL2 src/ik/ik_sim.c 2> /tmp/err_ik_sim.txt || true
echo "errors: $(grep -c error: /tmp/err_ik_sim.txt)"
# placeholders for library functions the runtime does not implement yet
U=$(mktemp -d)
for f in $E/src/ik/ik_*.c $E/src/platform/sdl/*.c; do
  case $f in *ik_sim.c|*ik_unsupported.c) continue;; esac
  gcc -std=gnu17 -O0 -w -I$E/src -I$S/iktg/third/sdl2/_libs/include/SDL2 -c $f -o $U/$(basename $f .c).o
done
for f in $E/src/core/*.c; do gcc -std=gnu17 -O0 -w -I$E/src -c $f -o $U/core_$(basename $f .c).o; done
: > $E/src/ik/ik_unsupported.c
python3 $E/tools/gen/gen_unsupported.py $E/src/ik/ik_sim.h $E/src/ik/ik_unsupported.c $U/*.o
rm -rf $U
