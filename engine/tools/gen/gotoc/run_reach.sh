#!/bin/sh
# Regenerates the translation of the whole game (src/ik/ik_sim.c/.h).
# Usage: run_reach.sh IKEMEN_SRC_DIR OUT.c
#   GOTOC_HDR        header path (src/ik/ik_sim.h)
#   GOTOC_GEN_TYPES  gen_types.h written by gencdefs (default: src/core/gen_types.h)
#   GOTOC_BIN        gotoc binary (default: engine/build/gen/gotoc)
# Normally run through regen.sh, which also sets GOOS/GOARCH and module flags.
#
# Roots: main (the game), cvfHeadlessMain (the headless reference runner)
# and the iOS app entry points (GOTOC_HOST_ROOTS).
# Not translated: the Vulkan renderer, ffmpeg video and the libxmp tracker
# player (cgo libraries without a native replacement yet).
E=$(cd "$(dirname "$0")/../../.." && pwd)   # engine/
export GOTOC_EXCLUDE=render_vk.go,render_vk_swap.go,font_vk.go,video_ffmpeg.go,sound_xm.go,cvfcook.go,model.go
export GOTOC_INCLUDE_FUNCS=
# functions implemented by hand in src/ik (ik_native.c)
export GOTOC_NATIVE_FUNCS=decodeShiftJIS,cvfGcSafepoint,cvfVitaWindow,cvfVitaSwap,cvfVitaPerf,cvfP2PAdvertise,cvfP2PAdvertiseStop,cvfP2PBrowse,cvfP2PJoin,cvfP2PClose,cvfNativeAskName
export GOTOC_EXCLUDE_FUNCS=saveScreenshot
# entry points of the iOS app (src/platform/ios: SDL_main and the host API)
GOTOC_HOST_ROOTS=cvfHTTPSelfTest,cvfIOSMain,cvfHostInMatch,cvfHostTap,cvfHostSetTouchMenus,cvfHostPeers,cvfHostNameEntered
. "$E/tools/gen/gotoc/pkgs.sh"
export GOTOC_GEN_TYPES=${GOTOC_GEN_TYPES:-$E/src/core/gen_types.h}
GOTOC_BIN=${GOTOC_BIN:-$E/build/gen/gotoc}
# type-check in module mode (resolves pure-Go dependencies such as go-ini)
export GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0
export GOOS=${GOOS:-linux} GOARCH=${GOARCH:-amd64}
case "$2" in /*) OUT=$2;; *) OUT=$(pwd)/$2;; esac
SRC=$(cd "$1" && pwd)
cd "$SRC/.."
exec "$GOTOC_BIN" ./src reach:main,cvfHeadlessMain,$GOTOC_HOST_ROOTS > "$OUT"
