# Development setup (native engine)

How to get from nothing to a verified build: clone, Ikemen GO source tree,
regeneration of the translated C, CMake build, reference fight. Verified from
scratch on macOS 27 arm64 (Apple M-series, Go 1.27.4 and 1.27.3, Apple clang 21,
CMake 3.31) on 2026-11-20. Linux works the same way (GCC or clang, distro SDL2).

## 1. Tools

- git, CMake >= 3.20, a C17 compiler (Apple clang, GCC, clang), python3, `patch`
- Go 1.27.x (only for regeneration; building the C needs no Go)
- SDL2: headers + a library for the target architecture
  - Linux: `libsdl2-dev`
  - macOS arm64: the Homebrew under `/usr/local` is x86_64, so its library
    cannot be linked into an arm64 build. Use its headers
    (`/usr/local/include/SDL2`) with the arm64 `libSDL2-2.0.0.dylib`
    (sdl2-compat + `libSDL3.0.dylib`) shipped in
    `CvF Online/CvF Native (Apple Silicon).app/Contents/Frameworks/`.
    CMake from that Homebrew is also x86_64: pass
    `-DCMAKE_OSX_ARCHITECTURES=arm64` or it builds x86_64 objects.

## 2. Repository

    git clone -b claude/native-engine https://github.com/kaioh1990/Kaioh.git dev
    cd dev/engine

On the project Mac this is `~/Castlevania Fighter Project/CvF Native Engine/dev`.

## 3. Build and reference test (no Go needed)

    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCVF_SDL2_INCLUDE_DIR=/usr/local/include/SDL2 \
      -DCVF_SDL2_LIBRARY="$HOME/Castlevania Fighter Project/CvF Online/CvF Native (Apple Silicon).app/Contents/Frameworks/libSDL2-2.0.0.dylib"
    cmake --build build/release -j10      # cvf_headless, cvf, cvf_tests, cvftool
    ./build/release/cvf_tests             # 92 checks, 0 failures

(Linux: drop `CMAKE_OSX_ARCHITECTURES`, and the two SDL2 options if
`find_package(SDL2)` works.) `cvf` (the game) is built too; run it as
`cvf <gamedir>` or package it with `tools/package/macos_app.sh`. It writes
`save/` in the game directory.

### Reference fight

The reference game directory is `cvf-refdata.tgz` (Alucard, Richter,
ParanoiaBG, the data files they need, `save/`, and the two empty flag files
`vita_statedump.txt` / `vita_statehash.txt`: with them present the engine
uses the fixed seed and writes `statedump.txt`). It lacks
`data/Xsound.snd` (687 MB), which must come from the full game:

    mkdir refgame && tar xzf cvf-refdata.tgz -C refgame   # "hardlink pointing to itself" warnings are harmless
    ln -s "/path/to/CvF Online/data/Xsound.snd" refgame/data/Xsound.snd
    cd refgame
    ../build/release/cvf_headless . -p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 \
        -s stages/ParanoiaBG.def -rounds 1
    md5 statedump.txt    # Linux: md5sum

`statedump.txt` is written into the game directory, so do not point
cvf_headless at a folder whose `statedump.txt` you want to keep.

Expected results (both identical on Linux x86-64 and macOS arm64):

| Rules file (`data/cvf_online.ini`) | winner | lines | md5 |
| --- | --- | --- | --- |
| `cvf-online-2` (as in cvf-refdata.tgz) | 1 | 287,752 | `54f8ae06d74648af2182587b82a390d6` |
| `cvf-online-5` (CvF Online's current file: SpecialCooldown, FightAspect 4:3, WideView) | 2 | 224,158 | `4334350cdebd48dc0caa04cf78e0d9e3` |

Timings on the M-series Mac: full Release build ~27 s (-j10); one reference
match ~4.1 s wall (old rules) / ~4.3 s (cvf-online-5), loading included.

### Nearby test and menu smoke test

    ./build/release/cvf_nearby_test /tmp/nt      # UDP 7520 must be free; "0 failure(s)"
    tests/smoke/run_smoke.sh "/path/to/CvF Online" smoke build/release /path/to/dev-ikemen

The smoke test needs a display and the full game (linked read-only); it
plays title -> Versus character select -> Online > Host Nearby (found by
`cvf_nearby_test scan` from a second process) -> Join Nearby and prints
`smoke: PASS` (~75 s). See tests/README.md.

### Online match test (netplay)

    tests/netplay/run_netplay.sh /path/to/refgame          # "netplay: PASS (dump ...)"

Two `cvf_headless` processes play a full online match over loopback through
the real path: host `-ip ""` / join `-ip 127.0.0.1` (main.lua's command
line: enterNetPlay, TCP handshake, sync settings, synchronize), then GGPO
rollback over UDP. Each side's input is scripted (`CVF_NET_INPUT=seed`, a
function of the match time only), so the match must come out the same
whatever the network does. Scenarios (default `direct lag:40:15:0
loss:20:10:8`): `direct`, or `NAME:DELAY:JITTER:LOSS[:sync]` through
`tests/netplay/udp_relay.py` (delay and jitter in ms, loss in %; the loss
spares GGPO's sync handshake unless `:sync` is given; builds with the GGPO
handshake fix pass `:sync` cases too, see GAPS.md). Each side
writes the last simulation of every match frame to `statedump.txt`; PASS
needs both sides identical in every scenario and every scenario identical
to the first. It prints the frames re-simulated in rollbacks (thousands
with lag or loss) and the relay's packet counts. ~1.5 min (unpaced; `FPS=60`
paces both like a windowed game, ~2 min per scenario).

