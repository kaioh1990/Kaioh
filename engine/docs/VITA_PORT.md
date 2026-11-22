# PS Vita port of the native C engine

Status 2026-11-21, branch `claude/native-vita` (on `claude/native-integration`
4444062). Feasibility study and milestone M1: the engine builds with vitasdk
for the Vita, and the 32-bit runtime has been checked and fixed on armv7 with
the Vita's code generation. Nothing has run on a Vita yet.

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
looked up by name). The Vita shader set and workarounds are behind
`isVita`, a compile-time `false` in the native configuration, so they were
translated away. Plan (M2):

- In the fork patch: a run-time switch (`cvfVitaGL`, set by the Vita
  platform layer) that selects the GLSL 1.x shaders and the workarounds
  above; regenerate (`regen.sh`; no hand edits of generated C).
- `src/platform/vita`: `vglInit` at 960x544, GL entry points through
  `vglGetProcAddress` instead of SDL's, swap with `vglSwapBuffers`.
- vitaGL (2.x) and SDL2 (2.32.8) are vitasdk packages; both download from
  `github.com/vitasdk/packages/releases/download/master/<name>.tar.xz` (the
  container can reach it; vitasdk.org cannot be reached from here).
- Raw GXM (no vitaGL) is a later optimisation if vitaGL's 6-17 ms of CPU per
  drawn frame stays the limit; the renderer only draws textured quads.

## 6. Platform layer

| Area | Plan / state |
| --- | --- |
| Entry | `src/platform/vita/vita_main.c`: `sceUserMainThreadStackSize` 8 MB (the character compiler recurses deeply), newlib heap 48 MB, IEEE FP mode, `cvf_env.txt` -> environment, logs in `ux0:data/CvFNative/`. M1 runs `cvf_tests_main` and `cvf_headless_main` (the desktop mains, renamed by a compile definition). |
| Goroutines | SceFiber fibers (core/sched.c `CVF_VITA_FIBERS`); the main thread is the hub (fibers switch only thread <-> fiber), registers are spilled for the collector, IEEE mode per fiber. |
| Collector | arena = one memory block (`sceKernelAllocMemBlock`, zeroed), data roots from `__data_start` to a marker in `.noinit` (not `__bss_end__`: section 7), stack bounds from `sceKernelGetThreadInfo`, owner check by thread id. |
| Clock, sleep | `sceKernelGetProcessTimeWide` (monotonic, µs), `sceKernelDelayThread`. |
| Files | newlib's file API on `ux0:`; `chdir` works (newlib emulates a working directory). Game folder `ux0:data/CvFNative/<game>` (the TinyGo build uses `ux0:data/CvF`; separate folders, as with the Android apps). `os.Executable` is `app0:eboot.bin`. The file systems are case-insensitive, so Ikemen's case-insensitive lookup fallback (a directory glob per miss) should be skipped on the Vita as the TinyGo build does (110k directory reads per boot). |
| Input (M2) | SDL2's Vita backend as a game controller (the engine's SDL GameController code and `gamecontrollerdb.txt`), or `sceCtrl` directly; front touch through SDL touch events (the mobile touch-menu code). |
| Audio (M2) | SDL2 audio (Vita backend over `sceAudioOut`), queue-based like the desktop. Music streamed (the TinyGo build decoded fight music whole: 26 MB). |
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
    # SDL2 headers (headless: headers only) from the packages release
    curl -L -o sdl2.tar.xz https://github.com/vitasdk/packages/releases/download/master/sdl2.tar.xz
    mkdir pk && tar xJf sdl2.tar.xz -C pk && cp -r pk/include/* $VITASDK/arm-vita-eabi/include/

    cmake -S . -B build/vita -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
    cmake --build build/vita -j2                  # cvf_vita_headless (ELF, ~4 min)
    tools/package/vita_vpk.sh build/vita cvf-native-m1.vpk

`vita_vpk.sh` follows the TinyGo port's packaging: `vita-elf-create`,
`vita_velf_fixrel.py` (drops same-segment branch relocations: the Vita's
loader rewrites conditional BL into B, which crashed TinyGo builds 2-8),
`vita-make-fself`, param.sfo with `ATTRIBUTE2=12`, the CvF LiveArea art
from `vita/cvfsys`. It also writes the `.velf` next to the VPK (for vemu
and for symbolising core dumps).

### Running M1 on the Vita

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
| **M1** (this branch) | headless VPK: unit tests + reference fight; 32-bit runtime fixes | on hardware: 103/0, statedump md5 = reference for both rules files; per-tick time recorded (the `frames:` line) |
| **M2** | the game on the Vita: `cvfVitaGL` switch in the fork + regen, vitaGL renderer (Vita shaders and workarounds), SDL2 input and audio, Lua menus, 960x544, 30 fps drawing / 60 Hz logic, cooked characters to cut loading | title -> select -> offline match at full speed; memory and frame times logged |
| **M3** | online: sceNet start-up, Nearby (vita_ifaddrs), netplay against PC/Android/iOS (run_netplay.sh with a Vita side), HTTP lobby (SceHttp or curl) | netplay dump a350bc2d... with a Vita peer |
| **M4** | CPU: AOT compilation of cooked bytecode to C, string interning in the interpreter, renderer on a second core | game tick <= 5 ms at 444 MHz (rollback headroom), 60 fps drawing if vitaGL allows; reference md5s unchanged |
| **M5** | replace the TinyGo build: packaging, LiveArea, docs; TinyGo build retired | user-tested parity with the TinyGo build |

The order matters: M2 before M4 because the renderer's CPU share on
hardware (TinyGo: 6-17 ms per drawn frame) decides how much of M4 is
needed; M1's hardware run decides the 444 vs 500 MHz question.
