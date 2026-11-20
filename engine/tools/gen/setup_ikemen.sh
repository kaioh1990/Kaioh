#!/bin/sh
# Builds the Ikemen GO source tree that tools/gen/gotoc/regen.sh translates:
# Ikemen GO 6a09392 + the CvF fork (tools/ikemen_headless.patch) + the module
# replacements of the Vita/TinyGo tree the committed C was generated from
# (third/: patched go-sdl2, leonkasovan/gl, jfreymuth/vorbis, mewkiz/pkg copy,
# headless sqweek/dialog). See docs/DEVSETUP.md.
#
# Usage: tools/gen/setup_ikemen.sh DIR      (DIR is created or reset)
# Needs git, Go >= 1.27 and network access (Ikemen GO clone, go mod download).
set -e
E=$(cd "$(dirname "$0")/../.." && pwd)   # engine/
R=$(cd "$E/.." && pwd)                   # repository root (vita/ patches)
D=${1:?usage: setup_ikemen.sh DIR}
IKEMEN_REPO=${IKEMEN_REPO:-https://github.com/ikemen-engine/Ikemen-GO.git}
IKEMEN_COMMIT=6a09392

[ -d "$D/.git" ] || git clone -q --filter=blob:none "$IKEMEN_REPO" "$D"
cd "$D"
git checkout -q -f $IKEMEN_COMMIT
git clean -fdq
git apply "$E/tools/ikemen_headless.patch"
export GOTOOLCHAIN=local GOFLAGS=-mod=mod
go mod download

# third/: copies of module sources with the fork's changes
modcopy() {  # module dest
  src=$(go list -m -f '{{.Dir}}' "$1")
  [ -n "$src" ] || { echo "module $1 not found" >&2; exit 1; }
  rm -rf "third/$2"; mkdir -p third
  cp -R "$src" "third/$2"; chmod -R u+w "third/$2"
}
modcopy github.com/veandco/go-sdl2 sdl2
(cd third/sdl2 && patch -s -E -p3 < "$R/vita/go-sdl2-tinygo.patch")
modcopy github.com/leonkasovan/gl gl
(cd third/gl && patch -s -E -p3 < "$R/vita/gl-tinygo.patch")
modcopy github.com/jfreymuth/vorbis vorbis
(cd third/vorbis && patch -s -p2 < "$R/vita/vorbis-lookup1-fix.patch")   # lookup1Values in integer math
modcopy github.com/mewkiz/pkg mewkiz-pkg
mkdir -p third/dialog
printf 'module github.com/sqweek/dialog\n\ngo 1.20\n' > third/dialog/go.mod
cat > third/dialog/dialog.go <<'EOF'
// Package dialog is a headless stub used only for local test builds (no GTK available).
package dialog

import (
	"fmt"
	"os"
)

type MsgBuilder struct{ msg, title string }

func Message(format string, args ...interface{}) *MsgBuilder {
	return &MsgBuilder{msg: fmt.Sprintf(format, args...)}
}
func (b *MsgBuilder) Title(t string) *MsgBuilder { b.title = t; return b }
func (b *MsgBuilder) Info()                      { fmt.Fprintln(os.Stderr, b.title+": "+b.msg) }
func (b *MsgBuilder) Error()                     { fmt.Fprintln(os.Stderr, b.title+": "+b.msg) }
func (b *MsgBuilder) YesNo() bool                { fmt.Fprintln(os.Stderr, b.title+": "+b.msg); return false }
EOF
go mod edit \
  -replace github.com/sqweek/dialog=./third/dialog \
  -replace github.com/leonkasovan/gl=./third/gl \
  -replace github.com/veandco/go-sdl2=./third/sdl2 \
  -replace github.com/mewkiz/pkg=./third/mewkiz-pkg \
  -replace github.com/jfreymuth/vorbis=./third/vorbis
go mod download
echo "Ikemen GO tree ready: $D (export IKEMEN_DIR=$D)"
