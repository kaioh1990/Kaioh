# Development setup (native engine)

How to get from nothing to a verified build: clone, Ikemen GO source tree,
regeneration of the translated C, CMake build, reference fight. Verified from
scratch on macOS 27 arm64 (Apple M-series, Go 1.27.4 and 1.27.3, Apple clang 21,
CMake 3.31) on 2026-11-20. Linux works the same way (GCC or clang, distro SDL2).
Windows is a cross build from Linux with mingw-w64 ("Windows" in section 3).
iOS is in IOS_PORT.md.

## Branches

**Build from `claude/native-integration`.** It is the one branch with all
the fixes below; the others are its inputs and are not updated with each
other's work.

| Branch | What it adds | In integration |
| --- | --- | --- |
| `claude/native-gaps-impl` | base: the native engine with the gap fixes | yes |
| `claude/native-netplay` | online play, rollback fixes, netplay test | yes |
| `claude/native-windows` | mingw-w64 build; collector scans suspended fibers, frees finished ones; `go_cstr_copy` (all platforms); Windows Nearby, crypto/rand | yes |
| `claude/native-ios` | iOS port: GLES renderer, engine stack, GC roots and guard pages, NSURLSession HTTP, Nearby bridge | yes |
| `claude/native-ggpo-sync` | GGPO sync handshake that survives packet loss (`third/ggpo` in the fork patch) | yes (7439707) |
| `claude/native-android` | Android port | only its collector fix (registers spilled in clear, fe415a0); the Android platform layer and app stay on that branch |
| `claude/native-netplay2` | netplay: pre-match TCP race fix, content fingerprint, several matches per session, roster sweep scripts (all Go changes in both fork patches) | yes |
| `claude/native-vita` | PS Vita port M1 (VITA_PORT.md): armv7 build under qemu-arm, vitasdk build and headless VPK, 32-bit runtime fixes (ARM context switch, arena placement, map hashes, derived pointers, base-10 formatting) | yes |
| `claude/native-vita2` | PS Vita port M2 (VITA_PORT.md sections 5, 11): `cvfVitaGL` run-time switch in the fork (Vita shaders and vitaGL workarounds), the game on the Vita (`cvf_vita`, `cvf-native-m2.vpk`), frame-time logs, GLES render thread, `cvf_vitasim`/`gles_thread_test`, the `boundTexture` fix | no (from d290761) |
| `claude/native-memory` | collector memory and pauses (GAPS.md): `clear(map)` releases its entries, lazy sweeping, prefetching and parallel marking, fight pacing max(trigger, 96 MB), free pages returned to the OS, a real `sync.Pool`; `CVF_GC_WHY` retention diagnostics | no (from a14596e) |

Do not merge the input branches into each other; merge them into
`claude/native-integration` and regenerate the C there (section 4).

On `claude/native-netplay2` (2026-11-21, Linux x86-64, Go 1.27.4): regen
4858 functions, 0 TODO, 79 stubs, 31 placeholders, reproducible from a
fresh `setup_ikemen.sh` tree; `cvf_tests` 93/0; reference fights
`54f8ae06...` / `4334350c...`; netplay dump `a350bc2d...` (also as match 1
of every session); the new netplay scripts below.

`claude/native-cpu2` (2026-11-21, Linux x86-64, Go 1.27.4; CPU round 2,
VITA_PORT.md section 3): regen from a fresh `setup_ikemen.sh` tree
reproduces the committed C (4872 functions, 0 TODO, 79 stubs, 31
placeholders); `cvf_tests` and `cvf_tests_ucontext` 103/0 on x86-64,
armv7 and aarch64 (qemu), `cvf_tests` 103/0 under Wine (no ucontext build
on Windows); reference fights `54f8ae06...` / `4334350c...` on x86-64,
Windows (Wine), armv7, aarch64, GC stress (`CVF_GC_STRESS=5000`, ~1000
collections) and ASan/UBSan (only the known `memset(NULL, 0, 0)` of
`StoryboardManager.clear`); `run_netplay.sh` PASS with `a350bc2d...` (the
three default scenarios; a three-match session, same three dumps as
0b04ded's build; a Go `cvf_headless_go` host from 0b04ded's tree against
a native guest).

