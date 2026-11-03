#!/bin/bash
# build engine -> $1 (elf), velf+stubs alongside
set -e
S=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad; V=$S/vsdk/vitasdk; T=$S/tg/tinygo
cd $S/iktg
VITASDK=$V PATH=$S/goroot-vita/bin:$T/bin:$V/bin:$PATH CGO_CFLAGS="-std=c2x -DSDL_DISABLE_ARM_NEON_H -I$S/glinc -I$V/arm-vita-eabi/include/SDL2 -I$V/arm-vita-eabi/include" $S/vitago/vita-build.sh $S/vitago/vita.json $1 -opt=${OPT:-z} -tags vita ./src > $1.log 2>&1 || { tail -30 $1.log; exit 1; }
$V/bin/arm-vita-eabi-objdump -d $1 | grep -c 'svc' || true
$V/bin/vita-elf-create -v $1 ${1%.elf}.velf > ${1%.elf}.stubs 2>&1
echo built $1
