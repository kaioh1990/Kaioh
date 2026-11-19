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
