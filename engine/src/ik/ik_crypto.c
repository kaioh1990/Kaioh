// crypto/rand and the small part of math/big the game uses (cvf_nearby.go:
// a random host id and a default player name). Neither reaches the
// simulation, so OS randomness here cannot affect rollback determinism.
//
// big.Int only ever holds values that fit an int64 here (big.NewInt), so it
// is an opaque box around one.
#include <stdio.h>
#include <stdlib.h>  // _Exit; arc4random_buf on Apple
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <errno.h>
#include <sys/syscall.h>  // SYS_getrandom (also on Android before API 28, which lacks getrandom())
long syscall(long number, ...);  // declared by hand: <unistd.h> clashes with generated names
#endif

#include "ik/ik_sim.h"

GoIface crand_Reader;  // nil; crand_Int reads the OS source whatever reader it is given

// Go 1.24+: crypto/rand.Read never fails; if the OS source does, the program
// dies with a fatal error (not a recoverable panic) rather than continue with
// weak bytes. Same here: no clock or other fallback.
_Noreturn static void rand_fatal(const char *what) {
    fflush(stdout);
    fprintf(stderr, "fatal error: crypto/rand: failed to read random data (see https://go.dev/issue/66821): %s\n", what);
    fflush(stderr);
    _Exit(2);  // Go's exit status for a fatal error
}

#if !defined(_WIN32) && !defined(__APPLE__)
// /dev/urandom: on Linux only where the kernel has no getrandom(2) (< 3.17), as Go
static void urandom_read(u8 *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) rand_fatal("open /dev/urandom failed");
    size_t got = fread(p, 1, n, f);
    fclose(f);
    if (got != n) rand_fatal("short read from /dev/urandom");
}
#endif

static void os_random(u8 *p, size_t n) {
#if defined(_WIN32)
    // ULONG is 32-bit: read in chunks (callers ask for a few bytes, but be exact)
    while (n > 0) {
        ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n;
        NTSTATUS st = BCryptGenRandom(NULL, p, k, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st != 0) {  // STATUS_SUCCESS
            char msg[64];
            snprintf(msg, sizeof msg, "BCryptGenRandom: NTSTATUS 0x%08lx", (unsigned long)st);
            rand_fatal(msg);
        }
        p += k, n -= k;
    }
#elif defined(__APPLE__)
    arc4random_buf(p, n);  // cannot fail
#elif defined(__linux__)
    while (n > 0) {
        size_t k = n > 33554431u ? 33554431u : n;  // getrandom's per-call limit (2^25 - 1)
        long r = syscall(SYS_getrandom, p, k, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) {
                urandom_read(p, n);
                return;
            }
            char msg[64];
            snprintf(msg, sizeof msg, "getrandom: errno %d", errno);
            rand_fatal(msg);
        }
        p += r, n -= (size_t)r;  // a short read (signal) continues where it stopped
    }
#else
    urandom_read(p, n);  // other Unix
#endif
}

Ret_int__GoIface crand_Read(Slice_u8 b) {
    if (b.len > 0) os_random(b.p, (size_t)b.len);
    return (Ret_int__GoIface){b.len, {0}};
}

typedef struct BigInt {
    i64 v;
} BigInt;

GoOpaque *big_NewInt(i64 x) {
    BigInt *b = (BigInt *)gc_alloc_atomic(sizeof(GoOpaque));
    memset(b, 0, sizeof(GoOpaque));
    b->v = x;
    return (GoOpaque *)b;
}
i64 big_Int_Int64(GoOpaque *b) { return b ? ((BigInt *)b)->v : 0; }

// uniform in [0, max), as crypto/rand.Int (panics for max <= 0)
Ret_GoOpaque_big_IntPtrPtr__GoIface crand_Int(GoIface r, GoOpaque *max) {
    (void)r;
    i64 m = big_Int_Int64(max);
    if (m <= 0) go_panic_str("crypto/rand: argument to Int is <= 0");
    u64 um = (u64)m, lim = UINT64_MAX - UINT64_MAX % um, x;
    do {
        os_random((u8 *)&x, sizeof x);
    } while (x >= lim);  // rejection keeps it unbiased
    return (Ret_GoOpaque_big_IntPtrPtr__GoIface){big_NewInt((i64)(x % um)), {0}};
}
