# Native engine gap list (2026-11-20)

## Status on `claude/native-gaps-impl` (2026-11-20)

| # | Item | State |
| --- | --- | --- |
| 1 | Lua API parity (the 18 `cvf*` functions) | **Done.** Not hand-written stubs: regenerated from the fork, so the bindings are the Go code itself (cvf_nearby.go, cvf_online.go, cvf_p2p_other.go for the desktop answers). Checked by `cvf_nearby_test` and the menu smoke test (`tests/smoke`): title → Versus character select → Online > Host Nearby → Join Nearby passes on macOS arm64; the previous native build fails it at character select (`cvfTouchTap` is nil). |
| 2 | FightAspect reset | **No reset happens, and the 41233ed change is reverted** (claude/native-netplay). `normalize()` does not touch the aspect; FightAspect is a strict setting (compared, never applied from the host). 41233ed's diagnosis was also wrong: `Config.Save` writes `c.IniFile`, not the struct, so native never saved its forced 0,0 into `save/config.ini`. Its only effect was re-applying the rules' aspect on `loadGameOption(file)`, which Go's `loadConfig` does not do; native follows the Go fork again. Netplay is unaffected (startup aspect: old rules 0,0, cvf-online-5 4:3, as before). |
| 3 | Regen toolchain in the repo | Done before this branch (`tools/gen/setup_ikemen.sh`, DEVSETUP.md). |
| 4 | Rebase the fork on d4d84f7, regenerate, shims | **Done.** New shims: crypto/rand (Read, Int, Reader), math/big (NewInt, Int64), net (CIDRMask, IPMask.Size, IP.Mask, ResolveUDPAddr), IPNet layout. Also fixed: `time.Ticker` never ticked (Nearby re-sweep, lobby heartbeat), UDP `WriteTo` failed on a full send buffer instead of waiting (every other Nearby sweep lost), `fmt.Fprintf` ignored its writer (status screens lost lines, the GGPO desync log was empty). Regen fails on new placeholders (already in place); placeholder list unchanged (31). |
| 5 | Cross-play verification vs a Go build | **Done** (claude/native-netplay). Go 1.27.4 build of the same headless runner (`cvf_gohead.go`, amd64 on the Mac): `cvf` tag gives 54f8ae06… (old rules) and 4334350c… (cvf-online-5), byte-identical to native. Desktop configuration (no `cvf` tag: no vitaPerf paths, as the shipping PC/iOS/Android builds): 4334350c… with cvf-online-5; with the old rules 6924075c…, only because config.ini says FightAspect -1,-1 where native forces 0,0 (with 0,0: 54f8ae06…). The roster-wide desync test is still not run. |
| 6 | End-to-end online test | **Done for direct IP** (claude/native-netplay, `tests/netplay/run_netplay.sh`): full rollback matches (TCP handshake, sync settings, synchronize, GGPO over UDP; cvf-online-5, Alucard vs Richter, scripted inputs), direct and through a delay/jitter/loss relay. Native ↔ native (Linux x86-64, macOS arm64), native ↔ Go `cvf` tag and native ↔ Go desktop configuration, both directions: both sides' per-frame dumps identical and identical across network conditions (a350bc2d…, 6,431 frames, up to ~4,700 frames re-simulated), no GGPO desync. Bugs fixed on the way: map-delete leak (OOM in a rollback match), dropped `defer close`/`defer delete` (GGPO shutdown hang), log.Logger and fmt.Fprintln ignoring their writer, os.OpenFile(O_CREATE) failing on a new file (GgpoLogsEnabled ended every match). Not covered: lobby POST, UPnP, NAT, a windowed match (the test is headless), more than one match per session. See the open item below on GGPO synchronization under loss. |
| 8 | `cvf-files/` from claude/android | **Done** (cvf-online-5 rules, Nearby options11.def, Intro door fix). |
| 7, 9–14 | | Open, unchanged. Item 11/12 (mobile front ends, Apple P2P, touch taps from the UI thread, native name box) stay stubs on desktop by design (cvf_p2p_other.go). |

### Collector: derived pointers on 32-bit ARM (open, 2026-11-21)