`claude/native-cpu3` (2026-11-22, Linux x86-64, Go 1.27.4; CPU round 3,
VITA_PORT.md section 3): regen from a fresh `setup_ikemen.sh` tree
reproduces the committed C (4884 functions, 0 TODO, 79 stubs, 31
placeholders; `src/core/opcodes.h` from `gen_opcodes.py`); `cvf_tests` and
`cvf_tests_ucontext` 103/0 on x86-64, armv7 and aarch64 (qemu), `cvf_tests`
103/0 under Wine; reference fights `54f8ae06...` / `4334350c...` on x86-64,
Wine, armv7, aarch64, GC stress (`CVF_GC_STRESS=5000`) and ASan/UBSan (only
the known `memset(NULL, 0, 0)` of `StoryboardManager.clear` and `.step`),
and with `CVF_SC_CHECK=1 CVF_IDX_CHECK=1`; `run_netplay.sh` PASS with
`a350bc2d...` (the three default scenarios, a Go `cvf_headless_go` host
against a native guest, and a three-match session with the same three
dumps as before). The netplay dumps need a game directory with the
`cvf-online-5` rules file (`refgame5`): with `cvf-online-2` (the plain
cvf-refdata.tgz) native pairs play a different match (`7ecbd3bd...`,
6,159 frames) and a Go host refuses a native guest ("effective fight
aspect differs (local=stage remote=resolution:16:9)"), because native
builds force `FightAspect` 0,0 (`config.go`, `vitaPerf`) while the Go build
keeps the ini's -1,-1; the cvf-online-5 rules set 4:3 on both. Same with
cb9a781's build, so not a change of this branch.

`claude/native-cpu4` (2026-11-22, Linux x86-64, Go 1.27.4; CPU round 4,
VITA_PORT.md section 3): regen from a fresh `setup_ikemen.sh` tree
reproduces the committed C (4901 functions, 0 TODO, 79 stubs, 31
placeholders; `src/core/opcodes.h` from `gen_opcodes.py`, now with opcodes
203-208); `cvf_tests` and `cvf_tests_ucontext` 103/0 on x86-64, armv7 and
aarch64 (qemu), `cvf_tests` 103/0 under Wine; reference fights
`54f8ae06...` / `4334350c...` on x86-64, Wine, armv7, aarch64, GC stress
(`CVF_GC_STRESS=5000`) and ASan/UBSan (only the known `memset(NULL, 0, 0)`
of `StoryboardManager.clear` and `.step`), and with `CVF_SC_CHECK=1
CVF_IDX_CHECK=1`; `run_netplay.sh` PASS with `a350bc2d...` (the three
default scenarios; a three-match session with the same three dumps as
rounds 2 and 3, `a350bc2d...`, `f9e74b10...`, `c088c178...`; a Go host
against a native guest, both 0b04ded's `cvf_headless_go` and one built
from this branch's fork tree with `-tags cvfgohead`, since `-tags
cvf,cvfgohead` no longer builds: "Go build of the headless runner" in section 3). The online fork with this
branch's change compiles with `go build ./src`.

`claude/native-vita2` (2026-11-22, Linux x86-64, Go 1.27.4; Vita M2,
VITA_PORT.md section 11): regen from a fresh `setup_ikemen.sh` tree
reproduces the committed C (4902 functions, 0 TODO, 79 stubs, 31
placeholders; three new native functions in `GOTOC_NATIVE_FUNCS`);
`cvf_tests` and `cvf_tests_ucontext` 103/0 on x86-64, armv7 and aarch64
(qemu), `cvf_tests` 103/0 under Wine; reference fights `54f8ae06...` /
`4334350c...` on x86-64, Wine, armv7 and aarch64; `run_netplay.sh` PASS with
`a350bc2d...`; `go build -tags cvf,cvfgohead` builds again and plays
`54f8ae06...`; Vita `cvf_vita` and `cvf-native-m2.vpk` build; in vemu, the
VPK image's headless mode gives 103/0 and `54f8ae06...`. When a container
reaches only GitHub, `setup_ikemen.sh` needs `go mod edit -replace` lines
that point golang.org/x/* and gopkg.in/ini.v1 at their GitHub mirrors
(github.com/golang/*, github.com/go-ini/ini), at the same versions, after
the patch is applied. The output is unchanged.

Integration of netplay2 and vita (2026-11-21, Linux x86-64, Go 1.27.4):
regen from a fresh `setup_ikemen.sh` tree reproduces the committed C (4858
functions, 0 TODO, 79 stubs, 31 placeholders); `cvf_tests` and
`cvf_tests_ucontext` 103/0 on x86-64, Windows (Wine), armv7 and aarch64
(qemu); reference fights `54f8ae06...` / `4334350c...` on x86-64 (also
GC stress and ASan/UBSan), Windows (Wine), armv7 and aarch64 (qemu);
`run_netplay.sh` PASS with dump `a350bc2d...` (defaults, `:sync`, `tcp=`,
a three-match session, Wine host to Linux guest); `run_content.sh` and
`run_prematch.sh` (20 runs, LOAD=2) PASS; Vita ELF and VPK build with
vitasdk 2026.08. macOS/iOS were not rebuilt (no Apple toolchain in the
container).

Earlier, on the integration branch (2026-11-21): regen gives 4842
functions, 0 TODO, 79 stubs, 31 placeholders and is reproducible;
`cvf_tests` 93/0 and `cvf_nearby_test` on Linux, macOS arm64 and Windows
(Wine); reference fights `54f8ae06...` / `4334350c...` on Linux, macOS and
the iOS Simulator; `run_netplay.sh` PASS with dump `a350bc2d...` for Linux
pairs (defaults plus `20:10:25:sync`, four relay seeds) and Windows (Wine)
host to Linux guest; signed `CvF-Native.ipa` rebuilt (IOS_PORT.md).

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
    If you copy those dylibs elsewhere for `CVF_SDL2_LIBRARY`, copy
    `libSDL3.0.dylib` also as `libSDL3.dylib` beside them: sdl2-compat
    dlopens the unversioned name, and without it every SDL-linked tool
    (`cvf_headless` too) stops at start in a modal "SDL3 not found" dialog
    (0 % CPU, no output). `cvf_tests` does not link SDL and still runs.

## 2. Repository

    git clone -b claude/native-integration https://github.com/kaioh1990/Kaioh.git dev
    cd dev/engine

On the project Mac this is `~/Castlevania Fighter Project/CvF Native Engine/dev`.

## 3. Build and reference test (no Go needed)

    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCVF_SDL2_INCLUDE_DIR=/usr/local/include/SDL2 \
      -DCVF_SDL2_LIBRARY="$HOME/Castlevania Fighter Project/CvF Online/CvF Native (Apple Silicon).app/Contents/Frameworks/libSDL2-2.0.0.dylib"
    cmake --build build/release -j10      # cvf_headless, cvf, cvf_tests, cvftool
    ./build/release/cvf_tests             # 100 checks, 0 failures
    ./build/release/cvf_tests_ucontext    # Linux: the same with the ucontext scheduler (below)

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
TCP 7650, UDP 7651-7654, TCP relay 7655), `vita_statedump.txt` and `vita_statehash.txt`
(fixed seed and pre-match time, reproducible runs) and ButtonAssist off (it
keeps state per input poll, not per frame). `HOST_BIN` / `GUEST_BIN` choose
the engines, e.g. a Go build (below). Debugging: `GGPO_LOGS=1` (GGPO's trace
in each instance's `save/logs`), `RELAY_LOG=1` (one line per packet with the
GGPO header), `RELAY_SEED`, `TIMEOUT`, `PORT_BASE`, `SEED_HOST`/`SEED_GUEST`,
`GC_TRACE=1` (the native collector's lines in each `out.txt`).

More options (claude/native-netplay2):

- scenario option `tcp=D/J` (e.g. `lag:40:15:0:tcp=30/20`): the netplay TCP
  connection (sync settings, seed, delay-netplay input, stop markers) also
  goes through `tests/netplay/tcp_relay.py` with D ± J ms, in order;
- `MATCHES="P1:P2[:PAL1:PAL2] ..."`: more matches in the same online
  session after the first (`CVF_NET_MATCHES` of `cvfheadless.go`): as
  main.lua's netplay loop, the game function returns and resynchronizes,
  the character select runs `SELECT_FRAMES` (60) delay-netplay frames, the
  next pair is loaded (a new RollbackSession, the same UDP ports) and
  played; the same pair again is a rematch. Every match's dump is compared
  (`statedump.K.txt` for match K > 1), and each side prints its RSS after a
  full collection at the end of every match (`session: match K done`);
- `LOAD=N`: N busy loops beside the engines (a loaded machine);
- `GUEST_GAME`: the guest plays from another game directory;
- `NODUMP=1` (memory figures without the dump buffer; only winners are
  compared), `CLEAN=1` (delete dumps once compared: a long session writes
  tens of MB per match and side).

The last `TAIL` (8) frames of each match are compared separately: they can
be simulated with predicted inputs that are never rolled back, because the
rollback loop ends with the match (GAPS.md); a difference there is
reported, not failed.

Three more scripts use the same instances:

    tests/netplay/run_prematch.sh /path/to/refgame 100   # "prematch: PASS"
    tests/netplay/run_content.sh /path/to/refgame        # "content: PASS"
    tests/netplay/run_roster.sh /path/to/game            # "roster: PASS"

`run_prematch.sh` repeats only the TCP part of a match start (connect,
handshake, delay-netplay frames, loading, the second Synchronize) RUNS
times and stops each run at "Rollback startup" (~4 s per run); `LOAD=3`
and `TCP=D/J` make it harsher. Builds without the netplay.go race fix
(GAPS.md) fail a few percent of runs with `LOAD=3` on two cores.
`run_content.sh` runs the guest from modified copies of the game directory
(gameplay rules, lobby-only rules lines, a common file, a character in use,
the stage, an unused extra character, LF line endings) and checks that the
content fingerprint stops the session with "Game data mismatch" exactly when
the change matters; with `OLD_BIN` (a build without the fingerprint) every
case also runs old host - new guest and new host - old guest and must play.
`run_roster.sh` runs sessions over character pairs (each character against
the next one, both ways; `CHARS`, `PAIRS`, `PER_SESSION`, `SEEDS`, `STAGE`,
`ROUNDS`) so the whole roster can be swept on a machine with the full game.

### Collector checks: stress mode, sanitizers, ucontext scheduler

**GC stress mode.** A build with `-DCVF_GC_STRESS=ON` (CMake option; it
defines `CVF_GC_STRESS` for `core/gc.c`) reads the environment variable
`CVF_GC_STRESS=N` at the first allocation and then runs a full collection on
every Nth allocation, wherever it happens (inside the simulation, a goroutine,
a Lua call), and fills every freed object, free page and freed large object
with `0xA5`. A missed root (a pointer the collector did not see) then reads
garbage at once instead of surviving by luck until the memory is reused.
Unset or `0`: no extra collections (the poisoning still applies to normal
collections). Normal builds contain none of this code. The simulation must
not notice the collections: the dumps stay bit-identical.

    cmake -S . -B build/stress -G Ninja -DCMAKE_BUILD_TYPE=Release -DCVF_GC_STRESS=ON
    cmake --build build/stress
    cd refgame
    CVF_GC_STRESS=5000 CVF_GC_TRACE=1 ../build/stress/cvf_headless . -p1 Alucard -p2 Richter \
        -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1 2>gc.log
    md5sum statedump.txt                       # same md5 as the Release build
    grep -c '^gc [0-9]' gc.log                 # collections (about 2,000 per match at 5000)
    # netplay, both peers the stress build (absolute paths: each peer runs in its own dir):
    CVF_GC_STRESS=20000 HOST_BIN=$PWD/build/stress/cvf_headless GUEST_BIN=$PWD/build/stress/cvf_headless \
        PORT_BASE=14100 tests/netplay/run_netplay.sh refgame5 direct loss:20:10:8

Smaller N is slower (each collection scans the whole heap: at N=5000 a
reference match took ~2.5x as long on Linux x86-64). `CVF_GC_TRACE=1` prints one line per collection
(live set, pages in use, high-water mark, pause with its thread CPU time,
bytes returned to the OS so far).

Collector settings (environment; claude/native-memory): `CVF_GC_THREADS=N`
markers during a pause (default half the CPUs, at most 4; 1 = sequential;
none on the Vita), `CVF_GC_RELEASE=0` keeps free pages resident,
`CVF_GC_FIGHT_BUDGET_MB` (96 / 80 MB on 64 / 32 bits) and `CVF_GOGC`
(100) set the pacing (core/gc.h). Stress and sanitizer runs should also
be made with `CVF_GC_THREADS=2` so the parallel marker is exercised on a
2-CPU machine.

**Sanitizers** (gcc; LSan reports the collector's own mark/run arrays, which
live in mmap'd memory it does not scan, so leak detection is off):

    CC=gcc cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    cmake --build build/asan
    ASAN_OPTIONS=detect_leaks=0 build/asan/cvf_tests
    unshare -rn sh -c 'ip link set lo up; ASAN_OPTIONS=detect_leaks=0 build/asan/cvf_nearby_test /tmp/nt'

(`unshare -rn`: a private loopback, so the fixed UDP port 7520 of Nearby is
free even when other runs share the machine.) The only UBSan report is
`memset(NULL, 0, 0)` in generated StoryboardManager code (harmless).

**ucontext scheduler.** x86-64 and arm64 switch goroutines with `ctx_switch`
(assembly), Windows with fibers; every other POSIX target (32-bit ARM, i686)
uses `swapcontext`. `-DCVF_SCHED_UCONTEXT` forces that fallback on x86-64 /
arm64 too: on Linux, `cvf_tests_ucontext` is `cvf_tests` linked against a
core built that way, so the fallback is tested on every Linux build.
`tests/test_gc_roots.c` collects from goroutines while main and another
goroutine are suspended (before 2026-11-21 the ucontext path scanned no
suspended stack at all). A 32-bit ARM check without hardware:

    sudo apt-get install gcc-arm-linux-gnueabihf qemu-user
    # compile src/core/*.c src/formats/*.c into a static archive, link tests/*.c, then
    qemu-arm -L /usr/arm-linux-gnueabihf ./cvf_tests

(there, test_sched.c's suspended-goroutine check fails by design of
conservative scanning: GCC keeps only `data - 4` live across the call for a
pre-indexed loop, a pointer below the object; see GAPS.md.)

Goroutine stacks and the iOS engine stack have a 64 KB `PROT_NONE` guard
below them, and the build adds `-fstack-clash-protection` where the compiler
accepts it (not on Windows, which probes large frames itself). An overflow is
a plain crash (no handler on an alternate stack).

### Go build of the headless runner (cross-checks)

`tools/ikemen/cvf_gohead.go` (build tag `cvfgohead`) runs the same
`cvfHeadlessMain` from Go, so the Ikemen tree from section 4 builds a Go
`cvf_headless_go` that takes the same arguments and writes the same dumps:

    cd dev-ikemen
    go build -tags cvf,cvfgohead -o cvf_headless_go ./src   # native's configuration (vitaPerf)
    go build -tags cvfgohead -o cvf_headless_go_desktop ./src  # the shipping PC/mobile configuration

(From `claude/native-cpu4` until `claude/native-vita2` the first one failed:
the `cvf` tag selects both render_gl33.go and render_gles32.go, which both
declared `boundTexture`. gotoc ignores type errors, so regen did not show
it. The GLES32 type is now `boundTextureGLES32`, so both configurations
build. With `-tags cvf,cvfgohead` the reference fight gives `54f8ae06...`.
On Linux: `CGO_CFLAGS=-I/usr/include/SDL2 CGO_LDFLAGS=-lSDL2`, and
PKG_CONFIG_PATH pointing at an FFmpeg 7.)

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
| `run_prematch.sh` 100 runs, `LOAD=3`, Go host - native guest (claude/native-netplay2) | unfixed 4/100 and 2/60 "Synchronization error"; with the netplay.go fix 100/100 (GAPS.md) |
| `run_content.sh` native - native, Go - native, native - Go, each with old builds on either side | PASS: "Game data mismatch" exactly for rules / data / character in use / stage; plays with lobby-only rule lines, an unused extra character, LF endings, and whenever one side is old |
| Sessions (`MATCHES`, 6-16 matches), native - native, direct and 8 % loss + TCP lag | every match identical on both sides; memory in GAPS.md |
| 10-match session, Go - Go (`NODUMP=1`) | same winners as native in all 10; heap 52-63 MB, RSS 165-220 MB |
| Reference fight cvf-online-5, Go desktop configuration, with the fingerprint | `4334350c...` (unchanged) |

### Windows (cross-build with mingw-w64, checks under Wine)

The Windows build is a cross build from Linux with mingw-w64 and the
toolchain file `tools/package/mingw64.cmake`; there is no MSVC build (the
translated C uses GNU statement expressions and `typeof`). Verified on
Ubuntu 24.04 x86-64 on 2026-11-20 with Ubuntu's mingw-w64 (GCC 13, win32
threads, msvcrt) and the SDL2 2.30.9 MinGW development package. **Only
under Wine 9.0**: no build has run on real Windows yet (GAPS.md).

    sudo apt-get install gcc-mingw-w64-x86-64 wine64     # wine64 only for the checks
    curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz
    tar xzf SDL2-devel-2.30.9-mingw.tar.gz
    S=$PWD/SDL2-2.30.9/x86_64-w64-mingw32
    cmake -S . -B build/win64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=tools/package/mingw64.cmake \
      -DCVF_SDL2_INCLUDE_DIR=$S/include/SDL2 -DCVF_SDL2_LIBRARY=$S/lib/libSDL2.dll.a
    cmake --build build/win64 -j8    # cvf.exe, cvf_headless.exe, cvf_tests.exe, cvf_nearby_test.exe, cvftool.exe
    cp $S/bin/SDL2.dll build/win64/

(In a container whose apt sources are `http://` and whose network only
allows HTTPS through a proxy: switch the sources to `https://archive.ubuntu.com`
and set `Acquire::https::Proxy`.) The executables need only system DLLs
plus `SDL2.dll`, which ships next to `cvf.exe`; `cvf` and `cvf_headless`
link libgcc statically. They link ws2_32, iphlpapi (Nearby interfaces),
winhttp (lobby), bcrypt (crypto/rand) and winmm. The same determinism flags
apply (`-ffp-contract=off`, detmath, 32-bit `int`). A UCRT toolchain
(llvm-mingw) should work too but was not tried.

Checks under Wine (64-bit only; the "wine32 is missing" notice is harmless).
Wine 9.0 aborts at start with `free(): invalid pointer` when
`$XDG_RUNTIME_DIR` (or `/run/user/<uid>`) does not exist:

    export WINEPREFIX=/tmp/wineprefix WINEDEBUG=-all XDG_RUNTIME_DIR=/run/user/$(id -u)
    mkdir -p -m 700 $XDG_RUNTIME_DIR
    W=/usr/lib/wine/wine64
    cd build/win64
    $W ./cvf_tests.exe                                  # 100 checks, 0 failures
    $W ./cvf_nearby_test.exe 'Z:\tmp\nt'                # 0 failure(s) (UDP 7520 free)
    $W ./cvf_nearby_test.exe 'Z:\tmp\nt' ifaces         # interfaces as Nearby sees them

Nearby across platforms: `cvf_nearby_test DIR host 15` in one engine and
`cvf_nearby_test DIR scan 7555 Tester` in the other (Wine and Linux native,
both directions). Netplay with a Windows side: a wrapper for
`run_netplay.sh`'s `HOST_BIN`/`GUEST_BIN`,

    #!/bin/bash
    export WINEPREFIX=/tmp/wineprefix WINEDEBUG=-all XDG_RUNTIME_DIR=/run/user/0
    exec /usr/lib/wine/wine64 /path/to/engine/build/win64/cvf_headless.exe "$@"

    HOST_BIN=./wine_headless.sh GUEST_BIN=build/release/cvf_headless \
      PORT_BASE=9670 TIMEOUT=900 tests/netplay/run_netplay.sh /path/to/refgame

(`PORT_BASE`: pick free ports when other runs share the machine; two runs on
the same ports cross their packets.) Results (2026-11-20, Wine 9.0 on
Ubuntu 24.04 x86-64; refgame with cvf-online-5):

| Check | Result |
| --- | --- |
| `cvf_tests.exe` | 93 checks, 0 failures on the integration branch (92 before gc_selftest; before the fiber fix: 1 failure, GC freed data on a suspended goroutine's stack) |
| `cvf_nearby_test.exe` (one process, host + scan) | 0 failures |
| Nearby host Wine ↔ scan Linux native, both directions | host listed |
| `ifaces` with a /23 added on eth0 and a tun device | /23 → 510 probe targets (was a fixed /24); the tun device (Wine: IF_TYPE_PPP) named `ppp tun97`, skipped |
| Netplay Windows host (Wine) ↔ Linux guest, direct / lag / loss | PASS, dump `a350bc2df11a03519f7d65d278844432` (= Linux ↔ Linux) |
| Netplay Linux host ↔ Windows guest (Wine), direct / lag / loss | PASS, same dump |

`cvf.exe` (window, OpenGL, menus) was built but not run: the container has
no display or GL, and the full game is not there.

Windows Firewall: the first time a program listens, Windows asks whether to
allow it; if the player declines, inbound connections are dropped silently
and a host just waits. When a TCP listener (Host online / Host Nearby) has
had no guest for 30 s, `ik_net.c` (Windows only) prints a one-time hint per
listener to the console (`cvf.exe` is a console program) with the steps to
allow the game in Windows Security and the exe's path. It changes nothing
else: no firewall rules (they need admin), no game state (cvf_tests 93/0 and
the netplay dump `a350bc2d...` unchanged, Wine host <-> Linux guest PASS).

### Desktop packages in the CvF Online folder

The double-clickable builds live in `CvF Online/` next to the game data:
`CvF Native (Apple Silicon).app` and `CvF Native (Windows x64).exe`.
Last refreshed 2026-11-21 from `claude/native-integration`.

macOS: build Release as above, then

    tools/package/macos_app.sh build/release SDL_DIR "CvF Native (Apple Silicon).app" icon.icns

with `SDL_DIR` holding the arm64 `libSDL2-2.0.0.dylib` (sdl2-compat) and
`libSDL3.0.dylib`, and `icon.icns`, all taken from the previous app before
replacing it. The script writes the Info.plist (`net.cvf.native`) and signs
ad hoc. The app finds the game data by changing to the folder that holds
the .app, twice: in `game_main.c` (only without a gamedir argument) and in
Ikemen's own `System.init` (`isRunningInsideAppBundle`, always, after
`save/config.ini` is read). So the bundled binary always plays from the
folder of its .app, even when given a gamedir or started through a symlink
(the executable path is resolved). To run it on another folder (smoke
test), put a copy of the .app in that folder; for `run_smoke.sh`, create
WORKDIR with the .app copy inside and give a BUILDDIR whose `cvf` is a
wrapper `exec "$PWD/CvF Native (Apple Silicon).app/Contents/MacOS/cvf"`
(the script starts it in WORKDIR; its `*.app` exclusion leaves the copy
alone). The reference fight needs `cvf_headless` (the game binary loads
the full menus); run it from the same build: the bundled `cvf` has the
same text section as `build/release/cvf` (`otool -t`), only its load
commands and signature differ.

Windows: `build/win64/cvf.exe` renamed to `CvF Native (Windows x64).exe`.
It imports only system DLLs plus `SDL2.dll`. The `SDL2.dll` already in
`CvF Online/` is SDL 2.32.10 (shipped for the Go `Ikemen_GO.exe`); it
exports all 52 SDL functions the exe imports, and the exe loads with it
under Wine, so it stays (newer SDL2 2.x is ABI-compatible with the 2.30.9
headers used to build). The other DLLs there (FFmpeg, libxmp) belong to
the Go build; the native exe does not use them.

Replaced builds go to `CvF Online/_to_delete/` (moved, not deleted).

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
