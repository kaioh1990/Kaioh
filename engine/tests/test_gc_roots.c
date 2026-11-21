// Collector roots across goroutine switches (core/sched.c + core/gc.c),
// from the independent runtime review (2026-11-21). test_sched.c collects
// only from main; these collect from goroutines:
//
//  1. main's stack while a goroutine collects (main is suspended)
//  2. a parked goroutine's stack while another goroutine collects
//  3. many short goroutines (reap of finished ones) with collections between
//  4. objects held only by a pointer below their start (32-bit ARM GCC)
//
// cvf_tests runs them with the platform's context switch (x86-64 / arm64:
// ctx_switch; Windows: fibers); cvf_tests_ucontext (Linux) runs the same
// tests against a core built with -DCVF_SCHED_UCONTEXT, the swapcontext
// path of 32-bit ARM, i686 and other POSIX targets. Before the fix there,
// cases 1 and 2 failed: no suspended stack was scanned.
#include <stdio.h>
#include <string.h>

#include "core/gc.h"
#include "core/sched.h"

#define CHECK(cond, ...) do { (*checks)++; if (!(cond)) { (*failures)++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N 64

static void fill(int *p, int seed) {
    for (int i = 0; i < N; i++) p[i] = i * 7 + seed;
}
static int check(const int *p, int seed) {
    for (int i = 0; i < N; i++)
        if (p[i] != i * 7 + seed) return 0;
    return 1;
}
// garbage of the same size class: a freed object would be handed out again
static __attribute__((noinline)) void churn(void) {
    for (int j = 0; j < 40000; j++) memset(gc_alloc(N * sizeof(int)), 0xAB, N * sizeof(int));
    gc_collect();
    for (int j = 0; j < 40000; j++) memset(gc_alloc(N * sizeof(int)), 0xCD, N * sizeof(int));
    gc_collect();
}

// 1 ------------------------------------------------------------------------
static void collector(void *e) {
    churn();
    int one = 1;
    go_chan_send((GoChan *)e, &one);
}
static __attribute__((noinline)) int case_main_stack(void) {
    int *volatile data = (int *)gc_alloc(N * sizeof(int));  // only on main's stack
    fill(data, 1);
    GoChan *c = go_chan_make(sizeof(int), 0);
    go_spawn(collector, c);
    int v;
    go_chan_recv(c, &v);  // main suspended while the goroutine collects
    return check(data, 1);
}

// 2 ------------------------------------------------------------------------
typedef struct {
    GoChan *c;
    int ok;
} HEnv;
static void holder(void *e) {
    HEnv *h = (HEnv *)e;
    int *volatile data = (int *)gc_alloc(N * sizeof(int));  // only on this goroutine's stack
    fill(data, 2);
    int v;
    go_chan_recv(h->c, &v);  // parked while another goroutine collects
    h->ok = check(data, 2);
    go_chan_send(h->c, &v);
}
static void collector2(void *e) {
    churn();
    int one = 1;
    go_chan_send(((HEnv *)e)->c, &one);
}
static __attribute__((noinline)) int case_parked(void) {
    HEnv *h = (HEnv *)gc_alloc(sizeof *h);
    h->c = go_chan_make(sizeof(int), 0);
    go_spawn(holder, h);
    go_yield();  // holder allocates and parks
    go_spawn(collector2, h);
    int v;
    go_chan_recv(h->c, &v);  // collector2 wakes holder, holder replies here
    return h->ok;
}

// 3 ------------------------------------------------------------------------
static void shortlived(void *e) {
    int *p = (int *)gc_alloc(N * sizeof(int));
    fill(p, 3);
    if (((intptr_t)e & 7) == 0) gc_collect();  // collect while a zombie may be pending
    go_yield();
}
static __attribute__((noinline)) int case_reap(int base) {
    for (int i = 0; i < 2000; i++) {
        go_spawn(shortlived, (void *)(intptr_t)i);
        if (i % 16 == 0) go_yield();
    }
    while (go_num_goroutines() > base) go_yield();
    gc_collect();
    return go_num_goroutines() == base;
}

// 4. derived pointers below an object (GAPS.md, "derived pointers on 32-bit
// ARM"): the object's only reference is a stack word a few bytes *before*
// it, as GCC's pre-indexed loads leave on 32-bit ARM. The word lives in a
// volatile stack slot, so the test does not depend on register allocation.
static __attribute__((noinline)) uintptr_t make_below(size_t n, int seed, uintptr_t below) {
    int *p = (int *)gc_alloc(n);
    fill(p, seed);
    return (uintptr_t)p - below;
}
static __attribute__((noinline)) int case_derived(size_t n, uintptr_t below) {
    volatile uintptr_t d = make_below(n, 41, below);
    volatile uint8_t scrub[512];  // overwrite stale copies of p below this frame
    memset((void *)scrub, 0, sizeof scrub);
    churn();
    return check((const int *)(d + below), 41);
}

void test_gc_roots(int *failures, int *checks) {
    int base = go_num_goroutines();
    CHECK(case_main_stack(), "main stack scanned while a goroutine collects");
    CHECK(case_parked(), "parked goroutine scanned while another goroutine collects");
    CHECK(case_reap(base), "2000 goroutines spawned/reaped with collections between");
    CHECK(go_num_goroutines() == base, "goroutines left: %d (expected %d)", go_num_goroutines(), base);
    CHECK(case_derived(N * sizeof(int), 4), "small object held only by a pointer 4 bytes below it");
    CHECK(case_derived(N * sizeof(int), 200), "small object held only by a pointer 200 bytes below it");
    CHECK(case_derived(64 * 1024, 8), "large object held only by a pointer 8 bytes below it");
}
