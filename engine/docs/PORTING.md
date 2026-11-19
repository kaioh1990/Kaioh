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

Hand-written modules (rng, detmath, anim playback, values, AIR, cooked
content) replace their Go counterparts via `overrides.txt`.

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
