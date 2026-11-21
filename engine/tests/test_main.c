// Core tests. Golden values come from the Go reference implementation
// (Ikemen GO), see tests/README.md for how each was produced.
#include <stdio.h>
#include <string.h>

#include "core/base.h"
#include "core/gort.h"
#include "core/rng.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u32 bits32(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_rng(void) {
    // Same sequence as tests/golden/rng.go: mixed Random/Rand/RandF/RandF32.
    i32 seed = 12345;
    u32 h = 2166136261u;
#define MIX(v) (h = (h ^ (u32)(v)) * 16777619u)
    for (int i = 0; i < 100000; i++) {
        switch (i % 4) {
        case 0: MIX(cvf_random(&seed)); break;
        case 1: MIX(cvf_rand(&seed, 0, 999)); break;
        case 2: MIX(bits32(cvf_randf(&seed, -3.5f, 7.25f))); break;
        case 3: MIX(bits32(cvf_randf32(&seed, 1, 6))); break;
        }
    }
    CHECK(h == 0x3adc314cu && seed == 464161387, "rng hash %08x seed %d", h, seed);
}

// go_strz: short strings in the rotating buffers, long ones copied whole
// (they were cut at 1023 bytes)
static void test_strz(void) {
    static char big[3000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)('a' + i % 26);
    const char *z = go_strz((GoString){big, (int)sizeof big});
    CHECK(strlen(z) == sizeof big && !memcmp(z, big, sizeof big), "go_strz long: %zu bytes", strlen(z));
    const char *s = go_strz((GoString){big, 1023});
    CHECK(strlen(s) == 1023 && s != z, "go_strz 1023: %zu bytes", strlen(s));
    CHECK(*go_strz((GoString){NULL, 0}) == 0, "go_strz empty");
}

void test_formats(int *failures, int *checks);
void test_detmath(int *failures, int *checks);
void test_sched(int *failures, int *checks);
void test_gc_roots(int *failures, int *checks);

int main(int argc, char **argv) {
    test_rng();
    test_formats(&failures, &checks);
    test_detmath(&failures, &checks);
    test_sched(&failures, &checks);
    test_gc_roots(&failures, &checks);
    test_strz();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
