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
