#!/bin/sh
# Regenerates gen_types.h/gen_consts.h and the simulation translation.
set -e
S=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad
E=/home/claude/kaioh/engine
export GENCDEFS_OVERRIDES=$E/tools/gen/gencdefs/overrides.txt
(cd $S/iktg && PATH=$S/goroot/bin:$PATH GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0 $S/gencdefs/gencdefs ./src $E/src/core/gen_consts.h $E/src/core/gen_types.h ALL)
GOTOC_HDR=$E/src/ik/ik_sim.h $S/gotoc/run_reach.sh $S/iktg/src $E/src/ik/ik_sim.c
python3 $E/tools/gen/gen_reflect_stubs.py $E/src/ik/ik_sim.h $E/src/ik/ik_reflect.c $E/src/ik/ik_reflect_stubs.c
cd $E && clang -std=gnu11 -fsyntax-only -ferror-limit=0 -w -Isrc src/ik/ik_sim.c 2> /tmp/err_ik_sim.txt || true
echo "errors: $(grep -c error: /tmp/err_ik_sim.txt)"
