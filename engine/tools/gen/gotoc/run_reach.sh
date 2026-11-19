#!/bin/sh
# Regenerates the translation of the whole game (src/ik/ik_sim.c/.h).
# Usage: run_reach.sh IKEMEN_SRC_DIR OUT.c   (GOTOC_HDR: header path)
#
# Roots: main (the game) and cvfHeadlessMain (the headless reference runner).
# Not translated: the Vulkan renderer, ffmpeg video and the libxmp tracker
# player (cgo libraries without a native replacement yet).
export GOTOC_EXCLUDE=render_vk.go,render_vk_swap.go,font_vk.go,video_ffmpeg.go,sound_xm.go,cvfcook.go,model.go
export GOTOC_INCLUDE_FUNCS=
# functions implemented by hand in src/ik (ik_native.c)
export GOTOC_NATIVE_FUNCS=decodeShiftJIS,cvfGcSafepoint
export GOTOC_EXCLUDE_FUNCS=saveScreenshot
. /tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/gotoc/pkgs.sh
export GOTOC_GEN_TYPES=${GOTOC_GEN_TYPES:-/home/claude/kaioh/engine/src/core/gen_types.h}
# type-check in module mode (resolves pure-Go dependencies such as go-ini)
export PATH=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/goroot/bin:$PATH GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0
SRC=$(cd "$1" && pwd)
cd "$SRC/.."
exec /tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/gotoc/gotoc ./src reach:main,cvfHeadlessMain > "$2"
