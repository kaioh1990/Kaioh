# Go standard library copy for Vita builds

TinyGo compiles calls to `syscall.Syscall*` / `syscall.RawSyscall*` into raw
Linux `svc #0` instructions (by function name). The Vita kernel rejects those.

`go-stdlib-vita.patch` (against Go 1.27.3 `src/`) renames that function
family to `VSyscall*` everywhere, makes `syscall.VRawSyscall6` call the C
function `tinygo_vita_syscall` (tinygo/vita_syscall.c, which emulates the
Linux syscalls Go needs on top of newlib/Sce*), and drops the Go-runtime
enter/exit hooks TinyGo doesn't have. Build Vita binaries with this GOROOT
(`PATH=goroot-vita/bin:$PATH`). TinyGo's own `net/netdev_native.go` and
`os/osexec.go`, and `github.com/mewkiz/pkg`, get the same rename.
