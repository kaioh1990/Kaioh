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

### Online: still open after claude/native-netplay

| Item | State |
| --- | --- |
| GGPO synchronization under packet loss | **Open, in GGPO itself (shared Go code, not a translation bug).** A peer that has synchronized drops the other after `Rollback.DisconnectTimeout` (3 s) without inputs, while a peer still synchronizing retries a lost sync round trip only every 2 s (`SyncRetryInterval`). With two or three sync packets lost in a row the slower peer cannot finish in time: the running side times out ("disconnected"), the other fails with the rollback sync timeout. Seen on the Mac with 8 % loss in 2 of 12 runs (native ↔ Go desktop, both directions) and with 25 % loss in 3 of 4 runs of the Go desktop build against itself (Go ↔ Go), traced with `RELAY_LOG=1`. The netplay test therefore spares the sync handshake from loss unless a scenario ends in `:sync`. A fix belongs in the ggpo fork (do not count the disconnect timeout before the peer is synchronized, or keep answering sync requests) and in every build at once. |
| Lobby, UPnP, NAT, public IP | Not tested (direct IP only; no external services were used). |
| Windowed online match through the Lua menus | Not run; the test drives the same Go code headless (main.lua's command-line `-ip` path). The menu smoke test covers the Online menu up to Nearby host/join. |
| Several matches per session (rematch, `postMatchSetup` → new RollbackSession) | Not run. |
| Roster-wide desync test (93 characters) native vs Go | Not run; only Alucard vs Richter. |
| Content fingerprint | Still empty on both sides (`currentContentFingerprint`). |

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
