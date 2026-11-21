# Porting notes (living document)

## Approach

The C engine is a faithful port of Ikemen GO's simulation, so reference
fights recorded with Ikemen replay tick-for-tick. Ikemen's *compiler* is not
ported: Ikemen itself is the content cooker. `tools/ikemen_cvfcook.go`
(added to the Ikemen tree as `src/cvfcook.go`) writes each compiled
character to `cooked/<def>.cvfc` right after compilation, before any
platform-specific transformation. The C engine loads these
(`core/cooked.c`) and runs the exact bytecode Ikemen produced. A later
phase compiles the cooked bytecode ahead of time to C.

Cooking currently runs inside the Vita reference build in the emulator
(`vita_cook.txt` flag): about 5 seconds per character.

## Translation tooling (since 2026-11-19)

Hand-porting ~50k lines of Ikemen's simulation is error-prone, so the bulk
is translated mechanically and then fixed up by hand:

- `tools/gen/gencdefs`: generates the C data model (structs, slices, maps,
  constants) from Go type information. Go arrays become struct wrappers
  (`ArrN_T { T v[N]; }`) to keep value semantics.
- `tools/gen/gotoc`: translates Go function bodies to C using go/types:
  methods become `Type_method(recv, ...)`, package functions `fn_Name`,
  package variables `g_name`, integer division/shift/float->int
  conversions go through helpers with Go (amd64) semantics. Constructs it
  cannot translate are emitted as `TODO(/* what at file:line */)` so the C
  compiler flags them.
- First run: char.go 452 functions / 144 TODO, bytecode.go 312 / 52,
  system.go 228 / 153, anim.go 74 / 32.

After closures were added (lifted to static functions with an environment
of pointers to captured variables), remaining TODO sites: char.go 13,
bytecode.go 14, system.go 50, anim.go 3, common.go 8, input.go 2,
fightscreen.go 11.

### Plan from here (decided 2026-11-19 00:45)

Translate *all* simulation code consistently, including Ikemen's CNS
compiler (compiler.go, compiler_functions.go), instead of mixing generated
code with hand-written replacements:

1. gotoc gains reachability: start from a headless fight runner's roots
   (load characters and stage, run match ticks) and translate only reached
   functions; unreached callees become stubs. Render/audio/platform types
   are overridden to opaque handles.
2. gencdefs generates the data model from `System` (sys) downward with
   the same overrides.
3. A small C runtime (core/gort.c) provides slices, maps (deterministic
   insertion order), strings, interface values and the Go-semantics
   arithmetic helpers.
4. Compile, fix, then replay the reference fight and compare per-tick
   dumps with the Ikemen build.
5. Then optimise: arenas + memcpy rollback, AOT script compilation, render
   thread, native platform layers.

The hand-written modules (rng, detmath, anim, values, AIR, cooked content)
stay as verified references and for the optimised paths later.

### Status 2026-11-19 01:10

- gencdefs ALL: the entire Ikemen data model (388 structs) compiles as C
  (src/core/gen_types.h). Embedded structs: `union { T T_; struct {...}; }`.
  C-keyword field names get a `_` suffix (cField).
- gotoc reach (roots Loader.load, System.runMatch, System.resetRound,
  System.loadStart; excluded render/menus/platform/vita-perf files):
  1789 functions, 147 TODO, 176 stubs, ~78k lines.
- First compile of the translation: 9302 errors. Next translator work:
  1. package-level variables (g_sys etc.) + initializers/init();
  2. interface values: type ids, boxing conversions, per-method
     dispatchers, type switches/assertions (StateController, error);
  3. Go std shims: strings, strconv, sort, fmt (Sprintf with %v/%d/%s/%f),
     filepath, encoding/binary, regexp (few);
  4. `reflect` uses (Value_*) in state_clone/compiler: stub or rewrite.

### Status 2026-11-19 02:10 — first full translated match runs

- gotoc now translates everything the headless match reaches: interfaces
  (type ids, switch dispatchers, type switches/assertions), generics
  (monomorphised), defer, method values, closures (captured and
  address-taken locals live on the heap), select (single-threaded: default
  clause), go-ini via a C port (ik_ini.c), reflect over generated type
  descriptors (ik_reflect.c), fmt/strconv/strings/sort/regexp/os/io shims.
