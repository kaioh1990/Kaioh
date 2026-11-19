#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/detmath.h"

#define CHECK(cond, ...) do { (*checks)++; if (!(cond)) { (*failures)++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 b64(f64 f) { u64 u; memcpy(&u, &f, 8); return u; }
static f64 fb64(u64 u) { f64 f; memcpy(&f, &u, 8); return f; }

// Same inputs and hash as tests/golden/detmath.go.
void test_detmath(int *failures, int *checks) {
    static f64 ins[4100];
    int n = 0;
    const f64 base[] = {0, -0.0, 1, -1, 0.5, 2, 3, 10, -10, 0.1, 1e-300, 1e300, INFINITY, -INFINITY, 0, (f64)(1 << 29), 1e10, -1e10, 3.14159, 1e-8, 700, -700, 710, 0.66, 2.5, -0.7};
    for (size_t k = 0; k < CVF_ARRAY_LEN(base); k++) ins[n++] = base[k];
    ins[14] = fb64(0x7FF8000000000001ull);  // Go math.NaN()
    u64 s = 88172645463325252ull;
    for (int i = 0; i < 4000; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        int e = (int)(s % 40) - 20;
        f64 m = (f64)(s >> 11) / (f64)(1ull << 53);
        f64 v = ldexp(m, e);
        if (s & 1) v = -v;
        ins[n++] = v;
    }
    u64 h = 1469598103934665603ull;
#define MIX(f) (h = (h ^ b64(f)) * 1099511628211ull)
    for (int k = 0; k < n; k++) {
        f64 x = ins[k], y = ins[(k * 7 + 3) % n];
        MIX(cvf_exp(x));
        MIX(cvf_ln(fabs(x)));
        MIX(cvf_log2(fabs(x)));
        MIX(cvf_pow(fabs(x), y));
        MIX(cvf_pow(x, 3));
        MIX(cvf_pow(x, -2));
        MIX(cvf_hypot(x, y));
        MIX(cvf_sin(x));
        MIX(cvf_cos(x));
        MIX(cvf_tan(x));
        MIX(cvf_atan(x));
        MIX(cvf_asin(x / 21));
        MIX(cvf_acos(x / 21));
        MIX(cvf_atan2(y, x));
    }
    CHECK(h == 0xf274271f7797deb1ull, "detmath hash %016llx", (unsigned long long)h);
}
