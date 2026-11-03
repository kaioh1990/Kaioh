# Castlevania Fighter on Ikemen GO: Rollback Compatibility Report

*Castlevania Fighter v0.8.5b, Ikemen GO develop branch (commit 6a09392, Oct 31 2026) plus two local fixes. Tested Nov 1, 2026.*

## Summary

- **The plan works.** Castlevania Fighter runs on Ikemen GO with its own lifebars, stages, effects, title screen and menus, and Ikemen's built-in rollback netcode (GGPO) runs underneath it.
- **Ikemen GO had two rollback bugs.** The stock engine would have desynced on every CvF character. Both bugs are fixed (see "Engine fixes").
- **After the fixes, none of the 93 roster entries desynced.** Every entry played a full AI match in Ikemen's built-in desync test, which re-simulates every 4 frames and compares checksums.
- **Nobody needs to be removed.** One boss (Stella & Loretta) is hidden from character select but stays in arcade mode.

## How the testing worked

Each roster entry played an AI vs AI match against Simon. The test used CvF's own fight screen (`bars.def`) and the entry's own stage from `characters.def`. Ikemen's offline sync test was on (`DesyncTest=1`, `DesyncTestFrames=4`, `DesyncTestAI=1`). In that mode the engine rolls back 4 frames on every frame, re-simulates them, and stops with an error if the re-simulated state differs in any way. This is a stricter version of what happens during a real online match.

- The test machine was a 2-core Linux cloud server with software OpenGL (Mesa llvmpipe).
- The test covered all 93 entries in `[Characters]`: 47 playable characters and 46 boss entries.
- Four entries are listed in `characters.def` but have no files in this release: Legion (disabled by the author), Karasuman, Adramelech and PMaster. They were skipped.

## Results (final pass, both fixes applied)

| Result | Count | Entries |
|---|---|---|
| Passed with no desync | 90 | Everyone not listed below |
| Rollback desync | **0** | none |
| Match never ended (no desync) | 3 | HAlucard, John (1 of 5 runs), Stella & Loretta boss as P1 |

### The three that never ended

- **HAlucard (Hellsing Alucard).** He never desynced. In AI vs AI his matches never finish: the round timer doesn't count down under CvF's lifebars, so only a KO ends the round, and the AI never lands one. Across all runs he went through 20+ minutes of rollback checks without a problem. He stays in.
- **John Morris.** The match stalled once in 5 runs, with John frozen in a teal (petrified-looking) state and Simon off screen. It wasn't a desync. The other 4 runs finished normally in 48–88 s, on builds with and without the second fix. It looks like a rare AI standoff. Watch for it in real play.
- **Stella & Loretta (boss, `Stella/Stella2.def`).** When she's player 1, the pre-fight intro never ends (the opponent waits in intro state 190 forever). As player 2, which is how arcade mode uses her, she passes. The cause is CvF's own intro scripts (`data/intros.st`), not the engine. **Fix:** she's marked `hidden=2` in the port's `characters.def`, so she can't be picked on the select screen but stays as the arcade boss.

### Content problems in CvF itself (not engine)

- **DChick** points to `stages/DimitriBG.def`, and **Galamoth** to `stages/GalamothBG.def`. Neither stage exists in the release, so both crash at load. On a working stage both pass. The arcade bonus stage `stages/Entrance2.def` is also missing.
- **NSuccubus** is listed with different capitalization from her folder (`Nsuccubus`). This would break on Linux/SteamOS, but Ikemen's case-insensitive file lookup handles it.
- Hundreds of "unknown parameter" warnings, such as `pausemovetime` and `supermovetime` on some controllers. These are harmless: MUGEN 1.1 ignored them too.

## Engine fixes (Ikemen GO)

Both are general rollback bugs in Ikemen GO, not CvF-specific. They should go upstream to the Ikemen team.

### 1. `ScreenPos` used a stale camera during rollback

- **Symptom:** Grant vs Simon desynced about 2 s into the match. CvF's shared `common1.st` stores `screenpos x` into `fvar(15)` every frame, and that value differed by 6.25 between the original run and the re-simulation.
- **Cause:** `ScreenPos X/Y` reads `sys.zoom.resultPos`, which was only recomputed in `renderFrame()`. Rollback re-simulates several frames per rendered frame, so those frames saw the camera from the last drawn frame.
- **Fix:** the zoomed camera is now computed inside `System.action()` as part of each simulated frame. `renderFrame()` reuses that result. File: `src/system.go`.