The instances are made in `$WORK` (default a temp dir) from the reference
game dir through symlinks, with their own `save/` (ports `PORT_BASE`=7650:
TCP 7650, UDP 7651-7654), `vita_statedump.txt` and `vita_statehash.txt`
(fixed seed and pre-match time, reproducible runs) and ButtonAssist off (it
keeps state per input poll, not per frame). `HOST_BIN` / `GUEST_BIN` choose
the engines, e.g. a Go build (below). Debugging: `GGPO_LOGS=1` (GGPO's trace
in each instance's `save/logs`), `RELAY_LOG=1` (one line per packet with the
GGPO header), `RELAY_SEED`, `TIMEOUT`, `PORT_BASE`, `SEED_HOST`/`SEED_GUEST`.

### Go build of the headless runner (cross-checks)

`tools/ikemen/cvf_gohead.go` (build tag `cvfgohead`) runs the same
`cvfHeadlessMain` from Go, so the Ikemen tree from section 4 builds a Go
`cvf_headless_go` that takes the same arguments and writes the same dumps:

    cd dev-ikemen
    go build -tags cvf,cvfgohead -o cvf_headless_go ./src   # native's configuration (vitaPerf)
    go build -tags cvfgohead -o cvf_headless_go_desktop ./src  # the shipping PC/mobile configuration

It needs cgo with SDL2, FFmpeg (7), libxmp and a `-lMoltenVK` on macOS. On
the project Mac (amd64 Go with the x86_64 Homebrew libraries, under
Rosetta), libxmp 4.6.3 was built from source into a local prefix and
MoltenVK replaced by a one-function stub library (`vkGetInstanceProcAddr`
returning NULL; nothing headless touches Vulkan):

    export GOARCH=amd64 CGO_ENABLED=1 CC="clang -arch x86_64" \
      PKG_CONFIG_PATH=/usr/local/opt/ffmpeg@7/lib/pkgconfig \
      CGO_CFLAGS="-I/usr/local/include/SDL2 -I/usr/local/include -I/usr/local/opt/ffmpeg@7/include -I$XMP/include" \
      CGO_LDFLAGS="-L/usr/local/lib -L/usr/local/opt/ffmpeg@7/lib -L$XMP/lib -L$NOVK -lSDL2"

(paths without spaces: cgo splits the flags on spaces). Results
(2026-11-20, Go 1.27.4 amd64 vs native macOS arm64 / Linux x86-64):

| Check | Result |
| --- | --- |
| Reference fight, old rules: Go `cvf` tag | `54f8ae06d74648af2182587b82a390d6`, = native |
| Reference fight, cvf-online-5: Go `cvf` tag | `4334350cdebd48dc0caa04cf78e0d9e3`, = native |
| Reference fight, cvf-online-5: Go desktop configuration | `4334350c...`, = native |
| Reference fight, old rules: Go desktop configuration | `6924075c...`: config.ini's FightAspect -1,-1 (native forces 0,0); with 0,0 it is `54f8ae06...` |
| Netplay native <-> native (Linux; macOS) | PASS, dump `a350bc2df11a03519f7d65d278844432` (cvf-online-5, 6,431 frames) |
| Netplay native host <-> Go guest, Go host <-> native guest (`cvf` tag) | PASS, same dump |
| Netplay native <-> Go desktop configuration, both directions | PASS, same dump |
| Netplay with 8 % / 25 % loss including sync (`:sync`), unfixed GGPO | fails in up to half the runs (Go and native alike): GAPS.md |
| Same with the GGPO handshake fix (claude/native-ggpo-sync), also against unfixed builds | 20/20 per pair and loss (Linux; one unrelated TCP-phase failure, GAPS.md) |

### Locations on the project Mac

Under `~/Castlevania Fighter Project/CvF Native Engine/`:

