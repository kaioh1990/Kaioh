# PS Vita port of the native C engine

Status 2026-11-22, branch `claude/native-vita2` (on `claude/native-integration`
d290761): milestone **M2 built, not yet run on hardware**. The game runs on the
Vita path: the `cvfVitaGL` switch, the vitaGL renderer, SDL2 input and audio,
Lua menus, and per-tick frame and memory logs, packaged as `cvf-native-m2.vpk`.
Section 11 lists what was verified and what is still untested. Earlier status
(M1, `claude/native-vita`, 2026-11-21): the engine builds with vitasdk, and
the 32-bit runtime was checked and fixed on armv7 with the Vita's code
generation.

| M2 (this branch) | Result |
| --- | --- |
| Vita build | `cvf_vita` ELF and **`cvf-native-m2.vpk`** (title ID CVFN00001, installs over M1): the game by default, M1's tests and reference fight with `CVF_VITA_MODE=headless` |
| VPK image in vemu, headless mode | unit tests 103/0; reference fight **54f8ae06...** |
| Vita renderer paths on a desktop GL context (`cvf_vitasim`, Mesa) | title screen, character select and a fight drawn correctly at 960x540 in 30 and 60 fps mode; render thread: 121 of 121 frames pixel-identical to direct drawing (lockstep run) |
| Desktop, armv7, aarch64, Wine | `cvf_tests` 103/0; both reference dumps; `run_netplay.sh` PASS a350bc2d...; no function of the GL 3.3 renderer changed in the generated C |
| Hardware | **not run**: frame times, memory, picture, sound and controls need the user's Vita (section 11) |

The M1 results below are unchanged:

| | Result |
| --- | --- |
| armv7 build (Linux armhf, the Vita's flags: Cortex-A9, NEON/VFPv3, Thumb-2, hard float), under qemu-arm | reference fights **54f8ae06d74648af2182587b82a390d6** (cvf-online-2) and **4334350cdebd48dc0caa04cf78e0d9e3** (cvf-online-5), also in GC stress mode; `cvf_tests` 103/0 (both context switches) |
| Vita build (vitasdk 2026.08, GCC 15.2) | `cvf_vita_headless` ELF and **`cvf-native-m1.vpk`** (title id CVFN00001): unit tests, then the reference fight, on screen and in log files |
| Vita image in the Vita user-mode emulator (vita/vemu, Unicorn), as packaged in the VPK | unit tests 103/0; reference fight **54f8ae06...** (cvf-online-2), section 9 |
| 32-bit runtime bugs fixed | 7 (section 7), plus a GCC derived-pointer case for the collector |
| x86-64 unchanged | both reference md5s (also with CVF_GC_STRESS), netplay dump a350bc2df11a03519f7d65d278844432, cvf_tests 103/0 |
| Memory | live set 41-51 MB, heap pages peak 112 MB on armv7: fits the Vita's ~365 MB easily (section 2) |
| CPU | 4.8 M ARM instructions per game tick with the Vita's code generation (claude/native-cpu; 6.5 M before, measured with Ubuntu's stack protector and PIE); estimated 6.5-13.5 ms per tick at 444 MHz: 60 Hz logic with 30 fps drawing is reachable, 60 fps and rollback need the tick 2x cheaper again (section 3, "Native CPU work") |

The TinyGo/Go Vita build (`claude/vita-port`, vita/README.md, the
compatibility report) stays the fallback until the native build reaches
the same point on hardware (M2).

## 1. What the Vita gives a game

- CPU: 4 Cortex-A9 cores (3 for games), 333 MHz default, 444 MHz available
  to homebrew (500 MHz with an overclock plugin, which the project's Vita
  has). VFPv3 (D32) and NEON; no hardware integer divide; no 64-bit integer
  arithmetic in hardware beyond add/sub/mul.
- Memory: 512 MB main RAM, of which an application gets about 365 MB
  (`ATTRIBUTE2=12` in param.sfo raises the budget; the TinyGo build used
  it and saw ~306 MB free with a 256 MB heap block allocated), plus 128 MB
  CDRAM (GPU memory, used by vitaGL).
- No mmap, no overcommit: memory blocks (`sceKernelAllocMemBlock`) are
  physical when allocated. newlib's malloc heap is one block sized at start
  (`_newlib_heap_size_user`).
- 32-bit `time_t` in vitasdk's newlib (wall clock only; the simulation does
  not use it).
- Threads start with FPSCR = 0x03000000: flush-to-zero and default-NaN.
- The kernel rejects system calls made on a stack it does not know: stacks
  must be thread stacks or SceFiber contexts (TinyGo port).
- File systems: `ux0:` (memory card), `app0:` (the application, read only);
  case-insensitive.

## 2. Memory

Measured with `CVF_GC_TRACE=1` (per collection: live, heap pages, high-water
mark) on the reference fight (cvf-online-2 rules; cvf-online-5 alike).