### 2. Projectile animations only advanced while drawing

- **Symptom:** Remilia (Touhou) desynced every time. The only differing value was the RNG seed, because a different number of projectiles spawned. Over 100 projectiles that should have been removed weren't removed in the re-simulation, so she hit the 256-projectile cap and spawned fewer bullets.
- **Cause:** `Projectile.cueDraw()` called `anim.Action()` and `anim.UpdateSprite()`. Animation progress decides when removed projectiles are cleaned up and which Clsn boxes are active. Rollback only calls `cueDraw()` once per rendered frame, and it's skipped entirely on frame-skip.
- **Fix:** a new `Projectile.animStep()` runs at the end of `System.action()` for every simulated frame. `cueDraw()` only draws. File: `src/char.go`.

**Patch:** `ikemen-cvf-rollback-fixes.patch`, 2 files, +41/−11 lines, applies to Ikemen GO develop 6a09392.

## Changes to the CvF files (port copy only)

Your original game files are unchanged.

- **`data/options11.def`:** added Ikemen's network menu. The main menu now reads ARCADE / VERSUS / **ONLINE**, with Host Game / Join Game / Versus underneath. This was confirmed on screen.
- **`data/characters.def`:** Stella & Loretta boss set to `hidden=2`.

## Cross-platform notes (Mac / Steam Deck / ROG Z13 SteamOS / Windows)

- Ikemen GO has native builds for macOS (Apple Silicon), Windows x64 and Linux x64. The Steam Deck and the Z13 on SteamOS use the Linux build.
- **Risk: Apple Silicon vs Intel/AMD.** Go's compiler fuses multiply-add into single FMA instructions on ARM64 but not on x86-64. That can make float math differ slightly, and in rollback tiny differences grow into desyncs. Ikemen does nothing about this today. **Recommended:** on the Mac, run the *Intel (x86-64) macOS build* through Rosetta 2, so all four devices do identical math. The other option is a native ARM build compiled with FMA fusion disabled, which needs code changes. **Not yet tested:** this needs a real Mac vs PC online session.
- The three x86-64 devices (Windows, Deck, Z13) should match each other.

## What hasn't been verified yet

- **A real online match between two machines.** Everything above used Ikemen's offline sync test, which is stricter, but real netplay also involves the network handshake, input delay and NAT/port forwarding.
- **The character select screen.** On this server's software rendering, loading the select screen with the full roster takes 3–8 minutes, and Ikemen's attract demo kept interrupting it. On real hardware it should load in seconds.
- **Mac (Rosetta) vs PC cross-play.**

## Next steps

1. Build the patched Ikemen GO for Windows, macOS (Intel, for Rosetta) and Linux, and package it with the CvF content and the two edited files.
2. Run a real online test between two of your devices: first Windows vs Steam Deck (both x86), then Mac vs Windows.
3. Submit both engine fixes upstream to ikemen-engine/Ikemen-GO.

## Update (Nov 1, 2026): Apple ↔ PC cross-play and iPhone/iPad

### Deterministic math across CPUs (fix 3)

- **Finding:** Go's `math.Exp`, `math.Log`, `math.Hypot` (and `Pow`/`Log2`, which use them) run hand-written assembly that differs between Intel/AMD and Apple chips. Over 2 million random inputs, Intel's results were off by up to 2 ULP (Exp) and 5 ULP (Pow) from the portable code, and Apple Silicon's by 1 and 3 ULP. Different answers on each side would desync rollback.
- **Fix:** new `src/cvf_detmath.go` has pure-Go copies of these functions from the Go standard library. All gameplay call sites use them: `bytecode.go` (CNS `exp`/`ln`/`log`/round), `char.go`, `common.go`, `fightscreen.go`, `system.go`. Rendering and sound code is unchanged. Apple builds also turn off FMA fusion (`-d=fmahash=n`).
- **Verified:** the CI job `detmath-check` hashes 10 million results on Linux x64, Windows x64 and macOS arm64. All three produce the same hash. Remilia, the projectile-heavy case, still passes the offline desync test on the new engine.
- **Result:** the native Apple Silicon Mac app and the iPad can now play against Windows, Steam Deck and Z13, so Rosetta is no longer needed. One risk remains: a float-to-int conversion of an out-of-range value behaves differently on arm64 and x86. It is unlikely in normal play.

### iOS / iPadOS build

