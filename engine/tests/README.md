# Tests and golden references

The engine must behave exactly like Ikemen GO (the reference engine) on
CvF's content. Each ported subsystem is checked against Go:

- `cvf_tests`: unit tests with golden values produced by the Go programs in
  `golden/` (`go run golden/rng.go`, etc.), plus the scheduler and collector
  (`test_sched.c`, `test_gc_roots.c`: roots on suspended goroutine stacks).
  `cvf_tests_ucontext` (Linux) runs them with the ucontext scheduler that
  32-bit ARM and i686 use (`-DCVF_SCHED_UCONTEXT`; docs/DEVSETUP.md).
- `cvftool airdigest FILES...` vs `golden/air.go` (built from Ikemen's own
  anim.go functions): digest over every compiled action of every file.
  Status: all 124 CvF AIR sources (75,496 actions) identical.
- Reference fights: per-tick state dumps from the Ikemen build
  (`tools/ikemen_statedump.go`), compared tick by tick once the simulation
  core runs.
- `cvf_nearby_test DIR`: the translated Nearby code through its Lua
  bindings (host on UDP 7520, subnet sweep + 127.0.0.1 probe, host list,
  periodic sweep) and the desktop answers of the touch / mobile / name-box /
  P2P bindings. Interop with Go's encoding/json and net: `ik/nearby_go`
  (`go run . probe` against `cvf_nearby_test DIR host 6`, and
  `go run . host` against `cvf_nearby_test DIR scan 7600 GoHost`).
- `netplay/run_netplay.sh GAMEDIR`: two headless engines play a full online
  rollback match over loopback (host/join by IP, handshake, GGPO over UDP,
  optionally through `netplay/udp_relay.py` with delay, jitter and loss);
  both sides' per-frame state dumps must be identical, in every network
  scenario. `HOST_BIN`/`GUEST_BIN` select native or Go builds
  (docs/DEVSETUP.md). `MATCHES` plays several matches in one session
  (rematch, character select, new characters), scenario option `tcp=D/J`
  delays the netplay TCP connection too (`netplay/tcp_relay.py`), `LOAD=N`
  adds busy loops.
- `netplay/run_prematch.sh GAMEDIR RUNS`: the TCP part of a match start
  RUNS times (the pre-match "Synchronization error" race, GAPS.md).
- `netplay/run_content.sh GAMEDIR`: the content fingerprint: the guest
  plays from modified copies of the game; "Game data mismatch" exactly
  when the change matters, old builds always allowed.
- `netplay/run_roster.sh GAMEDIR`: sessions over character pairs, every
  character at least once on each side (needs the full game).
