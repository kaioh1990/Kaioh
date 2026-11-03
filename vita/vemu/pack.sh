#!/bin/bash
# pack.sh <elf> <out.vpk>   (ships the .velf as app0:ikvita.velf for the in-game image verifier)
set -e
S=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad; V=$S/vsdk/vitasdk; export PATH=$V/bin:$PATH
D=$(mktemp -d)
vita-elf-create $1 $D/a0.velf > /dev/null
python3 $S/vemu/velf_fixrel.py $D/a0.velf $D/a.velf
vita-make-fself -s -c $D/a.velf $D/eboot.bin
vita-mksfoex -s TITLE_ID=CVFO00001 -d ATTRIBUTE2=12 "CvF Online" $D/param.sfo
vita-pack-vpk -s $D/param.sfo -b $D/eboot.bin${PACK_VELF:+ -a $D/a.velf=ikvita.velf} $2
cp $D/a.velf ${2%.vpk}.velf
rm -rf $D
ls -la $2