Found while verifying the runtime review fixes (ucontext roots, 32-bit
register spills) with `cvf_tests` cross-built for armhf and run under
`qemu-arm` (GCC 13, Thumb-2, -O2). With the fixes the suspended stacks and
registers are scanned (the review's root tests pass), but one existing check
still fails: `test_sched.c` "data on a suspended goroutine's stack survived
collection". The goroutine loops over `data[i]` after the call, and GCC's
pre-indexed loads (`ldr r1, [r4, #4]!`) keep only `r4 = data - 4` live across
`go_chan_recv`: a pointer just *below* the object, which a conservative
scanner attributes to the neighbouring block (or to nothing). x86-64 keeps
`data` itself (in `rbx`). Any GC object whose only reference is such a
derived pointer, across a call that collects, can be freed: this matters for
a 32-bit ARM build (armeabi-v7a Android, the Vita) only, and only when the
base pointer is not also held elsewhere (slices and structs normally are).
Not fixed: options are recognising pointers up to a small offset below an
object when scanning stacks and registers only (not the heap), or a GCC
option that avoids pre-indexed writeback addressing; both need measuring.

### Online: still open after claude/native-netplay

| Item | State |
| --- | --- |
| GGPO synchronization under packet loss | **Fixed on `claude/native-ggpo-sync`** (see below; in the Go fork patch, so phone and PC builds get it with their next build). |
| Lobby, UPnP, NAT, public IP | Not tested (direct IP only; no external services were used). |
| Windowed online match through the Lua menus | Not run; the test drives the same Go code headless (main.lua's command-line `-ip` path). The menu smoke test covers the Online menu up to Nearby host/join. |
| Several matches per session (rematch, `postMatchSetup` → new RollbackSession) | **Done** (claude/native-netplay2): `run_netplay.sh` with `MATCHES`, below. |
| Roster-wide desync test (114 characters) native vs Go | **Partly**: the sweep is scripted (`run_roster.sh`), but this container has only the two reference characters (cvf-refdata.tgz); every pair of those ran, native - native and native - Go. The full roster must run where the full game is (below). |
| Content fingerprint | **Done** (claude/native-netplay2, both fork patches): rules, data/, characters and stages, below. |
| TCP pre-match "Synchronization error" (once in ~270 runs) | **Fixed** (claude/native-netplay2, both fork patches): a race in delay-netplay's `update()`, below. |

### Windows (claude/native-windows)

Cross build with mingw-w64 (DEVSETUP.md, "Windows"). Before this branch it
had never been built: it failed to compile (msvcrt has no `timespec_get`,
no `nanosleep`). **Everything below ran under Wine 9.0 on Linux, not on
real Windows.**

| Item | State |
| --- | --- |
| Build: cvf.exe, cvf_headless.exe, cvf_tests.exe, cvf_nearby_test.exe, cvftool.exe | **Done** (Ubuntu mingw-w64 GCC 13, msvcrt; SDL2 2.30.9 MinGW package). Needs only system DLLs + SDL2.dll. No MSVC build, no Windows on ARM build. |
| Collector on Windows | **Fixed, was a real bug.** Goroutines are fibers there, and suspended fibers' stacks were never scanned (a collection on a fiber also scanned the wrong range): data held only by a waiting goroutine (netplay, GGPO, lobby workers) could be freed. cvf_tests caught it under Wine. Now registers are spilled with setjmp before SwitchToFiber and each fiber's stack range is scanned; finished fibers are deleted. |
| Timers | QueryPerformanceCounter (monotonic), GetSystemTimePreciseAsFileTime (wall); idle sleeps use a high-resolution waitable timer, else timeBeginPeriod(1) (Sleep alone rounds the 1 ms socket polls up to 15.6 ms). Frame pacing and input latency on real Windows not measured. |
| Nearby interfaces | **Fixed.** GetAdaptersAddresses as Go: real names, flags, MTU, MAC, every IPv4/IPv6 address with its prefix length (was one pseudo-interface, ≤ 4 host-name addresses, fixed /24). Non-LAN adapters (PPP, mobile broadband, tunnel, VPN/overlay drivers, VM/container host-only switches without a gateway) get the Unix name prefix cvf_nearby.go filters on, so Nearby skips them where Go's Windows build does not; the Mobile Hotspot adapter stays. Under Wine: a /23 gives 510 probe targets, a tun device is skipped. The VPN/VM classification by adapter description and the gateway rule are untested on real adapters (Wine shows Linux interfaces and does not support AF_UNSPEC with gateways, so it also ran IPv4 only). |
| crypto/rand | BCryptGenRandom checked; failure is a fatal error as in Go 1.24+ (also: Linux uses getrandom, no clock fallback). |
| Unit tests (Wine) | 92 checks, 0 failures. |
| Nearby (Wine) | cvf_nearby_test passes; host Wine ↔ scan Linux native and the reverse find each other over loopback. |
| Netplay (Wine) | **PASS** with `run_netplay.sh`: Windows host (Wine) ↔ Linux native guest and Linux host ↔ Windows guest, scenarios direct, lag:40:15:0, loss:20:10:8; every side and scenario gives dump `a350bc2df11a03519f7d65d278844432` (6,431 frames, up to ~4,100 frames re-simulated), the same as Linux ↔ Linux. The Windows side re-simulates more (Wine is slower and the run is unpaced). |
| Windows Firewall | Not handled. Hosting (TCP listen, UDP 7520 Nearby, GGPO UDP) makes Windows ask the first time; if the player declines, a LAN guest cannot reach the host. Nothing tells the player. |
| Windowed game, menus, WinHTTP lobby, gamepads, real Windows 10/11 | Not run (no display or GL in the container, no full game there). Next: the menu smoke test (`tests/smoke`) on a real Windows PC, then a Windows ↔ Mac/Linux match over a real LAN. |

Found on the way, all platforms: `go_strcat(go_cstr(buf), "")` does not
copy (go_strcat returns an argument unchanged when the other is empty), so
Linux interface names pointed into freed `getifaddrs` memory and
`IPNet.String`, `UDPAddr.String`/`TCPAddr.String` and the HTTP status
returned stack buffers; now `go_cstr_copy`. Not fixed (shared Go
behaviour): the first Nearby sweep of a big, mostly empty subnet is slow
on Linux, because probes to addresses without an ARP answer fill the
socket's send buffer until ARP gives up, and WriteTo waits (as Go does).
With an empty /23 added in the container, a sweep took more than 6 s and
the test host on 127.0.0.1 (probed after the subnets) was not found within
cvf_nearby_test's 6 s; with the /24 alone it is found at once.

### GGPO sync handshake under packet loss (claude/native-ggpo-sync)

The problem (GGPO's own code, Go and native alike): a peer that had finished
synchronizing dropped the other after `Rollback.DisconnectTimeout` (3 s)
without packets, while a peer still synchronizing retried a lost sync round
trip only every 2 s (`SyncRetryInterval`). Two lost sync requests in a row
and the running side disconnected ("EventCodeDisconnectedFromPeer"); the
other then failed with the rollback sync timeout.

The fix is in GGPO's `internal/protocol/protocol.go`, timing only:

- a lost sync request or reply is resent after 200 ms (was 2 s), with the
  same random as the copy before it, so a late reply to an earlier copy
  still counts and round trips longer than 200 ms still synchronize; the
  retry has its own timer (`lastSendTime` also moves with replies to the
  peer's requests);
- a synchronized peer applies a disconnect timeout of at least 15 s, and no
  "connection interrupted" notice, until the other has sent anything but
  sync packets (an input, keep-alive or quality report: only a running peer
  sends those). After that the configured timeouts apply unchanged.

GGPO is a module (`github.com/ikemen-engine/ggpo`), so the fork patch now
carries a copy of it in `third/ggpo` (sources only, go.mod unchanged; the
change is marked "CvF Online", see `third/ggpo/CVF_CHANGES.txt`) and a
`replace` in go.mod. `patches/ikemen-cvf-online.patch` and
`tools/ikemen_headless.patch` both have it; both still apply to 6a09392 with
`git apply`, and regenerating from a fresh `setup_ikemen.sh`-style tree gives
the committed C. ggpo's own tests: the same 12 upstream failures with and
without the change, no new ones.

**Compatibility with unfixed builds (the phones today): yes.** Packet
formats, the sync protocol (5 request/reply round trips) and everything
after synchronization are unchanged; an unfixed peer just sees retransmitted
sync requests, which it answers as before. In a mixed match the fixed side
covers both cases: when it is the slower one, its 200 ms retries keep the
unfixed side's 3 s timer from expiring; when the unfixed side is slower
(2 s retries), the fixed side waits up to 15 s for it. Game state and
determinism are untouched (reference fights and the netplay dump unchanged).

Results (Linux x86-64 container, `run_netplay.sh` with one `:sync` scenario
per run, relay 20 ± 10 ms, `RELAY_SEED` 1-20, unpaced; Go = desktop
configuration, Go 1.27.4; every PASS gives dump a350bc2d…):

| Pair (host - guest) | 8 % loss incl. sync | 25 % loss incl. sync |
| --- | --- | --- |
| **Before:** native - native (unfixed) | 19/20 | 9/20 |
| **Before:** Go - Go (unfixed) | | 12/20 |
| native - native (fixed) | 20/20 | 20/20 |
| native - Go (both fixed) | 20/20 | 20/20 |
| Go - native (both fixed) | 20/20 | 20/20 |
| Go - Go (both fixed) | | 20/20 |
| native fixed - Go **unfixed** | 20/20 | 20/20 |
| Go **unfixed** - native fixed | 20/20 | 19/20 (1) |
| Go fixed - Go **unfixed** | | 20/20 |
| Go **unfixed** - Go fixed | | 20/20 |

Every failure before the fix was the GGPO one (the synchronized side's
EventCodeDisconnectedFromPeer). (1) is not GGPO: Ikemen's TCP pre-match
check ("Synchronization error" in netplay.go after "Network synchronized",
before any UDP packet); the same seed passed when rerun. It was rare
(seen once in ~270 runs) and is fixed on claude/native-netplay2 (next
section). The default scenarios (`direct lag loss`) pass for
native - native, native - Go and Go - native. Reference fights on Linux:
54f8ae06… (old rules) and 4334350c… (cvf-online-5), unchanged. At 25 % loss, matches
with an unfixed side take longer to start (its 2 s sync retries): ~47-55 s
per run instead of ~33-41 s with both sides fixed.

### Sessions, the TCP race, content fingerprint (claude/native-netplay2)

**TCP pre-match "Synchronization error" (fixed).** Delay-netplay's
`NetConnection.update()` (upstream Ikemen GO code, so the Go PC and phone
builds have it too) computed `foo`, the last frame both peers have, then
waited in `sys.await` and left as soon as the connection was no longer
`NS_Playing`. When the peer's last input frame and its stop marker both
arrived during that wait (the peer had advanced and called `Stop()`), `foo`
was stale: this side left without the frame the peer had counted, its
`nc.time` stayed one behind, and the next `Synchronize()` failed its time
check. Reproduced with `run_prematch.sh` (Go host unfixed, native guest,
`LOAD=3` on two cores): 4/100 and 2/60 runs failed; an instrumented Go
build logged "update left with stale foo: time=1 foo=1 fresh=2
st=NS_Stopped" and "time check: local=1 remote=2" in both failures. It
needs the waiting side to be descheduled for more than the peer's 1 ms
input-goroutine sleep between its last frame and the marker: a Go build on
a loaded machine (real threads), or any build whose `await` waits long (a
paced windowed game). Unpaced and unloaded, 0/80 (FPS=60, TCP lag 30 ± 20
ms) on native.

Fix (netplay.go, both fork patches): when the peer has stopped, recompute
`foo` after the wait and consume the frames already received before
leaving. Timing only, no protocol change. After it: 100/100 under the same
load (Go host fixed, native guest fixed). With an unfixed peer the race is
still possible on the unfixed side when it is the one waiting (seen once in
the content tests below, old Go host - new native guest; 20/20 on rerun).

**Several matches per session (done).** `run_netplay.sh` `MATCHES`
(cvfheadless.go `CVF_NET_MATCHES`): rematches, back to the character select
(60 delay-netplay frames), new characters; a new RollbackSession on the
same UDP ports for each match. Results (native - native, Linux, unpaced):

| Session | Scenarios | Result |
| --- | --- | --- |
| 6 matches: A-R, A-R (rematch), R-A, R-R, A-A, A-R | direct | PASS, both sides identical in every match; match 1 = a350bc2d… |
| 8 matches: A-R, A-R, R-A, R-A, R-R, A-A, A-R pal 3/4, R-A pal 5/6 | direct; 8 % loss incl. sync + TCP 20 ± 10 ms | PASS, every match identical on both sides and in both scenarios (5,013 to 54,041 frames per match, up to 31,000 re-simulated) |
| 16 matches (2 rounds, varied pairs and palettes) | direct; 8 % loss + TCP lag | direct: 15 of 16 identical, the 16th not comparable (the disk filled while the guest wrote its dump: a truncated prefix, not a desync); loss: the 6 matches played before the run was stopped identical to direct |

Memory (native collector, `GC_TRACE=1`): the live heap at the start of
each match stays at 60-70 MB over 8 matches (load of the next pair; mirror
matches included), so the live data does not grow. Inside a match the
collector runs every few seconds (rollback re-simulation allocates ~3 GB
per long match; 40-190 ms pauses), and the in-match peaks vary: the test's
own per-frame dump buffer (up to ~80 MB for a 54,000-frame match) plus
pointer-rich bursts of 70-140 MB that are freed when the match ends
(probably save-state graphs kept by conservative roots). The collector
never returns pages, so RSS follows the highest peak so far: 494 → 663 MB
(host) and 494 → 878 MB (guest) over the 8-match session, and in the
16-match session it rose in steps to 1.09-1.22 GB (host) and 1.58 GB
(guest), flat for the last 9 matches. Without the dump (`NODUMP=1`, 10 matches, 1 round,
pairs and palettes varied): live at match start 54-65 MB in every match;
in-match peaks 115-300 MB; RSS host 413 MB after match 1, 502 MB from match
2 to 10; guest 413, 512 (matches 2-6), 671, then 763 MB (matches 8-10).
The Go desktop build, same 10-match session (Go - Go, `NODUMP=1`): heap
after a full collection 52-63 MB per match (54 → 63 MB, the slight rise
follows the pairs loaded), RSS 165-220 MB; both sides and the native run
had the same winners in all 10 matches.

So neither build grows without bound, but the native high-water mark is
2.5-3.5 times Go's RSS, too much for phones: the
fight budget (`CVF_GC_FIGHT_BUDGET_MB`, 256 MB) and the save-state garbage
of rollback (to-do item 10, memcpy rollback) are what to work on.

**Content fingerprint (done).** `cvf_fingerprint.go` in both fork patches
fills the handshake's `content_fingerprint` with
`cvf1;r=<rules>;d=<data>;c/<char dir>=<hash>;...;s/<stage>=<hash>;...`,
computed once per run (64-bit FNV-1a of relative paths and bytes, carriage
returns skipped; links to folders followed on both engines):

| Entry | What | Checked |
| --- | --- | --- |
| `r` | gameplay rules of `data/cvf_online.ini` as parsed (not LobbyURL, BuildTag, comments, WideView) | session start |
| `d` | simulation text files under `data/` (common states, `*.zss`, fight.def, select.def, system.def) | session start |
| `c/<dir>` | each character folder's `.def .cns .st .cmd .air .zss .const` | match start, loaded characters only |
| `s/<file>` | each stage `.def` | match start, loaded stage only |

A difference stops both sides with "Game data mismatch: ..." naming the
rules, `data/`, the character or the stage; characters only one side has,
or unused ones that differ, do not matter. Sprites, sounds and palettes are
not hashed (they do not change the simulation). An empty fingerprint
(builds before this, Ikemen GO, the Vita build) means unknown and is
allowed; those builds ignore the table. Cost: 2 characters 4.5 MB, 8-44 ms;
a 114-character folder (70 MB of text, made from copies of the two) 527 ms
native and 547 ms Go on the loaded container, once, at the first
connection. `run_content.sh` PASS for native - native, Go - native and
native - Go (Go desktop configuration), each also with an old build of
the same engine on either side (old - new and new - old play in every
case).

**Roster sweep (partly).** Only two characters exist here
(cvf-refdata.tgz: Alucard and Richter; the uploaded `CvF Online` folder has
only `font/`), so the 114-character sweep could not run. Covered: every
pair of the two, both sides, several palettes, 6 input-script seeds, one
stage (ParanoiaBG):

| Run | Pairs (host side first) | Network | Result |
| --- | --- | --- | --- |
| native - native sessions above | A-R, R-A, A-A, R-R, palettes 1-12 | direct, 8 % loss + sync + TCP lag | identical |
| Go host - native guest (`run_roster.sh`, seeds 33/44) | A-R, R-A, A-A 3/5, R-R 2/4 | 40 ± 15 ms + TCP 30 ± 20 ms | PASS; the same session direct also PASS with the same dumps |
| native host - Go guest (seeds 55/66) | same | 8 % loss incl. sync | PASS (one earlier run failed, see below) |
| Go - Go, 10 matches (`NODUMP`) | A-R, R-A, A-A, R-R, palettes | direct | same winners as native - native |

No simulation desync was found: in every run both engines computed the
same frames. Two things the sweep found, both at the **end of a match**:

- The last frames of a match can be simulated with predicted remote inputs
  that are never corrected, because the rollback loop ends with the match
  (`hijackRunMatch` stops at `endMatch`; GGPO has no later frame to roll
  back from). With lag, a command was recognized in the win pose on one
  side only, 6 frames before the end (Richter mirror, Go host - native
  guest); every frame before was identical, and the same session direct
  had no difference. The next match starts from a fresh load and a new
  seed, so nothing carries over; the dumps of `run_netplay.sh` now compare
  the last `TAIL`=8 frames separately (reported, not failed).
- Once, with 8 % loss, the Go guest ended a match one frame before the
  native host (56,737 vs 56,736 frames), closed its GGPO session, and the
  host, still missing the guest's last inputs, timed out with
  "EventCodeDisconnectedFromPeer" (winner -1). The rerun passed. This is
  GGPO/Ikemen behaviour (no linger after the match ends), not a native
  difference; it would show as a disconnect message right after a match.
  Open: keep the GGPO session (or resend the last inputs) until the peer
  has confirmed the final frame.

`run_roster.sh` runs the full roster on a machine with the full game, e.g.
`HOST_BIN=cvf_headless_go GUEST_BIN=build/release/cvf_headless
MATCH_ARGS="-time 30" tests/netplay/run_roster.sh "/path/to/CvF Online"`
(each of 114 characters against the next one, both ways: 228 matches).

The analysis below is unchanged from `claude/native-gaps` (a16220d).

This analysis compares `claude/native-engine` at a631d7a with the shipping CvF Online
fork, `claude/android` at 6286bb7, whose `patches/ikemen-cvf-online.patch` was last
changed in d4d84f7. Nothing was built or run for it. Every claim comes from reading
the source and diffing trees that were rebuilt from the patches.
"Player hits it" means a normal player reaches it with the current game data.

## 1. Placeholders

Stub mechanisms found:

| Mechanism | Where | Count |
| --- | --- | --- |
| Std-lib placeholders: return zero, or return an error saying "not supported" | `src/ik/ik_unsupported.c` (gen_unsupported.py) | 31 |
| Functions from excluded Go files: return zero | `/* stub: */` in `src/ik/ik_sim.c` (run_reach.sh `GOTOC_EXCLUDE`) | 79 |
| reflect channel ops: panic | `src/ik/ik_reflect_stubs.c` | 4 |
| Hand-written partial shims | `ik_io.c` zip and gob, `ik_image.c` PNG encoding, `ik_native.c` Shift_JIS passthrough, `ik_rt.c` sync.Mutex as a no-op, `gl_bind.c` GL debug callback | 6 |
| Untranslatable `TODO(...)` sites left in the translation | `ik_sim.c` | 0 |

The stubs grouped by what breaks:

| Group | Count | Items | What the player sees |
| --- | --- | --- | --- |
| (a) Gameplay / simulation | 0 hard stubs, 1 conditional | `gob` Encode/Decode, used by the CNS `SaveFile`/`LoadFile` controllers | A character that uses SaveFile/LoadFile silently loses its saved data. No CvF use was found in the repo, but **check the full game data**. |
| (b) Menus / UI / presentation | 52 + 8 + 7 + 10 + 6 | Vulkan renderer (52); glTF 3D stages (8: model.go, `fs.ReadFile`); XM/MOD tracker music (7); stage and storyboard video (10); `io.TeeReader` and `bytes.Reader.ReadByte`, which the FLAC decoder needs; `saveScreenshot` and PNG encoding; Shift_JIS names; zip archives; `user.Current` (TTF font `~` paths) | **Player hits it:** choosing Options > Video > Render Mode = "Vulkan 1.3" selects the stub renderer, so the screen is black after a restart. A `.flac`/`.xm`/`.mod`/video BGM, a zipped character, or a Shift_JIS name is silent, fails to load, or shows garbled text. Check the game data for these. Screenshots (F12) do nothing. |
| (c) Online / lobby / netplay | 1 + 1 | `gob.Register` (a no-op, harmless: GGPO uses its own binary format). `sync.Mutex` is a no-op, which is safe only while every caller runs on the engine thread. | Nothing today. See section 3 for the parts that are translated but untested. |
| (d) Nearby play | not in the engine at all | Not stubbed: the code is absent, because the fork used for generation is older than Nearby (section 2) | **Player hits it:** with the current game data, the Online menu calls the missing Lua functions and the Lua script errors out (section 2). |
| (e) Tooling / debug only (safe to leave) | 24 | Lua `io.popen`/`os.execute`/`os.tmpname`/`io.tmpfile`/`read("n")` (15 exec/os/syscall/ioutil/fmt.Fscanf/bufio functions; no CvF script uses them); `tabwriter` ×3 (mgl32 matrix `String`); `abi.Kind.String`, `abi.Type.Len`; `runtime.Stack` (crash logs have no stack); `runtime.LockOSThread`; `color.ModelFunc`; `sdl.InitSubSystem` (Android only, but needed there for game controllers); `cookChar`; 4 reflect channel ops | Nothing. |

## 2. Fork drift

**Which fork version generated the C engine.** `tools/ikemen_headless.patch` is the
whole fork as a diff against 6a09392. It applies cleanly to that base. It contains:

- the CvF Online patch at **a2196da (Nov 1)**, which is also the copy in
  `patches/` on this branch;
- the Vita performance paths and the headless/cvf build files;
- **cherry-picked gameplay rules from 3f9d897 (Nov 7)**: special cooldown, shared
  FightAspect and WideView.

It does **not** contain anything from 96bec78 (touch menus, Nov 4) onwards that isn't
gameplay, and none of 6e89472, 1ce6ed0 or d4d84f7 (Nearby, Apple P2P, touch online
screens, Nov 14–15). The gameplay Go code (`char.go`, `bytecode.go`, `system.go`,
cvf rules) **matches the latest fork**: every cvf hook is present.

**The Lua scripts are not part of the engine.** The engine reads
`external/script/*.lua` from the game folder at run time. The latest scripts call 18
`cvf*` functions that the native engine never registers, and they call them without
checking that they exist. The result is a Lua error, which ends the game:

| Missing in the native engine | Called from | Player hits it |
| --- | --- | --- |
| `cvfTouchTap`, `cvfTouchMenus` | start.lua, the character select loop | **Every match, at character select** |
| `cvfIsMobile`, `cvfAskNameBegin/Poll`, `cvfDefaultName` (name prompt, `f_cvfEnsureName`) | main.lua: Nearby, and Host/Lobby when a lobby is set up | **Entering Online** |
| `cvfTouchUI` | main.lua host list help | Online host list |
| `cvfNearby{HostBegin,HostEnd,HostStatus,ScanBegin,ScanEnd,Hosts,Hint,DirectJoin,DirectClose}` | main.lua Nearby menu | Nearby host/join |
| `cvfGetRollbackPort`, `cvfSetRollbackPort` | main.lua Nearby join | Nearby join |

The other drift:

| Change (latest fork) | Native | How to get it |
| --- | --- | --- |
| Nearby LAN discovery (`cvf_nearby.go`, 564 lines; UDP probe sweep, JSON) | missing | **Free by regenerating.** It needs new std shims: `crypto/rand` (Read, Int), `math/big`, `net.CIDRMask`, `net.ResolveUDPAddr` and IPMask helpers. gen_unsupported would turn these into **silent placeholders**, so Nearby would compile and then fail at run time. |
| Touch tap API (`cvfStoreTap`, `cvfTouchTap` and `cvfTouchMenus` in cvf_online.go) | missing | The Lua side is free by regenerating. Its callers are cgo `//export` / JNI functions in `util_ios.go` and `util_android.go`; gotoc skips cgo files, so they **need hand-written C**. `cvfTapMu` becomes a no-op, so a tap that arrives on another thread (the Android UI thread) must be queued over to the engine thread. |
| Name prompt (`cvfNameAsk*`, `cvfDefaultName`) | missing | The Go logic is free. The native name box (`CvfNameEntered`, `cvfNativeAskName`) needs hand-written C/ObjC/Java per platform. |
| Apple peer-to-peer link (`cvf_p2p_ios.go`, `cvf_p2p_export_ios.go`; the relay itself is ObjC in `ios/bundled-game.patch`) | missing | **Hand-written C**: wrappers for the CvfP2P* calls and exports. The ObjC can be reused, but only once a native iOS front end exists. |
| `--screen` arg, `nativeCvfSetScreen`, `CvfInMatch` | missing | Hand-written C in the mobile front ends. |
| "Stop hosting" hint for touch (`cvfStopHostingHint`) | missing | Free by regenerating. |
| `defaultMotif.ini` gets a `framerate` menu label (go:embed) | missing | Free by regenerating. Today the 120 FPS menu item shows no label. |
| `cvf-files/` on native-engine is stale: `cvf_online.ini` is still `cvf-online-2` with no cooldown or FightAspect; the Nearby `options11.def` and the Intro 16:9 door fix are missing | stale | Merge cvf-files from claude/android. This is data only. |

**Regenerating is not reproducible today.** `tools/gen/regen.sh` and
`gotoc/run_reach.sh` use absolute paths into a scratchpad from an earlier session:
the Go 1.27 toolchain, the gotoc/gencdefs binaries and the `iktg` fork tree. None of
these are in the repo.

## 3. Online: what an end-to-end match needs

| Piece | Status in C | Tested |
| --- | --- | --- |
| TCP/UDP sockets (`ik_net.c`: BSD/Winsock, non-blocking, deadlines, interfaces, SO_BROADCAST) | hand-written | Loopback on Linux only |
| Netplay handshake (`netplay.go`: JSON sync settings, delay netplay, replays) | translated; `encoding/json` in `ik_json.c` | No |
| GGPO rollback (`ikemen-engine/ggpo`: ggpo, protocol, transport, messages; 182 functions) | translated | No |
| Rollback save/load (`state_clone.go`, RollbackSystem) | translated | **No.** The Ikemen desync test has never run on native |
| UPnP port mapping (`upnpDiscover/Describe` + `encoding/xml` subset) | translated + hand-written | No |
| HTTPS lobby, public IP (`cvf_online.go` over `ik_http.c`) | translated + hand-written: libcurl loaded at run time (Linux, macOS), WinHTTP (Windows) | HTTPS GET on macOS. The lobby **POST** path is untested. **No backend for iOS or Android**, which have no system libcurl |
| Goroutines (netplay, GGPO and lobby workers) | cooperative scheduler; sockets polled every 1 ms | No measurement of input latency or frame jitter during a rollback match |
| Lua online menu (host, join, lobby, ports) | present in the a2196da fork | Only if the scripts match (section 2) |

Nothing is stubbed on the online path. **Everything on it is translated and has never
been run.** What remains is verification, the mobile HTTP backend, and the bug below.

**Suspected desync bug (FightAspect).** `applySyncSettings` (netplay.go) calls
`cfg.normalize()` after it applies the host's settings, at session start and again
when the session ends. Under `vitaPerf`, which every native build sets,
`normalize()` forces `FightAspectWidth/Height = 0,0`, and `cvfApplyDisplay` does not
run again afterwards. The handshake compares the two sides at 4:3, then native
switches to 0,0 while Go stays at 4:3. Fight aspect is simulation state (PORTING.md),
so **every native↔Go match would desync**, and offline play after online would run at
0,0. Fix: apply the rules' FightAspect inside `normalize()`, or call
`cvfApplyDisplay` after it.

## 4. Determinism and cross-play risks

| Risk | Severity | Notes |
| --- | --- | --- |
| FightAspect reset at session start (above) | **High** | Suspected from reading the code. Fix it and test it first. |
| Native has never been compared with a Go build under the cvf-online-5 rules | High | Linux native is deterministic with itself (4334350c…). macOS arm64 was not re-run with the new rules. |
| `vitaPerf` paths (bytecode fusion, var cache, explod and SpriteData reuse) run only in native and Vita builds, never in the Go PC/iOS/Android builds | High | They were checked against one reference fight (Alucard vs Richter). Run the 93-entry roster desync test native vs Go. |
| No build/content fingerprint: `currentContentFingerprint()` returns "" | Medium | A different `cvf_online.ini` or different characters on each side gives a silent desync, not a refused connection. Direct IP and Nearby skip the lobby's BuildTag filter. This needs a change in the Go fork too. |
| Go `int` becomes C `int` (32-bit). The shipping Go builds are 64-bit | Medium-low | This matches the Vita, not PC/iOS/Android. Check the sim and checksum code for `int` math that can pass 2³¹. |
| float→int conversion of out-of-range values | Low | Native always uses amd64 semantics. Go arm64 builds (iOS, Android, Apple Silicon) saturate instead. This mismatch already exists between the Go builds. |
| Strict setting strings come from native `fmt`/`json` | Low | Values are compared as strings. If a float is formatted differently from Go, the connection is refused; it does not desync. |
| Rollback cost and GC | Medium (mobile) | 0.85–1.4 ms per tick before optimisation, and a rollback re-simulates several ticks. `state_clone` allocates far more than the 5 KB per tick measured offline, and the collector's fight budget may start mid-fight. |
| Map iteration in insertion order (native) vs random (Go) | Low | Simulation code can't depend on map order, or Go would desync against itself. |

## 5. Ranked to-do

| # | Item | Size |
| --- | --- | --- |
| 1 | **Lua API parity.** Register the 18 missing `cvf*` functions now: hand-written C that reports touch and Nearby as unavailable, `cvfIsMobile`=false, and `cvfDefaultName`. Without them every match crashes at character select and the Online menu crashes. | S |
| 2 | **Fix the FightAspect reset** in `normalize()` under vitaPerf, then confirm with a native↔Go replay | S |
| 3 | **Rebuild the regen toolchain inside the repo**: pinned Go 1.27, gotoc/gencdefs built from `tools/gen`, a script that applies the fork patches to 6a09392, no absolute paths | M |
| 4 | **Rebase the fork inputs on d4d84f7 and regenerate.** This brings Nearby, touch tap, the name prompt and the motif label. Add std shims for crypto/rand, math/big, CIDRMask and ResolveUDPAddr. Make regen **fail when the placeholder count grows** | M |
| 5 | **Cross-play verification**: a Go statedump with the cvf-online-5 rules vs native (Linux and macOS arm64); the roster desync test on native; a native↔Go rollback match | M |
| 6 | **End-to-end online test** (native↔native and native↔Go): TCP handshake, GGPO over UDP, lobby POST, UPnP, NAT; measure latency with the cooperative scheduler | M |
| 7 | Map the Vulkan render mode to GL 3.3 in cvf builds (desktop selection, and the iOS forcing in `main.go`) | S |
| 8 | Merge `cvf-files/` from claude/android: cvf-online-5 rules, Nearby menu, Intro fix | S |
| 9 | Content/rules fingerprint in the handshake. It goes in both the Go fork and native, and needs the shipping builds updated | S–M |
| 10 | Measure the rollback re-simulation cost and the collector; tune the fight budget, or implement the planned memcpy rollback | M |
| 11 | Mobile front ends (iOS, Android): SDL entry points, a GLES binding (`leonkasovan/gl` gles2), JNI and ObjC exports in C (touch, screen, name box, in-match), queueing callbacks onto the engine thread, an HTTP backend (NSURLSession/Java), `sdl.InitSubSystem` | L |
| 12 | Nearby over Apple peer-to-peer on native iOS: C glue for CvfP2P*, reusing the ObjC relay | M |
| 13 | Check the game data for FLAC/XM/video/zip/Shift_JIS/SaveFile; implement whichever are used (TeeReader + ReadByte, gob subset, Shift_JIS table) | S each |
| 14 | Leftovers: screenshot PNG encoding, `runtime.Stack` for crash logs, checking `int` width in the sim | S |
