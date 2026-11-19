#!/bin/sh
# Regenerates the reachable-simulation translation.
# Usage: run_reach.sh IKEMEN_SRC_DIR OUT.c   (GOTOC_HDR: header path)
export GOTOC_EXCLUDE=render_gl33.go,font_gl33.go,font_vk.go,render_vk_swap.go,util_linux.go,util_darwin.go,cvfcook.go,font.go,input_sdl.go,luacache.go,model.go,music.go,netplay.go,render.go,render_gles32.go,render_vk.go,sound.go,spec_match.go,storyboard.go,system_sdl.go,util_android.go,util_desktop.go,util_ios.go,util_raw.go,util_vita.go,video_ffmpeg.go,video_stub_vita.go,vita_platform.go,vita_platform_other.go,vk_stub_vita.go,rollback.go,audio_sdl.go,sound_xm.go,compiler_bchash.go
# simulation functions living in otherwise excluded (platform) files
export GOTOC_INCLUDE_FUNCS=Rotation.IsZero,itoa,fmtHex,Motif.resolvePath
# functions implemented by hand in src/ik (ik_native.c)
export GOTOC_NATIVE_FUNCS=decodeShiftJIS,cvfGcSafepoint
# platform/render/diagnostic functions in otherwise translated files
export GOTOC_EXCLUDE_FUNCS=runGfxTask,GetKeyboardState,saveScreenshot,parseMusicSection,Char.cueDebugDraw,AnimTextSnd.Draw,FightScreenCombo.draw,Loader.runTread,System.drawDebugText,modifyBgm.Run,modifySnd.Run,Layout.DrawText
# Go packages translated besides main (the Lua VM the menus run on)
export GOTOC_PKGS=${GOTOC_PKGS-github.com/yuin/gopher-lua,github.com/yuin/gopher-lua/ast,github.com/yuin/gopher-lua/parse,github.com/yuin/gopher-lua/pm}
export GOTOC_GEN_TYPES=${GOTOC_GEN_TYPES:-/home/claude/kaioh/engine/src/core/gen_types.h}
# type-check in module mode (resolves pure-Go dependencies such as go-ini)
export PATH=/tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/goroot/bin:$PATH GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off CGO_ENABLED=0
SRC=$(cd "$1" && pwd)
cd "$SRC/.."
exec /tmp/claude-0/-home-claude/54342c0c-22b1-5a32-9784-78de9b3a8983/scratchpad/gotoc/gotoc ./src reach:cvfHeadlessMain > "$2"
