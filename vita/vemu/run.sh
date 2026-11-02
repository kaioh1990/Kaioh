#!/bin/bash
# run.sh <elf> [extra vemu args]   (uses <elf-base>.stubs)
S=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad
export PATH=$S/vsdk/vitasdk/bin:$PATH UCPY=$S/uc/unicorn/bindings/python VEMU_CLOBCALL=${VEMU_CLOBCALL:-0}
E=$1; shift
exec python3 $S/vemu/vemu.py $E ${E%.elf}.stubs "$@"