- **How it's built:** the CI `ios` job runs on macos-26 with Xcode 26. It builds the patched engine with the Jesuszilla/ikemen-ios wrapper (pinned at 60606d6), which `ios/customize.sh` turns into "CvF Online":
  - bundle id `com.kaioh1990.cvfonline`, with no signing team hard-coded;
  - minimum iOS 17;
  - Files/Finder sharing switched on;
  - only the engine scripts and the controller DB ship as default assets. The stock asset list would have unzipped Ikemen's `fight.def`/`common.snd`/… on top of CvF's own files.
- **Releases (`cvf-online-ios`):**
  - `cvf-online-ios.ipa`: unsigned, for Sideloadly, AltStore or SideStore.
  - `cvf-online-ios-xcode.zip`: a ready-to-sign Xcode project.
- **Install:** copy the CvF Online folder to the device through Finder's Files tab, then pick it in the app.
- **Not yet tested on a real device.**

## Update (Nov 1, 2026, late): PS Vita port, phase 2 results on hardware

Tested on the user's Vita (firmware 3.60, HENkaku/Ensō). 11 test builds.

**What works on the Vita now (test app, TinyGo-compiled Go):**
- Go runtime, goroutines (spawn, channels, sleep), file I/O, screen output, buttons, clean exit.
- **Rollback math is bit-identical to the PC.** All 21 math categories match, and so does the full rollback-math hash (`e7ccaee0656fb0ad`). So Vita-vs-PC online play is possible.
- About 232 MB is free in one block. A CvF match needs about 56 MB with the new sound loader.

**Vita-specific problems found and fixed:**
1. The loader needs exactly 2 segments, page-aligned data and free space after the code. The first build rebooted the console.
2. lld's long-branch thunks used absolute addresses that broke when the Vita relocates the module. Fixed with `--pic-veneer`.
3. The Vita kernel rejects system calls from stacks it doesn't know about. Goroutines now run as **SceFiber** fibers.
4. **Threads start in flush-to-zero FP mode** (`FPSCR=0x03000000`). This was the only math difference from the PC (denormals). The runtime now sets IEEE mode at startup and in every fiber.
5. newlib on the Vita uses a 32-bit `time_t`. Go's runtime expects time64 structs, and the mismatch broke `time.Sleep` and `time.Since`.
6. TinyGo substitutes C-library trig functions, so the engine now uses its own copies of `Sin`/`Cos`/`Tan`/`Atan`/`Atan2`/`Asin`/`Acos`. These are bit-identical to Go's, so PC, Mac and iPad results are unchanged.

**Open risks:**
- **CPU speed.** A simple float benchmark ran about 70× slower than a PC. Homebrew defaults to 333 MHz; 444 MHz is available. Real engine performance with rollback is still unknown.
- **Renderer.** The Vita's SDL2 has no OpenGL support, so the plan is to start vitaGL directly. vitaGL has VAOs, FBOs, buffer mapping and blits. It lacks 3D textures, multiple draw buffers and MSAA, which CvF doesn't need. Shaders need Vita-compatible GLSL versions, and runtime shader compilation needs `libshacccg.suprx` on the device.

**Next:**
- Add the Vita window layer (vitaGL init and swap, 960×544, SDL for input and audio only).
- Port the sprite and font shaders.
- Copy the game data to `ux0:data/` and boot to the title screen.
- Measure FPS.

## Update (Nov 2, 2026): PS Vita port, engine bring-up

**Status:** the full engine runs title → character select → a fight inside a PS Vita user-mode emulator I built for this (`vita/vemu`). It runs the real eboot (Unicorn CPU, Sce imports in Python, vitaGL stubbed). On real hardware, builds 2–6 crash right after the config file is read, in the step that clamps settings to valid ranges. The emulator does not reproduce this crash. Engine build 7 adds tracing and a code-integrity check to pin it down.

**Fixed with the emulator:**
- **TinyGo garbage collector:** switched from conservative to precise. On 32-bit with a 272 MB heap, the conservative collector treated pixel data as pointers and kept ~150 MB of dead sprite buffers alive. Live memory at the title screen went from 185 MB to 38 MB.
- **Scheduling:** TinyGo goroutines are cooperative. The frame loop now yields every frame, so background loaders run (before this, the game hung at character select).
- **OGG music:** every OGG failed to decode. libm `pow` made the vorbis `lookup1Values` result one too small. Patched with integer math.
- **Missing GL functions:** vitaGL lacks some of the GLES3 functions the engine calls. Those now resolve to no-ops instead of NULL.
- **File seeking:** `Seek` was missing under TinyGo. It now goes through `_llseek`.
- **Memory and loading:**
  - Texture atlases capped at 2048, and cleared in bands.
  - Sprite uploads run immediately instead of being queued.
  - fightfx.sff is loaded once and shared, not loaded twice.
  - Garbage collection runs at load checkpoints.
  - File reads are buffered.
  - newlib's heap is cut to 40 MB; the Go heap is sized from free memory.

