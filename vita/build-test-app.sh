#!/bin/bash
# Builds the Vita test app (vita/test-app) into CvF-Vita-Test2.vpk.
# Needs: VITASDK (with the toolchain), TinyGo 0.44 in PATH, Go 1.27.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${VITASDK:?set VITASDK}"
TG="$(tinygo env TINYGOROOT)"
cp "$HERE/tinygo/vita_shim.c" "$TG/targets/vita_shim.c"
sed "s#@V@#$VITASDK#g" "$HERE/tinygo/vita.json.in" > "$HERE/tinygo/vita.json"
cd "$HERE/test-app"
cp ../../ikemen/src/cvf_detmath.go detmath.go 2>/dev/null || true
tinygo build -target ../tinygo/vita.json -o vtest.elf .
"$VITASDK/bin/vita-elf-create" vtest.elf vtest.velf
"$VITASDK/bin/vita-make-fself" -s vtest.velf eboot.bin
"$VITASDK/bin/vita-mksfoex" -s TITLE_ID=CVFT00001 -d ATTRIBUTE2=12 "CvF Vita Test" param.sfo
"$VITASDK/bin/vita-pack-vpk" -s param.sfo -b eboot.bin CvF-Vita-Test2.vpk
ls -la CvF-Vita-Test2.vpk
