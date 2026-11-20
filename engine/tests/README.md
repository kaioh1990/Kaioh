# Tests and golden references

The engine must behave exactly like Ikemen GO (the reference engine) on
CvF's content. Each ported subsystem is checked against Go:

- `cvf_tests`: unit tests with golden values produced by the Go programs in
  `golden/` (`go run golden/rng.go`, etc.).
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
