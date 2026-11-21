// os / path / bytes / math/rand pieces the game uses outside the simulation
// (working directory, app-bundle location, environment, small helpers).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define cvf_chdir _chdir
#define cvf_getcwd _getcwd
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#else
// declared by hand: <unistd.h> clashes with generated names
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
#define cvf_chdir chdir
#define cvf_getcwd getcwd
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#elif !defined(_WIN32) && !defined(__vita__)
long readlink(const char *path, char *buf, size_t n);
#endif

#include "ik/ik_sim.h"

Ret_GoString__GoIface os_Executable(void) {
    char buf[4096];
#if defined(__APPLE__)
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return (Ret_GoString__GoIface){{"", 0}, go_errorf("os.Executable: path too long")};
    char real[4096];
    const char *p = realpath(buf, real) ? real : buf;
#elif defined(_WIN32)
    unsigned long n = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return (Ret_GoString__GoIface){{"", 0}, go_errorf("os.Executable: failed")};
    const char *p = buf;
#elif defined(__vita__)
    const char *p = "app0:eboot.bin";  // the application's own mount (no readlink, no /proc)
    (void)buf;
#else
    long n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return (Ret_GoString__GoIface){{"", 0}, go_errorf("os.Executable: failed")};
    buf[n] = 0;
    const char *p = buf;
#endif
    size_t len = strlen(p);
    char *c = (char *)gc_alloc_atomic(len + 1);
    memcpy(c, p, len + 1);
    return (Ret_GoString__GoIface){{c, (int)len}, {0}};
}

GoIface os_Chdir(GoString dir) {
    if (cvf_chdir(go_strz(dir)) != 0) return go_errorf("chdir %.*s: no such file or directory", dir.len, dir.p);
    return (GoIface){0};
}

GoIface os_Rename(GoString from, GoString to) {
#if defined(_WIN32)
    remove(go_strz(to));  // Go replaces an existing target
#endif
    if (rename(go_strz(from), go_strz(to)) != 0) return go_errorf("rename %.*s: failed", from.len, from.p);
    return (GoIface){0};
}

GoIface os_Setenv(GoString k, GoString v) {
#if defined(_WIN32)
    if (_putenv_s(go_strz(k), go_strz(v)) != 0) return go_errorf("setenv: failed");
#else
    if (setenv(go_strz(k), go_strz(v), 1) != 0) return go_errorf("setenv: failed");
#endif
    return (GoIface){0};
}

// path.Dir: Clean(everything up to the last slash)
GoString path_Dir(GoString p) {
    int i = p.len - 1;
    while (i >= 0 && p.p[i] != '/') i--;
    GoString d = {p.p, i + 1};
    return filepath_Clean(d);  // same rules as path.Clean for slash paths
}

Ret_GoString__GoIface filepath_Abs(GoString p) {
    if (p.len && (p.p[0] == '/' || (p.len > 2 && p.p[1] == ':'))) return (Ret_GoString__GoIface){filepath_Clean(p), {0}};
    char cwd[4096];
    if (!cvf_getcwd(cwd, sizeof cwd)) return (Ret_GoString__GoIface){{"", 0}, go_errorf("getwd: failed")};
    return (Ret_GoString__GoIface){filepath_Join(GO_SLICE_LIT(Slice_GoString, GoString, 2, go_cstr(cwd), p)), {0}};
}

bool bytes_Equal(Slice_u8 a, Slice_u8 b) { return a.len == b.len && (a.len == 0 || !memcmp(a.p, b.p, (size_t)a.len)); }
int bytes_Index(Slice_u8 s, Slice_u8 sep) {
    if (sep.len == 0) return 0;
    for (int i = 0; i + sep.len <= s.len; i++)
        if (!memcmp(s.p + i, sep.p, (size_t)sep.len)) return i;
    return -1;
}

// math/rand top-level functions: menus and random selection only (the
// simulation has its own generator); splitmix64
static u64 rng_state = 0x9E3779B97F4A7C15ull;
static u64 rng_next(void) {
    u64 z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
void rand_Seed(i64 seed) { rng_state = (u64)seed; }
f64 rand_Float64(void) { return (f64)(rng_next() >> 11) * (1.0 / 9007199254740992.0); }
int rand_Intn(int n) {
    if (n <= 0) go_panic_str("invalid argument to Intn");
    return (int)(rng_next() % (u64)n);
}

// runtime.GOOS / runtime.GOARCH of the machine the C engine was compiled for
GoString ik_goos(void) {
#if defined(__ANDROID__)
    return GOSTR("android");
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return GOSTR("ios");
#else
    return GOSTR("darwin");
#endif
#elif defined(_WIN32)
    return GOSTR("windows");
#elif defined(__linux__)
    return GOSTR("linux");
#else
    return GOSTR("unknown");
#endif
}
GoString ik_goarch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return GOSTR("arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    return GOSTR("amd64");
#elif defined(__arm__)
    return GOSTR("arm");
#elif defined(__i386__) || defined(_M_IX86)
    return GOSTR("386");
#else
    return GOSTR("unknown");
#endif
}
