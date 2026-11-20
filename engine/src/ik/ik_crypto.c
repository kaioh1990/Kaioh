// crypto/rand and the small part of math/big the game uses (cvf_nearby.go:
// a random host id and a default player name). Neither reaches the
// simulation, so OS randomness here cannot affect rollback determinism.
//
// big.Int only ever holds values that fit an int64 here (big.NewInt), so it
// is an opaque box around one.
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#endif

#include "ik/ik_sim.h"

GoIface crand_Reader;  // nil; crand_Int reads the OS source whatever reader it is given

static void os_random(u8 *p, size_t n) {
#if defined(_WIN32)
    BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__APPLE__)
    arc4random_buf(p, n);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(p, 1, n, f) : 0;
    if (f) fclose(f);
    if (got != n) {  // no /dev/urandom: monotonic clock mixing (ids only, never game state)
        u64 x = (u64)go_mono_ns() ^ 0x9E3779B97F4A7C15ull;
        for (size_t i = got; i < n; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            p[i] = (u8)(x >> 32);
        }
    }
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
