#!/bin/bash
# vita-build.sh <target.json> <out.elf> [tinygo build args...]
# Links with TinyGo, then makes sure there's >= 8 KB free after the code
# segment (vita-elf-create appends module info there) by placing .data at a
# computed address and relinking if needed.
set -euo pipefail
TARGET=$1; OUT=$2; shift 2
tinygo build -target "$TARGET" -o "$OUT" "$@"
read TEXT_END DATA_START < <("$VITASDK/bin/arm-vita-eabi-readelf" -lW "$OUT" | python3 -c '
import sys
l=[x.split() for x in sys.stdin if x.strip().startswith("LOAD")]
print(int(l[0][2],16)+int(l[0][5],16), int(l[1][2],16))')
GAP=$((DATA_START - TEXT_END))
if [ $GAP -lt 8192 ]; then
  # Code ends right before a 64 KB boundary: add 16 KB of padding so the data
  # segment moves to the next boundary, leaving a large gap.
  T2=$(mktemp --suffix=.json)
  python3 - "$TARGET" "$T2" <<'PY'
import json,sys
t=json.load(open(sys.argv[1])); t['extra-files'].append('targets/vita_pad.c')
json.dump(t,open(sys.argv[2],'w'))
PY
  tinygo build -target "$T2" -o "$OUT" "$@"
  rm -f "$T2"
fi
"$VITASDK/bin/arm-vita-eabi-readelf" -lW "$OUT" | grep LOAD
