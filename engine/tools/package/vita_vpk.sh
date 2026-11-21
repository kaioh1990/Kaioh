#!/bin/bash
# Packages the Vita headless application (VITA_PORT.md milestone M1) as a VPK:
#   tools/package/vita_vpk.sh BUILD_DIR OUT.vpk
# BUILD_DIR is a CMake build with vitasdk's toolchain file (it holds the ELF
# cvf_vita_headless); VITASDK must point at the SDK. Title id CVFN00001
# ("CvF Native M1"): it installs next to the TinyGo CvF Online (CVFO00001).
# The steps are pack.sh's from the TinyGo port (claude/vita-port, vita/vemu):
# vita-elf-create, then vita_velf_fixrel.py (the Vita loader rewrites
# conditional BL relocations into plain B, which crashed TinyGo builds 2-8 on
# hardware), vita-make-fself, param.sfo with ATTRIBUTE2=12 (extended memory
# budget), the CvF LiveArea art from vita/cvfsys.
set -e
B=${1:?usage: vita_vpk.sh BUILD_DIR OUT.vpk}
OUT=${2:?usage: vita_vpk.sh BUILD_DIR OUT.vpk}
: "${VITASDK:?set VITASDK}"
P=$(cd "$(dirname "$0")" && pwd)
SYS=$(cd "$P/../../../vita/cvfsys" && pwd)
T="$VITASDK/bin"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
"$T/vita-elf-create" "$B/cvf_vita_headless" "$D/a0.velf" > "$D/elf-create.log"
python3 "$P/vita_velf_fixrel.py" "$D/a0.velf" "$D/a.velf"
"$T/vita-make-fself" -s -c "$D/a.velf" "$D/eboot.bin"
"$T/vita-mksfoex" -s TITLE_ID=CVFN00001 -s APP_VER=01.00 -d ATTRIBUTE2=12 "CvF Native M1" "$D/param.sfo"
"$T/vita-pack-vpk" -s "$D/param.sfo" -b "$D/eboot.bin" \
  -a "$SYS/icon0.png=sce_sys/icon0.png" \
  -a "$SYS/livearea/contents/bg0.png=sce_sys/livearea/contents/bg0.png" \
  -a "$SYS/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png" \
  -a "$SYS/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml" \
  "$OUT"
cp "$D/a.velf" "${OUT%.vpk}.velf"   # for vemu and core dump symbolisation
ls -la "$OUT"