- `dev/` - this repository; `dev/engine/build/release/` the Release build
- `dev-ikemen/` - Ikemen GO tree for regeneration (section 4)
- `dev-refrun/` - reference game dir: cvf-refdata.tgz + `data/Xsound.snd`
  symlink. `rules-cvf-online-2.ini` / `rules-cvf-online-5.ini` are the two
  rules files (copy one over `data/cvf_online.ini`; the -2 file is in place)
- `refrun/statedump.txt` - the recorded reference dump (md5 54f8ae06...)
- `cvf-refdata.tgz` - reference game data; the game itself is in
  `../CvF Online/` (used read-only). `../Castlevania Fighter (v0.8.5b)/` is
  the untouched original: never modify it.

## 4. Regenerating the translated C

The C in `src/core/gen_*.h` and `src/ik/ik_sim.[ch]`, `ik_reflect_stubs.c`,
`ik_unsupported.c` is generated from Go source. Do not edit it by hand.

### Ikemen GO source tree

    tools/gen/setup_ikemen.sh ../../dev-ikemen      # creates or resets it

This clones Ikemen GO, checks out `6a09392`, applies
`tools/ikemen_headless.patch` (the whole CvF fork as one diff; it already
contains `tools/ikemen/*.go`, `tools/ikemen_cvfcook.go` as `src/cvfcook.go` and
`tools/ikemen_statedump.go` as `src/statedump.go`), downloads the modules and
builds `third/` with the module replacements the committed C was generated
with:

| Replacement | Source | Effect on the C |
| --- | --- | --- |
| `github.com/ikemen-engine/ggpo` => `third/ggpo` | in the fork patch itself (also `patches/ikemen-cvf-online.patch`, so every Go build has it); see `third/ggpo/CVF_CHANGES.txt` | **real**: sync handshake timing (retries, disconnect timeout before the peer runs), GAPS.md |
| `github.com/jfreymuth/vorbis` => `third/vorbis` | module + `vita/vorbis-lookup1-fix.patch` | **real**: `lookup1Values` in integer math (OGG music decodes) |
| `github.com/veandco/go-sdl2` => `third/sdl2` | module + `vita/go-sdl2-tinygo.patch` | binding types (syswm/hidapi/AudioCVT removed) |
| `github.com/leonkasovan/gl` => `third/gl` | module + `vita/gl-tinygo.patch` | none (GLES bindings, not translated) |
| `github.com/mewkiz/pkg` => `third/mewkiz-pkg` | plain module copy | none (the Vita tree renamed Syscall there; unreached) |
| `github.com/sqweek/dialog` => `third/dialog` | headless stub (in the script) | none (skipped package) |

These were missing from the repository before (the old scripts pointed at an
`iktg` tree in a deleted scratchpad); they were reconstructed from `vita/`
and `vita/engine-vita-full.patch` (its go.mod hunk). That go.mod also
replaced `golang.org/x/*` with `github.com/golang/*` mirrors of the same
versions (the old container had no network outside GitHub); they are
identical sources and are not needed.

### Run

    IKEMEN_DIR=../../dev-ikemen SDL2_INCLUDE=/usr/local/include/SDL2 \
      tools/gen/gotoc/regen.sh

It builds gotoc and gencdefs into `build/gen/`, regenerates everything and
prints `4727 functions, 0 TODO, 79 stubs`, `ik_sim.c syntax errors: 0`,
`31 unsupported functions` (about 60 s). Then rebuild and rerun the reference
fight. `git status` must show no change in `src/` when nothing was changed.

Notes:

- The Go sources are type-checked as linux/amd64 (`GOTOC_GOOS`/`GOTOC_GOARCH`)
  on every host; the C decides the OS at compile time (`ik_goos()`).
- Output is deterministic and independent of the host: identical with Go
  1.27.3 and 1.27.4, and with the Ikemen tree at a different path. (Before
  2026-11-20 gotoc ordered functions by absolute file path and named local
  types after FileSet offsets, and gencdefs iterated a map; the regenerated
  C differed from the committed one in ordering and numbering only. The
  committed C was regenerated once with the fixed tools; the reference md5s
  are unchanged.)
- **Placeholders fail loudly**: library functions the runtime does not
  implement become zero-value placeholders in `src/ik/ik_unsupported.c`.
  `tools/gen/unsupported_allowed.txt` lists the accepted ones (31); a new one
  stops regen.sh with the list. Implement it in `src/ik`, or accept it with
  `ALLOW_NEW_PLACEHOLDERS=1` (rewrites the list). Functions from the files
  in `GOTOC_EXCLUDE` (Vulkan, ffmpeg video, XM, glTF; `/* stub: */` in
  ik_sim.c, 79) are excluded by design: see `run_reach.sh`.
- `GOFLAGS=-mod=mod GOPROXY=off`: regen.sh never downloads; run
  `setup_ikemen.sh` (or `go mod download` in the tree) first.
