#!/bin/bash
# Packages the Vita application (VITA_PORT.md milestones M1/M2) as a VPK:
#   tools/package/vita_vpk.sh BUILD_DIR OUT.vpk [TITLE]
# BUILD_DIR is a CMake build with vitasdk's toolchain file (it holds the ELF
# cvf_vita); VITASDK must point at the SDK. Title id CVFN00001 (TITLE, default
# "CvF Native"): it installs next to the TinyGo CvF Online (CVFO00001) and
# over an earlier CvF Native VPK (M1 had the same id).
# The steps are pack.sh's from the TinyGo port (claude/vita-port, vita/vemu):
# vita-elf-create, then vita_velf_fixrel.py (the Vita loader rewrites
# conditional BL relocations into plain B, which crashed TinyGo builds 2-8 on
# hardware), vita-make-fself, param.sfo with ATTRIBUTE2=12 (extended memory
# budget), and a text-only icon and LiveArea from vita/cvfnative-sys (no game
# artwork in the VPK).
set -e
B=${1:?usage: vita_vpk.sh BUILD_DIR OUT.vpk [TITLE]}
OUT=${2:?usage: vita_vpk.sh BUILD_DIR OUT.vpk [TITLE]}
TITLE=${3:-CvF Native}
: "${VITASDK:?set VITASDK}"
P=$(cd "$(dirname "$0")" && pwd)
SYS=$(cd "$P/../../../vita/cvfnative-sys" && pwd)
T="$VITASDK/bin"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
"$T/vita-elf-create" "$B/cvf_vita" "$D/a0.velf" > "$D/elf-create.log"
python3 "$P/vita_velf_fixrel.py" "$D/a0.velf" "$D/a.velf"
"$T/vita-make-fself" -s -c "$D/a.velf" "$D/eboot.bin"
"$T/vita-mksfoex" -s TITLE_ID=CVFN00001 -s APP_VER=01.00 -d ATTRIBUTE2=12 "$TITLE" "$D/param.sfo"
"$T/vita-pack-vpk" -s "$D/param.sfo" -b "$D/eboot.bin" \
  -a "$SYS/icon0.png=sce_sys/icon0.png" \
  -a "$SYS/livearea/contents/bg0.png=sce_sys/livearea/contents/bg0.png" \
  -a "$SYS/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png" \
  -a "$SYS/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml" \
  "$OUT"
cp "$D/a.velf" "${OUT%.vpk}.velf"   # for vemu and core dump symbolisation
ls -la "$OUT"
