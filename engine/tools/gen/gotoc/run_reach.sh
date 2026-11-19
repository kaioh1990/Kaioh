#!/bin/sh
# Regenerates the reachable-simulation translation. Usage: run_reach.sh IKEMEN_SRC OUT.c
export GOTOC_EXCLUDE=bc_compile.go,bc_fuse.go,bcprof.go,cvf_online.go,cvfcook.go,font.go,input_sdl.go,luacache.go,model.go,motif.go,music.go,netplay.go,render.go,render_gles32.go,render_vk.go,sound.go,spec_match.go,statedump.go,storyboard.go,system_sdl.go,util_android.go,util_desktop.go,util_ios.go,util_raw.go,util_vita.go,video_ffmpeg.go,video_stub_vita.go,vita_platform.go,vita_platform_other.go,vk_stub_vita.go
export GOTOC_GEN_TYPES=${GOTOC_GEN_TYPES:-/home/claude/kaioh/engine/src/core/gen_types.h}
exec "$(dirname "$0")/gotoc" "$1" reach:Loader.load,System.runMatch,System.resetRound,System.loadStart > "$2"