- Type-checking runs in module mode with Go 1.27, so pure-Go dependencies
  resolve; platform packages (SDL, GL, audio) stay opaque.
- Entry point: `cvfHeadlessMain` (tools/ikemen_cvfheadless.go, copied into
  the Ikemen tree) replays main.go + main.lua's command-line quick match +
  the Lua `game` function. `src/app/headless_main.c` runs it:
  `cvf_headless <gamedir> -p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 -s
  stages/ParanoiaBG.def -rounds 1`.
- 1897 reachable functions, 0 compile errors, links, runs the whole match.
- Reference comparison (statedump, Alucard vs Richter AI 8, seed 12345):
  **all 287,545 lines (5,022 ticks) of the recorded reference are
  identical** - characters, helpers, explods, vars, positions as IEEE bits.
  (The reference recording stops at 185 s of emulation; a longer one is
  being recorded to check the round end and round 2.)
- Speed of the plain translation (x86-64, -O2, no optimisation work yet):
  1.38 ms per tick, 1.4 s load. Allocation-heavy (heap-boxed locals,
  maps for vars, malloc everywhere) - the optimisation phase starts from
  here.

### Status 2026-11-19 03:30 — match end, memory

- motif.go is translated: the match now ends exactly like the reference
  (Motif.act clears postMatchFlg, Lua exits) and the process exits with the
  winner. Still all reference lines identical.
- Memory: the translation never freed anything (peak RSS 1.23 GB for one
  match). Now every allocation goes through `core/gc.c`, a conservative,
  non-moving mark-sweep collector (8 KiB size-classed pages, interior
  pointers, roots = data/bss + stack + registers; Linux, macOS and Windows
  root discovery). Peak RSS 78 MB, live set ~30 MB.
- Pointer-free storage is allocated "atomic" (never scanned): gotoc and
  gencdefs decide per type (`pointerFree`) and emit GO_MAKE_A / go_box_a /
  go_new_atomic / GO_SLICE_FUNCS_A.
- gotoc escape analysis (lite): closures that are only called (directly or
  through a local used only in call position) get their environment on the C
  stack, and variables only they capture are stack boxes instead of heap
  boxes.
- Per-tick garbage: 43 KB → ~5 KB (closure boxing, explod Animation
  recycling now always on — flagForRemoval keeps the object for the next
  explod —, DisplayToClipboard/AppendToClipboard only build text while the
  debug display is on, float literal decoding without a temporary slice,
  EnemyNear scratch array, in-place map deletes).
- Collection policy: `System.await` calls `cvfGcSafepoint(fighting)` every
  frame. While a round is in progress (roundState 2) allocation does not
  trigger collections unless the fight budget is exceeded
  (CVF_GC_FIGHT_BUDGET_MB, default 256 MB 64-bit / 48 MB 32-bit); due
  collections run at frame boundaries outside fights. In the reference match
  all collections happen during loading and after the KO, none in the fight.
- Diagnostics: CVF_GC_TRACE=1 (per collection + summary), CVF_GOGC,
  CVF_GC_ARENA_MB; build with -DGC_PROFILE for allocation-site sampling
  (gcprof.txt, CVF_GC_PROF_SKIP_MB to skip the load).
- CMake: `cvf_ik` (translated simulation) and `cvf_headless` targets.
- **Cross-platform determinism verified**: the CMake Release build on macOS
  arm64 (Apple M1 Max, Apple clang 21) produces a statedump byte-identical
  to Linux x86-64 (GCC) for the whole match (md5 54f8ae06…, 287,752 lines).
  The collector's macOS root discovery works (peak RSS 127 MB there).
  Speed: ~0.85 ms/tick x86-64 (container), ~1.0 ms/tick M1 Max.
- Sanitizer builds: ASan's "fake stack" hides locals from the conservative
  collector; gc.c sets `detect_stack_use_after_return=0` via
  `__asan_default_options` in ASan builds.

## Conventions

- Determinism flags in CMakeLists.txt: no FMA contraction, no fast-math,
  wrapping signed arithmetic (Go semantics).
- No libm transcendental calls in game logic: core/detmath.
- Go constant expressions involving irrationals are written as exact hex
  floats (Go folds them at arbitrary precision).
