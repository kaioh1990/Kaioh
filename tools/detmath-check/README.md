CI check: copies `src/cvf_detmath.go` from the patched engine next to `main.go`
and runs it on Intel/AMD and Apple Silicon. All must print the same `hash=`
line, otherwise PC <-> Mac/iPad rollback would desync.
