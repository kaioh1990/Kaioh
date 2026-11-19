#!/bin/sh
# Regenerates gen_types.h/gen_consts.h and the simulation translation.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
E=/home/claude/kaioh/engine
export GENCDEFS_OVERRIDES=$E/tools/gen/gencdefs/overrides.txt
(cd $S/iktg && $S/gencdefs/gencdefs ./src $E/src/core/gen_consts.h $E/src/core/gen_types.h ALL)
GOTOC_HDR=$E/src/ik/ik_sim.h $S/gotoc/run_reach.sh $S/iktg/src $E/src/ik/ik_sim.c
cd $E && clang -std=gnu11 -fsyntax-only -ferror-limit=0 -w -Isrc src/ik/ik_sim.c 2> /tmp/err_ik_sim.txt || true
echo "errors: $(grep -c error: /tmp/err_ik_sim.txt)"