| | x86-64 (container) | armv7 (qemu-arm) | TinyGo Vita build (hardware, build 28 log) |
| --- | --- | --- | --- |
| Live set at the end of the match | 48.6 MB | 50.5 MB (cvf-online-5: 40.7 MB) | ~56 MB per match (README) |
| Heap pages in use at the last collection | 76 MB | 80 MB | |
| High-water mark of the arena (resident pages) | 143 MB | 112 MB | 256 MB block reserved up front |
| Process RSS | 153 MB | 175-179 MB (before the map fix; includes qemu's code cache) | |
| Collections (during the round) | 12 (none) | 11 (one: the state dump's allocations) | 13 (two in the fight) |
| Total allocated | 469 MB | 425 MB | |
| Longest pause | 22 ms | 170-280 ms qemu time (not a hardware figure) | 0.45-0.77 s mark+sweep at 500 MHz |

- The desktop figure "the native GC peaks at 78 MB" is RSS from November 19;
  the high-water mark was 143 MB because heap pages, once handed out,
  stayed resident until reused. Since claude/native-memory the collector
  returns free pages beyond the next cycle's needs to the OS after every
  collection (GAPS.md), except on the Vita: there the arena is committed
  RAM from the start, so its size is the budget: **192 MB** by default
  (`CVF_GC_ARENA_MB` in `cvf_env.txt` changes it). The armv7 high-water
  mark is 112-120 MB; with less room the collector simply collects earlier
  (an allocation that finds no page collects and retries before failing).
- Vita budget of the headless application: GC arena 192 MB, newlib heap
  48 MB (goroutine stacks of 512 KB, stdio, the collector's side tables
  ~2 MB), main thread stack 8 MB, image 6 MB: ~254 MB of ~365 MB. The game
  (M2) adds vitaGL's textures in CDRAM/main RAM and SDL's audio buffers; the
  sound cache (`CVF_SOUND_CACHE_MB`, lazy loading of the 687 MB
  `Xsound.snd`) is set to the TinyGo build's 24 MB on the Vita.
- **32-bit conservative scanning retained far more at first**: the live set
  was 86 MB on armv7 (48 MB on x86-64). Two causes, both fixed (section 7):
  the arena sat at 0x40800000 under qemu, where every 32-bit float from ~4.0
  to ~1e15 (positions, velocities) reads as a heap address; and the maps
  stored full 32-bit hashes in scanned memory. Now 41-51 MB, on par with
  x86-64. On the Vita, memory blocks are at 0x81000000 and up, which
  floats and small integers never hit.
- Collection pauses on hardware: the x86-64 pause (22 ms for a 76 MB heap
  before claude/native-memory, 9-14 ms now: lazy sweeping, prefetching)
  times the A9's ~10-15x lower speed gives ~100-200 ms. Marking stays on
  one thread on the Vita (no helper threads there). The policy is the
  desktop one: while a round is in progress a collection runs only once
  the larger of the usual trigger (live size, at least 32 MB) and the
  fight budget (80 MB on 32-bit, `CVF_GC_FIGHT_BUDGET_MB`; before
  claude/native-memory: trigger + 48 MB, about the same for this live
  set) has been allocated. The round allocates 11.8 KB per frame (51 MB
  over the reference round on x86-64; less on 32-bit), so a 99 s round
  stays under 80 MB and collections fall at load checkpoints and round
  ends, as with TinyGo. With the state dump on, the round allocates 23 KB
  per frame and armv7 collects once mid-round (a hitch on hardware;
  harmless for the reference check). PORTING.md's "~5 KB per tick"
  predates the current fork. Cutting per-tick garbage is part of M4.
  Rollback play allocates ~1 MB per frame (save states, GAPS.md): on the
  Vita a collection every ~1.5 s at 60 fps, each a hitch of the size
  above, which online play on the Vita will have to address (memcpy
  rollback, to-do item 10).

## 3. CPU

### Measurements

| | Per fight frame (game tick) | Loading (two characters, stage) |
| --- | --- | --- |
| x86-64 (container, GCC 13 -O3) | 1.10 ms average (4,438 fight frames), max 14 ms | 1.0 s |
| armv7, guest instructions (qemu TCG plugin, gameplay only) | **6.5 M** median (mean 6.6 M, p99 10.5 M) | ~7 G |
| armv7, same with the state dump written | 7.5 M median | |
| TinyGo build, emulator instruction count (build 20) | 7.4-7.6 M per fight frame | |
| TinyGo build, hardware (500 MHz) | logic 9 ms (build 21) to 15-19 ms (build 28) per tick; drawing 6-17 ms CPU per drawn frame | ~10 s per character |

The instruction counts come from two small qemu plugins (built against a
qemu 8.2.2 tree with plugins enabled; Ubuntu's qemu-user has none):
instructions between calls of `gc_safepoint` (once per frame) and
instructions per translation block mapped to functions. Where the game
tick goes on armv7 (gameplay, no dump):

| Share | Function |
| --- | --- |
| 29 % | `BytecodeExp_runOC` (the CNS bytecode interpreter) |
| 9 % | `BytecodeStack_Push` |
| ~10 % | string comparison and maps: `memcmp`, `go_streq`, `gomap_find`, `find_slot_h`, `map_case` |
| 4 % | `StateBlock_Run`; 3 % `BytecodeExp_eq`; 3 % `Char_numHelper`; 2 % `StateBlock_evalTrigger`; 2 % `Char_varGet` |
| 2 % | `Sprite_RlePcxDecode` (loading) |

With the state dump on, 64-bit division (`__udivmoddi4`, `__aeabi_uldivmod`:
32-bit ARM has none in hardware) was 8 % of all instructions, nearly all from
formatting numbers in base 10; the runtime now divides in 32 bits once a
value fits (section 7), which took the dump run from 7.8 M to 7.5 M per tick.

### Estimate for the Vita

The native engine executes about as many instructions per tick as the
TinyGo build did (6.5 M against ~7.5 M with draw submission): the translation
is a faithful one, and the TinyGo build had by then most of the same
interpreter optimisations (bytecode fusion, var caches; the native engine
inherits them through `vitaPerf`). The TinyGo build's hardware times give
1.2-2.5 ns per instruction at 500 MHz (memory latency dominates; the
emulator does not show it), so:

- **9-18 ms per game tick at 444 MHz** (8-16 ms at 500 MHz). The M1
  application measures exactly this on the device (`frames:` line).
- 60 Hz logic with 30 fps drawing (the TinyGo build's mode: two ticks plus
  one drawn frame in 33 ms) fits if drawing stays at TinyGo's 6-8 ms.
- 60 fps drawing, and rollback (re-simulating up to N frames in one frame)
  do not fit without making the tick 2-4x cheaper. The profile says where:
  the bytecode interpreter and the string/map lookups are two thirds of the
  tick. PORTING.md's planned step "compile the cooked bytecode ahead of time
  to C" (AOT) is the lever; interning the string keys the interpreter
  compares is a second one. Neither changes the simulation's results (both
  are verifiable with the reference dumps).
- Loading: ~7 G instructions (both characters' CNS compiled from source)
  is 15-30 s at 444 MHz. Cooked characters (`cooked/<def>.cvfc`, the
  format the C loader already reads) remove the compile.
- Only one core is used. The renderer (vitaGL command building) can move
  to a second core (M3); the simulation stays single-threaded (the
  collector is).

### Native CPU work (claude/native-cpu, 2026-11-21)

Reference fight, gameplay ticks 10-4400, `armprof` = qemu 8.2.2 with a
plugin counting instructions between `gc_safepoint` calls plus a
translation-block profile of that window (rebuilt in the scratchpad from the
Vita agent's plugins).

| Build | Median / mean per tick | Loading |
| --- | --- | --- |
| 1845746, armv7 toolchain as before (Ubuntu GCC: `-fstack-protector-strong`, PIE, stack clash probes) | 6.49 M / 6.70 M | 6.91 G |
| claude/native-cpu, same flags | 5.50 M / 5.70 M | 6.60 G |
| claude/native-cpu, Vita's code generation (`armv7-linux.cmake` now: no stack protector, no PIE, no probes) | **4.79 M / 4.96 M** | 5.82 G |

The toolchain change is a measurement correction: vitasdk's GCC has none of
the three (checked with `arm-vita-eabi-gcc -Q --help=common`), and the Vita
build now drops `-fstack-clash-protection` too (no guard pages there,
`CVF_STACK_CLASH_PROTECTION`). x86-64 (container): 0.88 -> 0.79 ms per tick.

What changed, by gain: Vita code generation in the measurement (~17 %);
`//go:inline` on the bytecode value helpers and `BytecodeStack` pushes
(fork, comments only; gotoc emits `always_inline`) ~5 %; 32-bit map
hashing and direct 4-byte keys ~3 %; C `switch` for constant Go switches,
parameter escape analysis and capture by value in gotoc (fewer GC boxes:
7412 -> 6153); trigger packing and `StateBlock` field order (fork, vitaPerf:
last-level data misses of the controller walk -26 %, of `evalTrigger` -63 %
in a 32 KB / 512 KB cache simulation).

Cache misses matter as much as instructions. callgrind's cache simulation
(32 KB L1, 512 KB L2, gameplay only) gives ~48 K last-level misses per tick
against ~5.5 M instructions (x86-64): at Vita memory latency that is a large
share of the measured 1.2-2.5 ns per instruction. Two experiments that cut
instructions were slower in wall time for this reason and are not merged:

- the closure compiler (`bc_compile.go`, `vita_jit.txt`), enabled in native
  builds and without its per-node heap boxes: 4.69 M instructions, but
  1.4 ms per tick on x86-64 against 0.9 ms (indirect calls);
- ahead-of-time C for every trigger evaluated in the reference fight
  (2328 functions, 8 MB of C, compiled into the `ik_sim.c` unit; exact:
  both reference dumps unchanged): -27 % instructions on x86-64, but L2
  instruction misses 21 M -> 146 M and 1.13 ms per tick against 0.87 ms.
  Hot triggers are distinct code each, so the code does not fit any cache.

Where the tick goes now (armv7, per tick): `BytecodeExp_runOC` 2.47 M
(43 K operations, ~57 instructions each: the global value stack's loads and
stores, float64 values, entry/exit of ~5 K calls), `Char.numHelper`
0.24 M (and 29 % of all L1 data misses: it walks every helper's large
`Char`), `StateBlock.Run` 0.24 M, map lookups 0.33 M (`map(x)`/`const(x)`
by name, `CallFunction` by name, the string pool), `Command.Step` 0.07 M.
Next steps, in order: a compact per-player helper id/destroyed index for
NumHelper/NumExplod; a register-based trigger evaluator compiled at load
(the closure compiler's exact parse, values in a local array, one dispatch
loop: no code growth); caching name lookups per bytecode site.

### Native CPU work, round 2 (claude/native-cpu2, 2026-11-21)

Same fight and window. The cache figures are now callgrind with
Cortex-A9-like caches (`--I1=32768,4,32 --D1=32768,4,32
--LL=524288,8,32`), collected inside `System_action` only (the tick), on
the x86-64 build, divided by the 4438 fight frames; round 1's ~48 K was a
different setup, so the baseline is re-measured here.

| Build | armv7 median / mean per tick | Last-level misses per tick | L1 data read misses | x86-64 ms per tick |
| --- | --- | --- | --- | --- |
| 0b04ded (round 1) | 4.79 M / 4.96 M | 51.3 K | 112.6 K | 0.79 |
| + NumHelper/NumExplod indexes | 4.62 M / 4.80 M | 38.9 K | 60.3 K | 0.80 |
| + lookup caches (const, CallFunction, var, string pool) = branch head | **4.38 M / 4.54 M** | **38.7 K** | 58.6 K | **0.73** |
| register trigger VM on top (not merged) | 4.16 M / 4.34 M | 54.8 K | 76.1 K | 0.73 (0.71 without, same runs) |

(x86-64: median of 9 alternating `CVF_FRAME_STATS` runs against the
previous build; the container's timings vary by ~5 %. The rollback change
below does not run in an offline fight.) Estimated Vita tick with the round-1 rule
(1.35-2.8 ns per instruction at 444 MHz): 5.9-12.3 ms, the upper end
lower in practice since a quarter of the last-level misses are gone.
Loading is unchanged (5.83 G).

Merged:

- `src/cvf_index.go` (fork): per player, the live helpers counted by
  helper ID, and the (ID, owner) pairs of the explods in two contiguous
  arrays. NumHelper was ~980 calls per tick over ~27 helper slots (8 live)
  and 29 % of the last-level misses: every helper's `helperId` and flags
  sit at the same offset of a large `Char`, so they fall into a few cache
  sets. The indexes are rebuilt on the next query after any change: every
  writer of `Char.helperId`, `Char.specialFlag` (also through
  `CharSystemVar`), `Explod.id`, `Explod.ownerId` and of whole Char /
  Explod / CharSystemVar values bumps a generation (found with a go/types
  scan of all assignments; the state loads too), and the index remembers
  the slice it was built from. Writes whose right-hand side is a trigger
  expression bump after the write (a Helper's `id = ...` can itself call
  NumHelper). `CVF_IDX_CHECK=1` compares every answer with the walk.
- `src/cvf_lookup.go` (fork): `const(name)` cached per string pool entry
  (the site's operand) and `CallFunction` per call site, both keyed on the
  map's identity and a generation bumped by every write to such maps (they
  change only while loading). `map(x) := v` from bytecode takes the key from
  the pool's lowercase list instead of `strings.ToLower` each time. The
  var()/fvar() read cache has 64 entries (with 32, var(n) and var(n+32)
  shared a slot: misses 1.81 M -> 0.35 M over the fight), and the string
  pool's pointer cache 64 hashed slots (aligned string data used only a few
  of the 16 `p>>2&15` slots).
- Rollback save states (both fork patches): `SaveGameState` returns the
  GameStates it drops to the pool, and a pooled state reuses its `[]Char`
  arrays. Netplay (direct scenario, host): 6.68 -> 4.06 GB allocated over
  the 6431-frame match, 79 -> 49 collections, collector pauses 863 ->
  537 ms; at the Vita's estimated 100-200 ms per collection pause that is
  30 fewer pauses per match.

Not merged (archived in the scratchpad, `cpu/rvm_attempt`): a register
trigger evaluator, `bc_regvm.go`. It reuses the closure compiler's parse
(redirect scoping, jump shapes, `bc_gen.go` stack effects plus a reviewed
supplement for the opcodes that table lacks: statetype/movetype/teammode,
assignments, map keys, 32-bit jumps), builds a tree and emits 8-byte
instructions over a register file (register d = interpreter stack slot d)
with constants folded into the instruction, one dispatch loop, no heap
allocation while evaluating; programs live in shared arenas in first-use
order. `CVF_RVM_CHECK` (every trigger also interpreted and compared, plus
the stack effect of every operation left to the interpreter) passes both
reference fights, and both dumps are unchanged. But the programs are
2-3x the size of the fused bytecode they replace, and the ~3500 trigger
evaluations per tick then no longer fit the 512 KB cache: -5 %
instructions, +42 % last-level misses, x86-64 time slightly worse. Its
first version (24-byte instructions, separately allocated programs) had
doubled the misses. A register VM pays off only with an encoding as dense
as the bytecode.

Where the tick goes now (armv7, per tick): `BytecodeExp_runOC` 2.43 M
(53 %; triggers are 70 % of the tick: ~3500 evaluations, ~12 operations
each), `StateBlock.Run` 0.24 M, `evalTrigger` 0.13 M, `rangeCheck`
0.11 M, NumExplod 0.10 M (explods change ~50 times per tick, so its index
is rebuilt often), `varGet` 0.09 M, NumHelper 0.08 M, `Command.Step`
0.07 M, string-keyed map lookups ~0.19 M (map(x) reads ~275 per tick,
`getAnim`, `enemyNear`, sprite lookups). Next steps: fewer trigger
evaluations rather than cheaper ones (most come from the shared negative
states; e.g. skipping controllers whose first `&&` term is a false
`stateno = K`, which must keep the persistence counters and hitpause rules
of `StateBlock.Run`); a map(x) read cache with write-through; netplay
still allocates ~640 KB per frame (Char.Clone's animation and slice copies,
FightScreen/motif clones).

### Native CPU work, round 3 (claude/native-cpu3, 2026-11-22)

Same fight, window and tools as round 2 (armv7 guest instructions between
`gc_safepoint` calls, frames 10-4400; callgrind with Cortex-A9-like
caches inside `System_action`, x86-64 build, per fight frame).

| Build | armv7 median / mean per tick | Last-level misses per tick | L1 data read misses | x86-64 ms per tick |
| --- | --- | --- | --- | --- |
| cb9a781 (round 2, re-measured) | 4.38 M / 4.54 M | 38.7 K | 58.8 K | 0.72 |
| + short-circuit `&&` / `\|\|` | 3.98 M / 4.15 M | 38.7 K | 58.6 K | |
| + jump threading, fusion past sub-opcodes, `rcint` | 3.83 M / 4.00 M | | | |
| + trigger guards, direct `var8` | 3.81 M / 3.98 M | | | |
| + head fusions = branch head | **3.74 M / 3.91 M** | **39.0 K** | 59.0 K | **0.62** |

(x86-64: median of 7 alternating `CVF_FRAME_STATS` runs; a second set on
a busier machine gave 0.78 -> 0.69 ms. Builds that
differ only in data layout vary by about +-0.03 M on armv7 and +-0.2 K in
last-level misses: the same code measured 3.68-3.74 M depending on where
the linker put things, through string compares and map probes; the
deterministic part of the change is in `runOC`, 2.43 M -> 1.73 M.) The
interpreter now executes 31.6 K operations per tick instead of 44.6 K
(counted with a temporary profiler); the number of trigger evaluations is
unchanged (3,130 per tick; 3,288 controller visits). Estimated Vita tick
with the round-1 rule (1.35-2.8 ns per instruction at 444 MHz): 5.0-10.5
ms (round 2: 5.9-12.3 ms). Loading 5.83 G -> 5.92 G (the compile-time
analysis). Instruction count, x86-64 (callgrind): 5.13 M -> 4.40 M.

What the data said first: of the ~44.6 K operations per tick, 41.5 K are
trigger evaluation, but only 298 evaluations per tick end at their first
operation and 595 at a triggerall line; 2,136 fail inside the
`triggerN` groups, after ~11 operations each, and the cost is spread over
~2,300 controllers (the 10 most expensive are 6 %, the top 100 29 %).
Skipping controllers on a leading `stateno = K` would save little; what
the triggers do is evaluate long `&&` chains in full, because inside state
controllers `&&` and `||` do not short-circuit (Mugen compatibility:
`a && b` is `a; b; bland`, `bland` being SetB(a.ToB() && b.ToB())), and
the shared states are full of `!(name = "X" && ...)` and `var(56) = 21 &&
fvar(18) > 0 && ...` lines. Memoizing triggers whose inputs did not
change is not possible: nearly every trigger reads something that changes
every tick (time, commands, redirects, helpers, life). All changes are in
the fork (tools/ikemen_headless.patch, `vitaPerf` only; the Go build is
untouched, and the online fork has none of this code):

- Short-circuit (`src/bc_shortcircuit.go`). When the right operand of a
  CNS `&&` / `||` is pure and silent, the compiler emits `a; jzb8 k; b;
  bland` (`jnzb8` for `||`): if `a` is false (true), `jzb8` sets the top
  of the stack to the boolean `bland` (`blor`) would give and jumps over
  `b` and the operator, so the value is the one the original produces.
  "Pure and silent" is a whitelist (`scSkippable`), checked opcode by
  opcode against runOC and the Char methods it calls: constants,
  comparisons, logic and arithmetic, `div`/`mod` only by a non-zero
  constant, `var`/`fvar`/`sysvar` only with a constant index >= 0 (a
  negative one prints a warning), simple triggers (stateno, time, anim,
  ctrl, life, power, command, numhelper/numexplod/numproj, movecontact,
  pos, vel, ...), jumps that stay inside `b` (a 0 target, "end of
  expression", would end the whole evaluation in the original). Never:
  assignments, `random` (the seed), redirects (`enemy`/`root`/`parent`/...
  print a warning when they fail, `enemynear` and `p2` fill an
  order-dependent cache), `name` / `authorname` (`PushS` adds the string
  to the pool), ex_/const_ sub-opcodes. `scThread` then retargets every
  `jzb8` past the jumps its value is certain to take next (a following
  `jzb8`, `jz8p` or `jz8`; a following `jnzb8` falls through), a target
  at the end being encoded as 0 like `jz8`. -9 % on armv7 alone.
- Fusion everywhere (`src/bc_fuse.go`). The fusion walkers stopped at the
  first `const_`/`ex_`/`ex2_`/`ex3_`/`assign_` instruction (their operand
  length depends on the sub-opcode), so everything after, e.g., the
  `0 && var(10) := ...` lines of the script controllers, stayed unfused
  (`int8; var`, `jz8; pop`, ...). `opLenAt` steps over them with the
  sub-opcode tables of `bc_gen.go` (generated from the `run_*` functions;
  an independent extraction from the source agrees on every entry) and a
  small table for `assign_`; unknown sub-opcodes still stop the walk.
  `run_caller` of a constant (`enemy, numhelper(1000)`: the argument is
  evaluated as the caller) becomes `rcint8`/`rcint32`, which push the
  constant and keep the redirect instead of entering the interpreter
  recursively (~750 nested calls per tick).
- Head fusions (`src/bc_head.go`): `int8 n; enemy|enemynear|partner`,
  `const_ s; string x; eq|ne` (s without operand bytes), `blnot; jz8p`,
  `bland; jz8p`. Only the first opcode is replaced by one that executes
  the whole sequence; the tail stays, so a jump into the middle (the end of
  a `cond()` branch can be the second opcode) still runs the original
  opcodes, and every walker steps over the head and sees the original
  tail. No jump analysis needed.
- Trigger guards (`src/bc_guard.go`): a trigger that starts with a fused
  triggerall line (`tcmp8/32`, `trange8/32`, `tvcmp8`) or consists of one
  simple trigger opcode (the blocks `hoistTriggers` makes) has that
  instruction evaluated by `evalTrigger` with the same helper calls runOC
  makes, and the interpreter runs only the rest (as the slice after the
  guard: jumps are relative and 0 is the same end). This is the "skip on a
  leading guard" idea; it gains little (298 + ~600 evaluations end there)
  and is kept for the runOC entry it saves.
- `var8`/`fvar8`/`sysvar8`/`tvcmp8` call `varGet(n)` directly instead of
  pushing `n`, converting it to float64 and back.

`CVF_SC_CHECK=1` checks all of it at run time: every skip still evaluates
the skipped operand (as an expression of its own) and panics if that
changed the random seed, wrote to the console (`appendToConsole` now
counts calls) or left the value stack unbalanced; every guard that is
false also runs the whole trigger, which must be false; every
`const_`/`ex_`/`ex2_`/`ex3_`/`assign_` instruction the interpreter runs
must have consumed exactly the operand length `opLenAt` assumes. Jump
threading is not done in check mode (the check needs the original operand
length). Both reference fights pass with `CVF_SC_CHECK=1
CVF_IDX_CHECK=1` and give the same dumps. The cooked format
(`cvfcook.go`, Vita only) can now contain `jzb8`/`jnzb8` (193/194) in
trigger bytecode; the other new opcodes only appear after fusion.

Not merged:

- A per-StateBlock field caching the guard length: a few instructions
  cheaper per evaluation, but as a new field it grew the hot first line of
  StateBlock (last-level misses 38.8 K -> 40.1 K); in a free byte of that
  line (`nestedInLoop` moved out, it is compile-time only) 39.0 K and no
  measurable armv7 gain. The guard length is looked up in a 256-byte table
  instead.
- Memoizing triggers by an input fingerprint (var generation, stateno,
  prevstateno, state and move type, roundstate): 95 % of the evaluations
  would hit, but 95 % of the evaluations read other inputs too (time,
  commands, redirects, helpers, life), so only ~150 per tick qualify.

Where the tick goes now (armv7, per tick): `BytecodeExp_runOC` 1.73 M
(44 %; ~31.6 K operations at ~55 instructions each including entry and
exit: the 16-byte stack values, float64 conversions and the global stack
slice reloaded around every call), `StateBlock.Run` 0.24 M,
`evalGuarded` + `evalTrigger` 0.20 M, string-keyed maps ~0.21 M (map(x)
reads ~275 per tick, `getAnim` ~230, `enemyNear`'s `playerID` lookups
~240, `sysVarGet` ~125 without a cache), NumExplod 0.10 M, `rangeCheck`
0.09 M, `varGet` 0.08 M, `Command.Step` 0.07 M. Next steps, by expected
gain: a cache for `sysVarGet` and the `enemyNear` pointers; incremental
explod index updates (per player instead of one global generation); more
head fusions for the remaining pairs (`var8; abs; eq8` ~490 per tick,
`stateno; range_ii32` ~510, `eq8; jz8p` / `eq8; jzb8` ~860, `jz8p;
jnz8p` ~275); then the interpreter's per-operation cost itself (a stack
pointer kept in a local between calls) — 3 M per tick needs that, since
the remaining operations are spread thinly over thousands of controllers.

Rollback garbage (netplay, direct, `GC_PROFILE` build as host, one 6,159-
frame match): 590 KB allocated per frame (~30 KB of it is loading), 49
collections, pauses 1.5 s in total (37 ms each on this busy machine). By
allocation site: `Char.Clone` 149 KB per frame (2,069 allocations),
`Explod.Clone` 118 KB (180 explods), `FightScreen.Clone` 113 KB (one
`new(FightScreenRound)` per save, a ~100 KB struct, plus 9 KB in its
fades), `SaveGameState` 58 KB, `PalFX.Clone` 42 KB (the explods' PalFX),
`GameStatePool.Get` 14 KB, `Animation.Clone` 9 KB. Next: reuse the
FightScreenRound, explods and PalFX of the dropped state that
`SaveGameState` already recycles (round 2), as for the `[]Char` arrays;
that would remove about half of it. Not done in this round.

### Native CPU work, round 4 (claude/native-cpu4, 2026-11-22)

Same fight, window and tools as rounds 2 and 3 (armv7 guest instructions
between `gc_safepoint` calls, frames 10-4400; callgrind with Cortex-A9-like
caches inside `System_action`, x86-64 build, per fight frame).

| Build | armv7 median / mean per tick | Last-level misses per tick | L1 data read misses | x86-64 ms per tick |
| --- | --- | --- | --- | --- |
| 989dbeb (round 3) | 3.737 M / 3.908 M | 39.0 K | 59.0 K | 0.675 |
| + interpreter per-operation cost (8fde2f8) | 3.515 M / 3.684 M | 39.1 K | 59.8 K | 0.670 |
| + explod index without spawn rebuilds | 3.482 M / 3.643 M | | | |
| + sysvar() cache | 3.476 M / 3.636 M | | | |
| + playerID cache | 3.454 M / 3.615 M | | | |
| + name / authorname compare memo | 3.385 M / 3.544 M | | | |
| + head fusions (13b5269) | 3.356 M / 3.516 M | 39.0 K | 48.4 K | 0.643 |
| + rollback saves reuse their copies (b4924b5) = branch head | **3.357 M / 3.518 M** | **39.1 K** | 48.4 K | **0.678** |

(x86-64: median of 7 alternating `CVF_FRAME_STATS` runs against the
previous commit; the last row is the median of 11 alternating runs against
989dbeb, which gave 0.679 in the same runs: on this busy container, with
single runs spread over 0.60-0.80 ms, the head is not measurably faster
on x86-64 than round 3, although it executes 6 % fewer instructions and
has 18 % fewer L1 data misses there. The rollback
change does not run in an offline fight; its row differs from the one
above only by code placement.) x86-64 instructions (callgrind): 4.40 M ->
4.13 M. Estimated Vita tick with the round-1 rule (1.35-2.8 ns per
instruction at 444 MHz): **4.5-9.4 ms** (round 3: 5.0-10.5 ms). Loading
5.92 G -> 5.93 G. The goal of 3 M per tick is not reached: -10 % this
round, see "what is left" below.

Interpreter (8fde2f8, -6 %). runOC keeps the value stack in locals: the
backing array of `sys.bcStack` and the length, for the opcodes that call
nothing which uses the stack (constants, jumps, comparisons and
arithmetic, var/fvar/sysvar reads, the simple triggers, the k-redirects,
the head fusions). Opcodes whose helpers work on `sys.bcStack` itself
(tcmp/tvcmp, cstrcmp, assign_/const_/ex*_) store the length back and run
in `runOCx`; every other opcode runs in `runOp`, which is the original
switch, moved as it was (its `continue`s return "keep the redirect"). The
exit returns the top value without a second slice copy, and a pushed value
is computed after the growth check, so no FP register lives across the
call. That alone was worth little (3.737 -> 3.710 M): the cost per
operation is mostly the value representation, and per call. Two more
changes carry the rest:

- gotoc: `s[lo:hi]` with an explicit `hi` is emitted as `GO_SLICE_HI` /
  `GO_ARRSLICE_HI`, without the "hi < 0: omitted" sentinel test that the C
  compiler cannot fold for a run-time `hi` (every slicing in the engine;
  -4 % on armv7 with the runOC exit change).
- `go_f2i32` / `go_f2i64` (gort.h, hand-written) test the range with one
  comparison on `fabs` instead of two: the values in range that `|f| <
  2^31` leaves out, (-2^31-1, -2^31], truncate to INT32_MIN, the
  out-of-range result, anyway, and `fabs(NaN) < x` is false. The fork's
  `ToB` likewise tests `|value| > 0` (false for 0 and NaN) instead of
  IsUndefined plus `value != 0`. Each comparison is a `vcmp` + `vmrs` on
  the Cortex-A9, which also stalls the pipeline on hardware.

runOC now executes ~42 instructions per operation (55 before), of which
~9 are the dispatch (bounds check, `tbh`, loop) and ~10 the entry and exit
of ~4,400 calls per tick spread over the operations.

Smaller items (13b5269, -4.5 %):

- NumExplod index (`cvf_index.go`): the explod a controller fills in right
  after `spawnExplod` (Explod, GameMakeAnim: `id = 0`, then the id
  parameter) is counted from its own fields by `numExplodIdx` and skipped
  by a rebuild, until the next spawn folds it into its player's index
  (controllers do not nest, so there is one at a time). Spawning no longer
  invalidates the index: Alucard spawns ~49 explods per tick and queries
  NumExplod ~120 times, and every spawn had cost a rebuild (~47 per tick);
  `flagForRemoval` now invalidates only for an explod the index holds (id
  >= 0). numExplodIdx 97 K -> 49 K per tick. Per-player generations
  instead would have saved little: player 1 spawns 98 % of the explods
  and makes 81 % of the queries.
- `sysvar()` reads: a 16-entry cache beside the var cache, same keys (the
  map's identity, `varGen`); `sysVarSet`/`sysVarAdd` write through,
  `sysVarRangeSet` bumps `varGen`.
- `playerID` (enemyNear's cached list, parent, root, ...): a 16-entry
  direct-mapped cache in front of the `idMap` lookup, keyed on the map's
  identity and `idMapGen`, which every write to an idMap bumps
  (`CharList.clear`/`add`/`delete`, the state clone; both save and load
  clone). The destroyed/disabled checks still run on every call.
- `name = "X"` / `authorname = "X"` (the cstrcmp head, ~850 per tick):
  the plain sequence adds the name to the working string pool and compares
  the lowercase texts. The pool only grows until it is cleared or replaced
  (both now give it a new generation, `StringPool.gen`), its entries never
  change and it holds each text once, so after the first compare the add
  changes nothing and the result is fixed. It is kept per pool string x
  for the last two name strings compared with it (matched by identity; the
  entry keeps the string, so its memory is not reused while cached).
  Tried first: one 32-entry hashed cache for all sites (thrashed: 875 K
  misses per fight against 2.2 M hits), content comparison instead of
  identity (the memcmp ate the gain).
- Head fusions (`bc_head.go`, opcodes 203-208): `eq8; jz8p`, `eq8; jzb8`,
  `stateno; range_xx32`, `var8; eq8`, `var8; abs`, `bland; jzb8`. The
  jzb8 heads run only their first opcode under `CVF_SC_CHECK`, so the
  jzb8 still checks its skip.

Not merged: a `case 255` in runOC's switch so that GCC would index the
jump table without a range check (it keeps the check: no gain).

Rollback garbage (b4924b5, both fork patches). A GameState that
`SaveGameState` dropped (round 2 returns them to the pool) still holds its
copies of the FightScreenRound (~100 KB per save) and of every explod and
its PalFX, and nothing else holds them (LoadState clones them again).
SaveState copies into them now (`cloneInto` for FightScreenRound,
FightScreen, Explod and PalFX; `Clone` is `cloneInto(nil)`) and keeps the
explod arrays, as `saveCharData` does with the `[]Char` arrays. Netplay,
direct scenario, refgame5, native host and guest, `CVF_GC_TRACE`, one
6,431-frame match (dump a350bc2d... unchanged):

| | Allocated | Collections | Pauses, total |
| --- | --- | --- | --- |
| host, 13b5269 | 4118.5 MB | 49 | 590 ms |
| host, b4924b5 | 2939.2 MB (-29 %) | 38 | 500 ms |
| guest, 13b5269 | 4381.7 MB | 55 | 643 ms |
| guest, b4924b5 | 2876.4 MB (-34 %) | 37 | 490 ms |

That is ~430 KB per frame (re-simulated frames included) instead of ~600
KB; at the Vita's estimated 100-200 ms per pause, 11-18 fewer pauses per
match. What remains is mostly `Char.Clone` (its animation and slice
copies, ~150 KB per frame in round 3's profile), SaveState's own slices,
the explods' Animation copies and the fades.

Go cross-check: the fork does not build with `go build -tags cvf,cvfgohead`
any more (`boundTexture` is declared in both render_gl33.go and
render_gles32.go, which the `cvf` tag both selects, and cvf_mobile.go needs
the GLES32 renderer; gotoc ignores type errors, so regen never showed it).
The netplay checks therefore ran with two Go hosts: `cvf_headless_go` from
0b04ded's tree (as in rounds 2 and 3), and one built from this branch's
fork tree with `-tags cvfgohead` only, the shipping PC/mobile
configuration (vitaPerf off, so the interpreter changes are not in it,
but the rollback change and `ToB` are). Both play the same match as a
native guest (a350bc2d...). The online fork with b4924b5's change
compiles (`go build ./src`).

What is left, armv7 per tick: `BytecodeExp_runOC` 1.32 M (37 %; ~31 K
operations at ~42 instructions), `StateBlock.Run` 0.24 M and
`evalGuarded` 0.14 M (3,288 controller visits), `runOCx` 0.10 M
(tcmp/tvcmp through the global stack: they could move to the locals too),
`rangeCheck` 0.08 M (not inlined; an int-only fast path), `varGet` 0.08 M,
`Command.Step` 0.07 M, string-keyed maps ~0.15 M (`hash_key_bytes`: map(x)
reads, ~275 per tick, and the remaining PushS), `cstrcmpName` 0.06 M,
NumHelper 0.06 M, `cmpConst` 0.05 M, NumExplod 0.05 M. The interpreter's
remaining per-operation cost is the value representation: every value is
a float64 with a type tag, and each conversion or test is a VFP compare
with a `vmrs`. 3 M per tick needs either typed operations chosen at
compile time (an int-only fast path for the comparisons and arithmetic
whose operands are known ints: most `var(n) = k`, `stateno = k`,
`time > k`) or fewer runOC calls (~4,400 per tick at ~40 instructions of
entry and exit: one call per controller instead of per trigger).

## 4. Determinism on 32-bit ARM

- **Verified**: the armv7 build produces both reference dumps bit for bit
  under qemu-arm (which implements the ARM pseudocode for VFP with IEEE
  semantics), and the same in GC stress mode. The Vita build uses the same
  source and flags with GCC 15.2 instead of 13.
- Flags: `-ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing`
  and `-fsigned-char` (plain char is unsigned in the ARM EABI; the same as
  the Android branch).
- Double precision is native: VFPv3 does f64 add/mul/div/sqrt in hardware,
  IEEE-exact with round-to-nearest. No x87-like excess precision exists on
  ARM.
- The Vita build contains no NEON floating-point arithmetic and no fused
  multiply-add (VFPv3 has none; checked with objdump: 0). GCC emits
  `vmla`/`vmls` (1,266) even with `-ffp-contract=off`: on VFPv3 these are
  the *chained* forms (FPMul then FPAdd, two roundings), bit-identical to a
  separate multiply and add; qemu implements the same pseudocode, so the
  armv7 runs cover them. NEON would matter (it always flushes denormals);
  GCC does not vectorise floating point for NEON without
  `-funsafe-math-optimizations`.
- **The Vita's FPSCR**: threads start in flush-to-zero/default-NaN mode,
  which was the only difference the TinyGo build found against the PC (all
  21 math categories match with IEEE mode). The application sets IEEE mode
  at start (`go_vita_fp_ieee`, core/sched.c) and every goroutine fiber does
  it on entry.
- Transcendentals: all through detmath (bit-exact on every platform).
- Out-of-range float to integer conversions follow Go/amd64 in
  `go_f2i32`/`go_f2u32` helpers (gort.h), not the ARM instruction's
  saturation. (The TinyGo build hit this: `BytecodeExp.appendValue`
  turned -1..-128 into 0 on ARM.)
- Settings that are simulation state must match between the Vita and the
  PC: FightAspect (forced 0,0 in native builds) and GameWidth/GameHeight
  (the TinyGo report).

## 5. Graphics

The TinyGo build renders with **vitaGL**, not raw GXM: the GLES renderer
with GLSL 1.x shader ports (`vita_shaders.go`), runtime shader compilation
(`libshacccg.suprx` extracted on the device), 960x544, and a set of
workarounds found on hardware: no scissor (vitaGL redraws a full-screen
stencil per change; clipping moved into the sprite shader), client-side
vertex arrays instead of `glBufferData` per sprite, atlases capped at 1024
and a 256² palette texture (vitaGL copies a whole in-use texture on
`glTexSubImage2D`), direct rendering to the display buffer, and a locked
30 fps in matches.

The native engine's renderer is the translation of `render_gles32.go`
(`#version 320 es` shaders), reached through `gles_bind.c` (GL entry points
looked up by name). The Vita shader set and workarounds sit behind `isVita`,
which is a compile-time `false` in the native configuration, so gotoc
translated them away until M2.

### M2: the `cvfVitaGL` switch (fork patch, regenerated)

`src/cvf_vitagl.go` in the fork (`tools/ikemen_headless.patch`) adds
`var cvfVitaGL bool`, set by the Vita platform layer before `main`, and
`onVita()`, which returns `isVita || cvfVitaGL` and is inlined. In the C
engine, every site in this list now tests `g_cvfVitaGL` at run time.
Everywhere else `cvfVitaGL` stays false, and the generated C of the GL 3.3
renderer is unchanged (no function of `render_gl33.go`/`font_gl33.go`
differs; section 11).

- GLES renderer, all of `render_gles32.go`, `render_gles32_batch.go` and the
  Vita sites of `render.go`: the GLSL 1.x shaders (`vita_shaders.go`, now
  built with the `cvf` tag), clipping in the sprite shader instead of
  scissor (`clipOn`/`clipRect`), client-side vertex arrays and sprite
  batching (`vitaClientArrays()` returns `cvfVitaGL`), atlases up to 1024,
  LUMINANCE/RGB/RGBA formats, no UNPACK_ALIGNMENT/ROW_LENGTH (rows are
  repacked), drawing straight to the display buffer (`vitaDirect`;
  `CVF_VITA_FBO=1` keeps the offscreen pass), no models or external shaders,
  cached matrix uniforms, the `fxOn` fast path.
- `system.go`: the 256² palette texture, no SDL GL context, the 30 fps fight
  mode (`vitaHalfRate()`: `cvfVitaFPS != 60` and in a fight), the
  `vitaMatchEnd` bookkeeping, no file search for `<...>` select
  placeholders. `config.go`: 960x540, MSAA 0, no external shaders or
  models. `main.go`: RenderMode "OpenGL ES 3.2". `system_sdl.go` and
  `sdl_other.go`: no SDL window, 960x544, vitaGL's swap.
- Files and audio (`common.go`, `sound.go`, `audio_sdl.go`): no
  case-insensitive glob fallback for missing files (on the Vita a failed
  stat means the file is not there; the glob read 110k directory entries
  per boot in the TinyGo build), buffered file reads, music streamed instead
  of decoded into RAM (no BGMRAMBuffer), resampling quality 1, and the
  deeper SDL audio queue that cooperative goroutines need.
- `motif.go`: the TinyGo build's motif fixes (`vitaMotifFixes`).
- Hooks (`GOTOC_NATIVE_FUNCS`, implemented in `ik_native.c` through function
  pointers that the platform layer sets): `cvfVitaWindow`, `cvfVitaSwap`
  (called by `vitaGLInit`/`vitaGLSwap`) and `cvfVitaPerf` (the existing
  `vitaPerfBegin/End/Tick` marks, plus one around EndFrame and the swap in
  `System.await`).

Not switched, because they are simulation state or TinyGo matters: the
bytecode profiler and hash dumps, the TinyGo collector triggers, string
cloning, trace output. On the Vita, `GameWidth`/`GameHeight` are 960x540, as
in the TinyGo build. That does not matter offline, but netplay with a PC
needs matching values (M3).

In the same patch: render_gles32.go's `boundTexture` is now
`boundTextureGLES32`. With `-tags cvf`, both GL renderers compile, and the
duplicate type had kept `go build -tags cvf,cvfgohead` from building since
round 4. That build works again and plays the reference fight
(`54f8ae06...`). In the generated C, the GLES renderer's texture-unit cache
now has its own struct type instead of borrowing the GL 3.3 one. The two
have the same layout, so this changes no behaviour.

### Platform side (`src/platform/vita/vita_game.c`, `gles_bind.c`)

- `vglInitExtended(0, 960, 544, CVF_VGL_RAM_THRESHOLD_MB=16 MB, no MSAA)`,
  `vglWaitVblankStart(CVF_VITA_VSYNC=1)`, `vglSwapBuffers` at the swap. vitaGL
  takes the main RAM left after the collector's arena and newlib's heap,
  minus the threshold, so it is initialised after both.
- GL entry points come from `vglGetProcAddress`. A name that vitaGL lacks
  gets a stub returning 0 and is logged once (`gles: vitaGL has no ...`), as
  in the TinyGo build: the paths that need those calls are off on the Vita.
- vitaGL's runtime shader compiler (vitaShaRK + SceShaccCg) needs
  `ur0:data/libshacccg.suprx` on the device, as the TinyGo build does.
- Clocks: ARM `CVF_ARM_MHZ` (444), bus 222, GPU 222, crossbar 166 (the
  TinyGo values).

### Drawing on a second core (`CVF_VITA_RENDER_THREAD=1`)

Implemented behind the setting (default off), in `src/platform/gles/`
(`gles_thread.h`, `gles_thread_impl.h`, compiled into `gles_bind.c` with
`CVF_GL_THREAD`):

- One render thread owns the GL context: vitaGL is initialised on it and
  every GL call runs on it. It is pinned to core 1 and the engine thread to
  core 0.
- After `gl.Init` has loaded the entry points, each `p_glX` pointer of the
  binding is redirected to a recorder. The translated renderer is unchanged.
  Each GL call appends a command (thunk, 64-bit argument slots, copied data)
  to the current buffer. Data passed by pointer is copied at record time:
  uploads, uniform arrays, id lists, and, for client-side vertex arrays, the
  vertices that each `glDrawArrays` will read (taken from a shadow of the
  bound VAO's attribute pointers, with overlapping ranges merged). Uploads
  over 1 MB run synchronously instead.
- Synchronisation: two command buffers and two SDL semaphores. At the swap,
  the engine waits until the render thread has finished the previous frame
  (at most one frame in flight), hands over the current buffer and goes on
  with the next game tick in the other buffer. Calls that return something
  (`glGen*`, `glGet*`, `glCreate*`, `glReadPixels`, `glFinish`, shader
  source, buffer mapping) hand over the buffer plus a call command and wait
  for the result. They happen while loading. `glGetError` returns 0 without
  waiting (the renderer only logs it), so `CVF_GL_CHECK` is not available
  with the thread.
- Determinism: GL results reach the engine only through those synchronous
  calls, which return what a direct call returns, in the same order. The
  simulation reads nothing else from the render thread. Checked on the
  desktop: the statedump is identical with and without the thread, and in
  lockstep runs (the same ticks drawn in both) all 121 compared frames are
  pixel-identical (section 11). `tests/gles/gles_thread_test.c` compares
  frames drawn through the thread with direct calls (client arrays with
  first > 0, uploads, a large synchronous upload, an FBO, mid-frame reads).
- Expected effect on the Vita: vitaGL's CPU work (6-17 ms per drawn frame in
  the TinyGo build) leaves the engine thread, which then pays only for
  recording (0.2-0.5 ms per frame in the desktop runs). A drawn frame then
  costs max(logic + draw-list building + recording, vitaGL), not the sum.
  This is unmeasured: the hardware run decides whether 60 fps drawing is
  reachable (`perf` lines, section 11).

Raw GXM (no vitaGL) remains a later optimisation if vitaGL's CPU time is
still the limit. The renderer only draws textured quads.

## 6. Platform layer

| Area | Plan / state |
| --- | --- |
| Entry | `src/platform/vita/vita_main.c`: `sceUserMainThreadStackSize` 8 MB (the character compiler recurses deeply), newlib heap 48 MB, IEEE FP mode, `cvf_env.txt` -> environment, logs in `ux0:data/CvFNative/`. `CVF_VITA_MODE` chooses: `game` (default, M2: `vita_game.c` runs the translated `main` with `cvfVitaGL` set, in `CVF_GAME_DIR`, default `ux0:data/CvFNative/game`; extra arguments from `cvf_args.txt`) or `headless` (M1: `cvf_tests_main`, then `cvf_headless_main`). |
| Goroutines | SceFiber fibers (core/sched.c `CVF_VITA_FIBERS`); the main thread is the hub (fibers switch only thread <-> fiber), registers are spilled for the collector, IEEE mode per fiber. |
| Collector | arena = one memory block (`sceKernelAllocMemBlock`, zeroed), data roots from `__data_start` to a marker in `.noinit` (not `__bss_end__`: section 7), stack bounds from `sceKernelGetThreadInfo`, owner check by thread id. |
| Clock, sleep | `sceKernelGetProcessTimeWide` (monotonic, µs), `sceKernelDelayThread`. |
| Files | newlib's file API on `ux0:`; `chdir` works (newlib emulates a working directory). Game folder `ux0:data/CvFNative/<game>` (the TinyGo build uses `ux0:data/CvF`; separate folders, as with the Android apps). `os.Executable` is `app0:eboot.bin`. The file systems are case-insensitive, so Ikemen's case-insensitive lookup fallback (a directory glob per miss) is skipped on the Vita (M2: `common.go` under `cvfVitaGL`), as the TinyGo build does (110k directory reads per boot). |
| Input (M2, done) | SDL2's Vita joystick backend as a game controller (SDL's built-in "PSVita Controller" mapping; the game's `[Joystick_P1]` config applies). Front touch: SDL polls the panels only while an SDL window exists, so the window hook creates a plain 960x544 SDL window (no GL, no GXM state) and an event watch passes front-panel taps (touch device 1) to `cvfHostTap`, the iOS host entry point that feeds the menus' `cvfTouchTap`. Touch-to-mouse events are off. |
| Audio (M2, done) | SDL2 audio (the Vita backend over `sceAudioOut`), queue-based as on the desktop, with the deeper queue for cooperative goroutines; music streamed (no BGMRAMBuffer decode); resampling quality 1; sound cache 24 MB. |
| Networking (M3) | BSD sockets from vitasdk's newlib (SceNet; `_vita_net_init` brings up sceNet/sceNetCtl). `ik_net.c` builds unchanged except interfaces: `vita_ifaddrs.c` lists loopback and the Wi-Fi interface from SceNetCtl (address, netmask) for Nearby's subnet sweep. TCP + GGPO UDP as on desktop; cross-play with PC needs matching GameWidth/Height. Ad hoc (SceNetAdhoc) only reaches other Vitas: not useful for PC cross-play, so not planned. |
| HTTP (M3) | lobby, public IP, UPnP: SceHttp/SceSsl or vitasdk's curl package; until then requests fail with an error (`ik_http.c`). |
| Packaging | `tools/package/vita_vpk.sh` (section 8). |

## 7. 32-bit fixes in the hand-written runtime

All in `src/core` / `src/ik` (no generated C changed, no regeneration
needed); every one keeps the 64-bit results (reference md5s, netplay dump).

1. **Context switch for 32-bit ARM** (core/sched.c): an AAPCS `ctx_switch`
   (r4-r11, d8-d15, interworking `pop {pc}`) instead of the ucontext
   fallback, which saves the signal mask with a system call per switch and
   is not available on the Vita. Android armeabi-v7a gets it too.
2. **Vita goroutines as SceFiber fibers** with registers spilled for the
   collector and IEEE FP mode per fiber.
3. **Arena placement** (core/gc.c): on 32-bit targets the arena is requested
   at 0x90000000 (then 0xA0000000, 0x80000000): addresses that read as
   large negative integers and tiny negative floats. At qemu's default
   (0x40800000) every float in ~[4, 1e15] was a false pointer; live set
   86 -> 64 MB.
4. **Map hashes masked to 28 bits** (core/gort.c): the entry array is
   scanned and held full 32-bit hashes, random words that hit the used heap
   a few percent of the time; 28-bit values never reach the arena. Lookups,
   slots and iteration order are unchanged on every platform. Live set
   64 -> 41-51 MB, high-water mark 154 -> 112 MB.
5. **Derived pointers below an object** (core/gc.c `mark_derived`): GCC's
   pre-indexed loads on 32-bit ARM can leave only `data - 4` live across a
   call; stack and register words now also mark blocks starting within 256
   bytes above them. Heap words unchanged. Test: `test_gc_roots.c` case 4
   (fails without the fix on x86-64 and armv7). Details in GAPS.md.
6. **Vita data roots**: `__bss_end__` lies at the very end of the data
   segment and is not relocated in the Vita image (vemu showed `__data_start`
   moved with the segment while `__bss_end__` kept its link address:
   objects referenced only from .bss were freed, caught by `gc_selftest`).
   The range now ends at a marker object in `.noinit`.
7. **Base-10 formatting in 32 bits** (ik_fmt.c, ik_std2.c): the Cortex-A9
   has no divide instruction, and 64-bit division by 10 per digit was 8 % of
   the dump run. Byte-identical output (checked against printf on
   boundaries and 1M random values, x86-64 and armv7).

Also: `go_streq` returns at once for identical storage (string-pool
comparisons); newlib portability (no `%zu` in the collector's messages,
`clock_gettime`, `timegm`/`tm_gmtoff` replacement, no `readlink`); the
headless runner's frame statistics (`CVF_FRAME_STATS=1`) and the
collector's high-water mark (`gc summary ... hwm`).

Reviewed and fine on 32-bit: `int` is 32-bit everywhere already (Go `int`
is translated as C `int`), slices/strings use `int` lengths, the collector
scans in pointer-size words and its mark bitmaps are 64-bit words on every
platform, size classes are 16-byte multiples (8-byte alignment for doubles
and `ldrd`), float-to-integer helpers do not depend on the word size, and
`time_t` is only used for wall-clock time.

## 8. Build

### armv7 Linux (no Vita needed)

    sudo apt-get install gcc-arm-linux-gnueabihf qemu-user
    # SDL2 headers only (the SDL functions are stubbed): e.g. a copy of
    # /usr/include/SDL2 with the x86_64 _real_SDL_config.h as SDL_config.h
    cmake -S . -B build/armv7 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=tools/package/armv7-linux.cmake \
      -DCVF_SDL_STUB=ON -DCVF_SDL2_INCLUDE_DIR=/path/to/SDL2-headers
    cmake --build build/armv7 -j2      # cvf_headless, cvf_tests, cvf_tests_ucontext (~4 min)
    qemu-arm build/armv7/cvf_tests     # 103 checks, 0 failures
    cd refgame && qemu-arm ../build/armv7/cvf_headless . -p1 Alucard -p2 Richter \
        -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1     # ~3 min

Static executables; `-DCVF_GC_STRESS=ON` for the stress build
(`CVF_GC_STRESS=10000` collects every 10,000 allocations: ~1,100
collections in the reference fight, ~15 min under qemu).

### Vita

vitasdk: the 2026.08 release (the "supported" channel of
`vitasdk/autobuilds`' channels.json, `sdk-snapshot-20260825.611.1`). The
GitHub API and vitasdk.org are not reachable from the project's container,
but release assets are; each release lists them in `SHA256SUMS`:

    T=sdk-snapshot-20260825.611.1
    curl -LO https://github.com/vitasdk/autobuilds/releases/download/$T/SHA256SUMS
    F=$(grep -o 'vitasdk-x86_64-linux-gnu-[0-9_-]*\.tar\.bz2' SHA256SUMS)
    curl -LO https://github.com/vitasdk/autobuilds/releases/download/$T/$F
    grep " $F" SHA256SUMS | sha256sum -c && tar xjf $F     # -> vitasdk/
    export VITASDK=$PWD/vitasdk
    # packages for the game (M2): SDL2, vitaGL and its runtime shader compiler
    for n in sdl2 vitaGL vitaShaRK SceShaccCgExt libmathneon taihen kubridge; do
      curl -L -o $n.tar.xz https://github.com/vitasdk/packages/releases/download/master/$n.tar.xz
      tar xJf $n.tar.xz -C $VITASDK/arm-vita-eabi
    done

    cmake -S . -B build/vita -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
    cmake --build build/vita -j2                  # cvf_vita (ELF, ~8 min)
    tools/package/vita_vpk.sh build/vita cvf-native-m2.vpk

(The packages release has no SHA256SUMS. `math-neon`/`mathneon` are 404:
the package is `libmathneon`.) Since M2 there is one ELF, `cvf_vita`, which
runs the game, or M1's tests and reference fight with `CVF_VITA_MODE=headless`.
M1's `cvf_vita_headless` target is gone. The VPK's icon and LiveArea are
text-only images (`vita/cvfnative-sys`). The art in `vita/cvfsys` (used by
M1 and the TinyGo `pack.sh`) is made from the game's title picture and is no
longer packaged.

`vita_vpk.sh` follows the TinyGo port's packaging: `vita-elf-create`,
`vita_velf_fixrel.py` (drops same-segment branch relocations: the Vita's
loader rewrites conditional BL into B, which crashed TinyGo builds 2-8),
`vita-make-fself`, param.sfo with `ATTRIBUTE2=12`, the CvF LiveArea art
from `vita/cvfsys`. It also writes the `.velf` next to the VPK (for vemu
and for symbolising core dumps).

### Running M2 on the Vita

`docs/VITA_M2_README.md` is the user's guide (installation, game folder,
`cvf_env.txt` settings, what to send back). It ships next to the VPK.

### Running M1 on the Vita

(M2's VPK runs this with `CVF_VITA_MODE=headless` in `cvf_env.txt`.)

1. Install `cvf-native-m1.vpk` with VitaShell (it installs next to the
   TinyGo "CvF Online", title id CVFN00001).
2. Copy the reference game directory (cvf-refdata.tgz plus
   `data/Xsound.snd` from the full game, with `vita_statedump.txt` /
   `vita_statehash.txt` present) to `ux0:data/CvFNative/refgame/`. For the
   cvf-online-5 rules, copy that rules file over `data/cvf_online.ini`.
3. Start "CvF Native M1". The screen shows the unit test result, then after
   the fight the statedump md5 (marked REFERENCE when it matches either
   reference), the frame times and the collector summary. The same text is
   in `ux0:data/CvFNative/result.txt`; full logs in `tests.log` and
   `headless.log`. START exits.
4. Optional: `ux0:data/CvFNative/headless_args.txt` (one line: game dir and
   arguments) and `cvf_env.txt` (`KEY=VALUE`: `CVF_ARM_MHZ=500`,
   `CVF_GC_ARENA_MB`, `CVF_GC_TRACE`...).

### vemu (the TinyGo port's Vita user-mode emulator)

`vita/vemu` on `claude/vita-port` runs a Vita ELF in Unicorn with the Sce
imports in Python (fibers, memory blocks, I/O); with `--velf` it also
applies the image's relocations as the Vita loader does (including the
conditional-BL rewrite). Local changes for the native image: an 8 MB main
stack (`VEMU_STACK_MB`), `sceKernelGetThreadInfo` reporting it, and
`native.bin` assembled from `native.S`. It found fix 6 of section 7, the
newlib `printf` redirection problem (vita_main.c defines `printf`/`puts`) and a
summary loop in vita_main.c that read back its own log.

## 9. M1 in vemu

The VPK's image (the `.velf`, relocated as the Vita loader does, plus the
ELF for symbols) in vemu, with `ux0:` mapped to a folder holding the
reference game (cvf-online-2 rules):

| | Result |
| --- | --- |
| Unit tests | **103 checks, 0 failures** |
| Reference fight | statedump **54f8ae06d74648af2182587b82a390d6**, 287,752 lines, winner 1 ("= REFERENCE" on screen) |
| Goroutines | ~2,000 SceFiber fibers created, run and finalized (tests and fight) |
| Collector | 276 collections (most from the unit tests in the same process), arena high-water mark 146 MB of 192 MB |

vemu's times (1,383 s for the run, 320 s to the first fight frame) are
Unicorn plus Python per import call, not hardware figures. Three Vita
problems were found and fixed this way (section 7 item 6, and in
vita_main.c newlib's `printf` ignoring a redirected stdout and a summary
loop that read back its own log); none of them would have shown under
qemu-arm. The cvf-online-5 rules were not run in vemu (the armv7 build
covers them).

## 10. Milestones

| | Content | Exit criterion |
| --- | --- | --- |
| **M1** (`claude/native-vita`) | headless VPK: unit tests + reference fight; 32-bit runtime fixes | on hardware: 103/0, statedump md5 = reference for both rules files; per-tick time recorded (the `frames:` line) |
| **M2** (`claude/native-vita2`: built, awaiting hardware) | the game on the Vita: `cvfVitaGL` switch in the fork + regen, vitaGL renderer (Vita shaders and workarounds), SDL2 input (controller, front touch) and audio (streamed music), Lua menus, 960x544, 60 Hz logic with 30 or 60 fps drawing (`CVF_VITA_FPS`), optional render thread, per-tick logic/draw/memory logs. Cooked characters (faster loading) are not part of this build. | title -> select -> offline match at full speed; memory and frame times logged (section 11) |
| **M3** | online: sceNet start-up, Nearby (vita_ifaddrs), netplay against PC/Android/iOS (run_netplay.sh with a Vita side), HTTP lobby (SceHttp or curl) | netplay dump a350bc2d... with a Vita peer |
| **M4** | CPU: AOT compilation of cooked bytecode to C, string interning in the interpreter, renderer on a second core (M2 has it behind `CVF_VITA_RENDER_THREAD`; M4 makes it the default if the hardware numbers say so) | game tick <= 5 ms at 444 MHz (rollback headroom), 60 fps drawing if vitaGL allows; reference md5s unchanged |
| **M5** | replace the TinyGo build: packaging, LiveArea, docs; TinyGo build retired | user-tested parity with the TinyGo build |

The order matters: M2 before M4 because the renderer's CPU share on
hardware (TinyGo: 6-17 ms per drawn frame) decides how much of M4 is
needed; M1's hardware run decides the 444 vs 500 MHz question.

## 11. M2: verification and frame-time logs

### Verified (2026-11-22, Linux x86-64 container, no Vita)

| Check | Result |
| --- | --- |
| Regeneration | from a fresh `setup_ikemen.sh` tree (golang.org/x and gopkg.in modules from their GitHub mirrors, `GOSUMDB=off`): identical to the committed C; 4902 functions, 0 TODO, 79 stubs, 31 placeholders |
| Generated C against d290761 | 52 functions changed, 2 added, 1 moved. None in `render_gl33.go`/`font_gl33.go`. The changed functions outside the GLES renderer and the Vita stubs only gained `cvfVitaGL` branches (false off the Vita) |
| x86-64 | `cvf_tests` and `cvf_tests_ucontext` 103/0; reference dumps `54f8ae06...` (winner 1, 287,752 lines) / `4334350c...` (winner 2, 224,158 lines) |
| armv7 (qemu-arm, the Vita's code generation), aarch64 (qemu) | `cvf_tests` and `cvf_tests_ucontext` 103/0; both reference dumps |
| Windows (Wine 9) | `cvf_tests.exe` 103/0; both reference dumps |
| Netplay | `run_netplay.sh` PASS, dump `a350bc2df11a03519f7d65d278844432` (direct, lag, loss) |
| Go cross-check | `go build -tags cvf,cvfgohead` builds again (boundTexture); its reference fight gives `54f8ae06...` |
| Vita ELF/VPK | `cvf_vita` links with SDL2 2.32.8, vitaGL, vitaShaRK, SceShaccCgExt, libmathneon; `cvf-native-m2.vpk` |
| VPK image in vemu (headless mode) | unit tests 103/0; reference fight `54f8ae06...`, winner 1 (753 s of emulation) |
| VPK image in vemu, game mode, host GL (`VEMU_GL=1`) | see "vemu, game mode" below |
| Vita renderer paths on the desktop (`cvf_vitasim`, Mesa llvmpipe, Xvfb) | quick match Alucard vs Richter drawn correctly at 960x540 (stage, characters, effects, lifebars, fonts) in 30 fps mode (150 of 300 ticks drawn) and 60 fps mode; title -> ARCADE -> character select -> fight through Ikemen's own scripts and screenpack (with the reference data, that arcade fight stays at the round start, and the GL 3.3 desktop game does exactly the same with that data) |
| Render thread | `gles_thread_test`: 8 frames pixel-identical to direct calls; `cvf_vitasim`: statedump identical with and without the thread; lockstep run (the same 60 fps ticks drawn in both runs): **121 of 121 frames pixel-identical**; engine-thread drawing time 0.2-0.5 ms per frame with the thread against 18-23 ms without (llvmpipe rasterises on the render thread) |

`cvf_vitasim` (Linux build, `tests/vita_sim/`) runs the game with
`cvfVitaGL` set on an OpenGL 2.1 compatibility context, through the same
hooks as the Vita: the Vita shaders, batching, client arrays, shader
clipping, the 30/60 fps modes and the render thread. Two differences come
from the desktop. vitaGL reads texture rows tightly packed, so the simulator
sets `GL_UNPACK_ALIGNMENT` 1 once; otherwise desktop GL shears one-byte rows.
And Linux file systems are case-sensitive while the Vita's are not: with the
glob fallback skipped, as on the Vita, the game opens `pixel.sff` for
`Pixel.sff`, so the test game directory needs lower-case aliases.

    cmake --build build/release --target cvf_vitasim gles_thread_test
    xvfb-run -a build/release/gles_thread_test direct; xvfb-run -a build/release/gles_thread_test thread
    cd testgame && CVF_SHOT_DIR=shots CVF_SHOT_EVERY=150 xvfb-run -a ../build/release/cvf_vitasim . \
        -p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1
    # also: CVF_VITA_FPS=60, CVF_VITA_RENDER_THREAD=1, CVF_SHOT_TICKS=n, CVF_SIM_LOCKSTEP=1, CVF_SIM_FRAMES=n
    # menus: Modules = tests/vita_sim/menu_drive.lua in save/config.ini (title -> ARCADE -> select -> fight)

The test game was the reference data plus Ikemen GO's screenpack
(`github.com/ikemen-engine/Ikemen-GO-Screenpack`), its `external/script` and
fonts, and a `select.def` with Alucard and Richter. It stayed in the scratch
directory and is not in the repository.

### Not verified (needs the Vita)

- Everything vitaGL does on the device: shader compilation by
  libshacccg, the picture, texture memory, and whether the TinyGo workarounds
  still suffice with the native engine's draw order.
- Frame times and memory: the point of the M2 logs below.
- SDL2 audio on `sceAudioOut` and streamed music; the controller mapping;
  front touch (SDL's window-gated polling); the render thread on vitaGL
  (pinning to core 1, vitaGL used from a thread other than the main one);
  `CVF_VITA_VSYNC`.
- Loading time with the full game (compiled characters: 15-30 s each
  estimated, section 3; cooked characters are not in M2).
- The full CvF game's own menus (`options11.def`, CvF's Lua scripts) on the
  native engine with `cvfVitaGL`. They ran on the desktop native build
  (smoke test), but not on the Vita path.

### Frame-time and memory logs (`ux0:data/CvFNative/`)

`perf.log` (also copied into `cvf.log`):

    CvF native M2: ARM 444 MHz, bus 222, GPU 222; draw mode 30 fps in fights; game ux0:data/CvFNative/game
    mem before vitaGL: ... / mem after vitaGL: ...
    perf t=95s ticks 300 (fight 300) drawn 150 = 30.0 fps, mode 30 | logic avg 6.10 p95 8.00 max 9.80 ms/tick | draw avg 7.20 max 12.00 ms, swap avg 0.90 max 3.10 ms, draw+swap p95 9.90 ms/drawn frame | frame avg 16.70 max 34.20 ms, 3 over 17.5 ms
    perf render thread: vitaGL avg 8.10 max 14.00 ms/drawn frame        (with CVF_VITA_RENDER_THREAD=1)
    mem 5s: gc live 48.1 heap 70.2 hwm 120.0 MB, 12 collections, max pause 150 ms; newlib in use 9.8 MB; kernel free user 20.1 cdram 40.2 phycont 10.0 MB; vitaGL free ram 30.5 vram 60.1 MB

(the numbers are made up, to show the format). There is one `perf` line
every 300 game ticks (5 s at 60 Hz):

- **logic**: `action` + `uiAction`, CPU time per game tick, on the engine
  thread (`vitaPerfBegin/End(0)`).
- **draw**: `cueDraw` + `renderFrame` per drawn frame: the engine building
  the frame and, without the render thread, vitaGL's calls.
- **swap**: `EndFrame` (the last batch) + `vglSwapBuffers`, or with the
  render thread the hand-over, including the wait for the previous frame.
- **render thread**: the render thread's time in vitaGL per drawn frame,
  swap excluded.
- **frame**: wall time between ticks; ticks over 17.5 ms are late.
- `mem`: the collector (`gc_stats`), newlib's `mallinfo`,
  `sceKernelGetFreeMemorySize` and `vglMemFree`.

`frames.csv` has one line per game tick (header
`tick,t_ms,logic_us,draw_us,swap_us,render_us,frame_us,drawn,fight`; stops at
32 MB; `CVF_FRAME_LOG=0` turns it off). Menus have logic/draw 0: only the
fight loop sets those marks.

How to read it: 60 fps drawing is reachable when, in fights, logic +
draw + swap stays under 16.7 ms per tick (60 fps mode, `late` near 0). With
the render thread, the condition is logic + draw + swap < 16.7 ms and
render-thread time < 16.7 ms. 30 fps mode needs 2 x logic + draw + swap <
33.3 ms.

