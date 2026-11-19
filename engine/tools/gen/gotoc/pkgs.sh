# Go packages translated with the game, and cgo bindings typed from source
# but implemented in src/platform. Sourced by regen.sh (gencdefs) and
# run_reach.sh (gotoc).
#
# auto: every third-party package the game imports is translated (the Lua
# VM, matrices, TrueType, the beep audio stack and its decoders, rollback
# netcode...), except bindings and GOTOC_SKIP_PKGS:
#   sqweek/dialog   cgo (GTK/Cocoa): native message boxes instead
#   x/exp, x/text   generic helpers and Shift_JIS: native handling
#   go-ini          native C port (src/ik/ik_ini.c)
#   qmuntal/gltf    3D stage models (not used by the native engine yet)
export GOTOC_PKGS=${GOTOC_PKGS-auto}
export GOTOC_SKIP_PKGS=${GOTOC_SKIP_PKGS-github.com/sqweek/dialog,golang.org/x/exp/...,golang.org/x/text/...,gopkg.in/ini.v1,github.com/qmuntal/gltf/...}
export GOTOC_STD_PKGS=${GOTOC_STD_PKGS-container/list}
export GOTOC_BIND_PKGS=${GOTOC_BIND_PKGS-github.com/veandco/go-sdl2/sdl,github.com/go-gl/gl/v3.3-core/gl}