**Open:** the hardware-only crash in config normalize, plus real-device performance, controls and networking.

## Update (Nov 2, 2026, evening): PS Vita port, builds 9–16 on hardware

**Status:** on the Vita the game boots through the intro and title screen to character select, and loads a training match. The match is still a black screen with music playing, and it runs very slowly. Build 16 has fixes aimed at both and is waiting for a hardware test.

**Hardware crashes, found from Vita core dumps and fixed:**
- **Config-normalize crash (builds 2–8):** the Vita loader rewrites `R_ARM_JUMP24` branch targets as plain `B`, which turns `bleq` into `beq`. `velf_fixrel.py` now drops same-segment branch relocations before packaging.
- **False stack-overflow abort:** SceFiber writes `0xEEEEEEEE` into the lowest word of each fiber context, which tripped the goroutine guard check.
- **Crash after the intro screens:** vitaGL copies a whole texture when `glTexSubImage2D` updates one the GPU used in the last 4 frames, and doesn't check whether that allocation succeeded. With a 2048² palette atlas this was a 16 MB copy that failed. The palette texture is now 256², and atlases are capped at 1024.
- **Out of memory and freezes entering a fight:** fight music was decoded whole into RAM (~26 MB, in a goroutine that never yields). The Vita now streams music instead.
- **Hangs while loading:** `while loading() do end` in Lua never let the loader goroutine run. `loading()` and `preloading()` now yield.

**Major gameplay bug: negative constants compiled to 0 on 32-bit ARM.**
- **Cause:** `BytecodeExp.appendValue` converted a constant's float64 value straight to `OpCode` (uint8). x86 and arm64 wrap -1 to 255; 32-bit ARM saturates it to 0.
- **Effect:** every CNS/ZSS constant from -1 to -128 became 0 on the Vita.
- **Example:** explods with `pausemovetime=-1` froze during every pause and were never removed. A Paranoia-stage fight piled up 500+ explods: ~400 draw calls per frame instead of ~65, then out of memory. This is a likely cause of the black screen too.
- **Fix:** convert through `int8`. A type-checker scan (`vita/tools-f2u`) found no other float→unsigned conversions in the engine that are affected.

**Performance work** (from the hardware frame-time log plus an instruction profiler in the emulator):
- Renders at 960×540 natively. FightAspect, MSAA, 3D models, shadows and external shaders are forced off on the Vita.
- **TinyGo GC:** `findHead` walks back from interior pointers, and line substrings of multi-MB `.st` files made every collection quadratic (62% of character compile time). Fixed two ways: large files' lines now get their own allocations, and `findHead` skips 16 blocks at a time. In the emulator, boot to fight went from 73 s to 40 s.
- **State-controller dispatch:** TinyGo compares the type against all ~160 controller types on each interface call (13% of fight frame time). Each method is now resolved once.
- `BytecodeStack.Push` no longer calls append; that call was 17% of frame time.
- Ranging over `sys.chars` no longer copies the array.
- Per-frame allocations cut in `BytecodeExp.run`, `RenderSprite`, `renderWithBlending` and `drawQuads`, plus redirect and map-set closures and blend lookup maps.
- The audio queue is kept ~4 buffers deep, because music was choppy.

**Diagnostics in the log:**
- A `perf:` line every 300 frames: fps, worst frame, skipped draws, time waiting on the GPU, heap size.
- Timestamps on every line.
- Pixel probes of the fight image, taken before and after the final scale to the screen.
- An explod count.
- Optional `vita_diag.txt`, which cycles through skipping the stage, characters and lifebars.
- Optional `vita_args.txt` for command-line arguments, for example a quick match.

**Open:**
- Black fight screen (build 16 untested).
- Real-hardware fps after these fixes.
- Load time: each character still compiles CvF's 69k-line `common1.st` (~15 s per character in the emulator).
- Controls mapping.
- Networking.
- Vita-vs-PC online needs matching GameWidth/GameHeight, because Ikemen syncs those settings.