- Every port is checked against Go golden output (tests/golden/*.go) or
  against reference dumps from the Ikemen build.

## Status

| Part | Source (Ikemen) | State |
| --- | --- | --- |
| RNG | common.go | done, bit-exact |
| Atoi/Atof/INI | common.go | done, bit-exact |
| Det. math | cvf_detmath.go | done, bit-exact (amd64 + arm64) |
| AIR | anim.go | done, all 124 CvF AIR sources identical |
| Cooked content loader | cvfcook.go | done, 0 unknown controllers for Alucard/Richter |
| Bytecode VM | bytecode.go | next |
| Char runtime | char.go | |
| Controllers | bytecode.go | |
| System / fight loop | system.go | |
| Commands / input | input.go | |
| Sprites (SFF) | image.go | (render phase) |

### Status 2026-11-19 06:00 — build configuration `cvf`, Lua VM translated

- gotoc translates several Go packages (GOTOC_PKGS): the Lua VM
  (gopher-lua and its parser) is translated with the game. cgo binding
  packages (GOTOC_BIND_PKGS: go-sdl2, the GL bindings) are type-checked
  for their signatures and real struct layouts; their functions are
  implemented by hand in C.
- New build tag `cvf` in the Ikemen fork (tools/ikemen/cvf_engine*.go):
  the native engine uses the desktop platform files (SDL window, GL 3.3
  renderer) and `vitaPerf` (= isVita || cvfEngine) keeps the Vita port's
  simulation-equivalent performance paths (var cache, bytecode fusion,
  explod/SpriteData reuse, synchronous gfx tasks) in every native build.
  Platform quirks (vitaGL, memory-card file system, 30 fps lock, TinyGo GC
  workarounds) stay behind `isVita`.
- The headless translation under `cvf` reproduces the Vita reference dump
  byte for byte — once the fight aspect matches.
- **Finding: the fight aspect is simulation state.** The Vita build forces
  FightAspectWidth/Height = 0,0 (full-panel fights) while the PC default is
  -1,-1 (stage-aspect letterbox); with -1 the same match diverges at tick
  ~12 (a helper's float var, screen-relative). Every native build now uses
  0,0. Existing Go builds that cross-play (CvF Online PC vs Vita) are
  exposed to the same desync when settings differ.
- The non-vita Go build of this fork did not compile (itoa, vitaHalfTick
  only in the Vita file); fixed.
- Embedded structs: fields Go does not promote (shadowed or ambiguous)
  keep their layout slot under a private name (`_sh<i>_<name>`).

## Status 2026-11-19 07:30 — the native game runs (Linux, macOS arm64)

- The whole game is translated (4,352+ functions): `cvf` opens the window,
  runs the Lua menus (intro storyboard, title, main menu) and plays matches;
  `cvf_headless` still reproduces the reference match (md5 54f8ae06…) on
  Linux x86_64 and macOS arm64.
- macOS: built with Apple clang for arm64 against the SDL2 bundled in
  "CvF Online (Apple Silicon).app"; OpenGL 4.1 (Metal) renders the game.
- Runtime pieces added for the full game: panic/recover (setjmp frames per
  goroutine), go:embed, interface-keyed maps, image/png + stb_image,
  image/draw, encoding/json over reflect, fmt Error()/String(),
  sort.Sort on concrete types, time.Sleep/runtime.Gosched yielding.
- Known gaps: placeholders for net/http/exec/xml/tabwriter (117 functions,
  src/ik/ik_unsupported.c) - online play still to port; Vita/iOS/Android
  front ends; .app bundling; input not yet exercised by hand.

### Status 2026-11-19 09:35

- Audio fixed: 64-bit int limit constants (beep's `math.MaxInt64` end marker)
  now map to the 32-bit limits in gotoc.
- cvf-online-5 rules ported from patches/ikemen-cvf-online.patch:
  SpecialCooldown/SpecialStates, FightAspect (shared, gameplay) and WideView
  (display only). Reference fight with the old rules still 54f8ae06…; with the
  cvf-online-5 rules the match is deterministic (4334350c…) on Linux; macOS
  arm64 still reproduces 54f8ae06….
- Not yet cross-checked against a Go build with the new rules (the container
  lacks the ffmpeg dev libraries the Go build needs); next check is a
  two-device match against the iOS build.

### Status 2026-11-20 — reproducible regeneration, dev setup

- docs/DEVSETUP.md: clone, Ikemen tree (`tools/gen/setup_ikemen.sh`), regen,
  build, reference fight. The old regen scripts pointed at a deleted
  scratchpad; the missing inputs (third/ module replacements: patched vorbis,
  go-sdl2, gl, mewkiz/pkg, dialog stub) are rebuilt from `vita/`.
- gotoc/gencdefs output no longer depends on absolute paths, FileSet offsets
  (Go version) or map order; the committed C was regenerated once (ordering
  and numbering only). Regen fails on new library placeholders
  (tools/gen/unsupported_allowed.txt).
- Verified on macOS arm64 from scratch: old rules 54f8ae06… (287,752 lines),
  cvf-online-5 rules 4334350c… (224,158 lines) — the same as Linux, so the
  new rules are deterministic across x86-64 and arm64 too.

### Status 2026-11-20 — caught up with the current fork (d4d84f7)

- `tools/ikemen_headless.patch` now carries the fork's Nearby, touch tap,
  name-prompt and motif-label changes (cvf_nearby.go, cvf_p2p_other.go as
  the desktop variant). The engine registers every `cvf*` Lua function the
  current scripts call; the native game passes a menu smoke test (title →
  Versus character select → Online > Host Nearby → Join Nearby,
  `tests/smoke/run_smoke.sh`) on macOS arm64.
- Nearby LAN discovery works for real (UDP 7520 probe/reply, subnet sweep):
  `cvf_nearby_test`, including native ↔ Go interop (`tests/ik/nearby_go`).
  Apple peer-to-peer, touch taps and the native name box stay stubs on
  desktop, as in cvf_p2p_other.go.
- New runtime pieces: crypto/rand, a math/big int64 box, net mask helpers,
  ResolveUDPAddr, a real `time.Ticker`. Fixed: UDP sends now wait for a
  full send buffer like Go; `fmt.Fprintf` writes to strings.Builder /
  bytes.Buffer / files (status texts and the GGPO desync log were lost).
- Fight aspect: `loadConfig` no longer saves the forced 0,0 into the shared
  config.ini, and applies the rules' FightAspect on every load.
- Reference fights unchanged: 54f8ae06… (old rules), 4334350c…
  (cvf-online-5), macOS arm64 and Linux x86-64.
- Regenerating without proxy.golang.org (as in the Linux container): build
  Go 1.27.4 from the github.com/golang/go tag, and replace golang.org/x/*
  and gopkg.in/ini.v1 with their GitHub mirrors (same sources) in the
  Ikemen tree's go.mod, locally only; the output is identical to the Mac's.
  Set `GOSUMDB=off` (and `GOFLAGS=-mod=mod`, `GOPROXY=off` once the modules
  are in the cache): the mirrors' module paths have no go.sum entries, and
  with the checksum database on, `go` refuses to load them. gotoc and
  gencdefs do not stop on type errors, so a package that fails to load used
  to change the generated C silently; regen.sh now checks first that every
  package loads (`go list -deps`).

### Status 2026-11-20 — online play (claude/native-netplay)

- First online matches on the native engine: `tests/netplay/run_netplay.sh`
  (DEVSETUP.md). Native ↔ native (Linux x86-64, macOS arm64) and native ↔ Go
  1.27.4 (amd64, `cvf` tag) in both directions, direct and through a relay
  with 40±15 ms delay or 20±10 ms with 8 % loss: every dump identical
  (a350bc2d…), thousands of frames re-simulated in rollbacks, no GGPO desync.
- Bugs found and fixed: map delete kept the deleted value alive and never
  reused entries (rotating save-state keys: ~250 MB/s live growth, OOM in a
  rollback match); gotoc dropped `defer close(ch)` / `defer delete(m, k)`
  (GGPO's UDP shutdown hung both peers at match end); log.Logger ignored its
  writer (GGPO's discarded per-frame trace went to stderr); fmt.Fprintln
  ignored its writer.
- First Go statedumps of the reference fight with the current tree: Go
  (`cvf` tag) gives 54f8ae06… (old rules) and 4334350c… (cvf-online-5), the
  native md5s. The desktop configuration (no vitaPerf) also gives 4334350c…
  with cvf-online-5; with the old rules it differs only through config.ini's
  FightAspect -1,-1 (with 0,0: 54f8ae06…), so the vitaPerf paths are
  simulation-equivalent on this fight.
