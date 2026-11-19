// Cooperative goroutines, channels, select, and the collector scanning
// suspended goroutine stacks (core/sched.c).
#include <stdio.h>
#include <string.h>

#include "core/gc.h"
#include "core/sched.h"

#define CHECK(cond, ...) do { (*checks)++; if (!(cond)) { (*failures)++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { GoChan *in, *out; int n; } PipeEnv;

static void doubler(void *e) {  // receives until closed, sends doubles
    PipeEnv *p = (PipeEnv *)e;
    int v;
    while (go_chan_recv(p->in, &v)) {
        int d = v * 2;
        go_chan_send(p->out, &d);
    }
    go_chan_close(p->out);
}

typedef struct { GoChan *c; int **keep; } GcEnv;

static void gc_holder(void *e) {  // holds the only reference to heap data on its stack across collections
    GcEnv *g = (GcEnv *)e;
    int *data = (int *)gc_alloc(64 * sizeof(int));
    for (int i = 0; i < 64; i++) data[i] = i * 7;
    int dummy = 0;
    go_chan_recv(g->c, &dummy);  // suspended here while main collects
    int ok = 1;
    for (int i = 0; i < 64; i++) ok &= data[i] == i * 7;
    go_chan_send(g->c, &ok);
}

static void sleeper(void *e) {
    go_sleep_ns(2000000);  // 2 ms
    int v = 42;
    go_chan_send((GoChan *)e, &v);
}

void test_sched(int *failures, int *checks) {
    // unbuffered pipeline through a goroutine
    PipeEnv *p = (PipeEnv *)gc_alloc(sizeof *p);
    p->in = go_chan_make(sizeof(int), 0);
    p->out = go_chan_make(sizeof(int), 0);
    go_spawn(doubler, p);
    int sum = 0;
    for (int i = 1; i <= 10; i++) {
        go_chan_send(p->in, &i);
        int r = 0;
        CHECK(go_chan_recv(p->out, &r), "recv %d", i);
        sum += r;
    }
    go_chan_close(p->in);
    int r = 0;
    CHECK(!go_chan_recv(p->out, &r) && r == 0, "closed channel yields zero, ok=false");
    CHECK(sum == 110, "pipeline sum %d", sum);

    // buffered channel: no blocking within capacity, FIFO order
    GoChan *b = go_chan_make(sizeof(int), 3);
    for (int i = 0; i < 3; i++) go_chan_send(b, &i);
    CHECK(go_chan_len(b) == 3, "len %d", go_chan_len(b));
    int order = 0;
    for (int i = 0; i < 3; i++) {
        go_chan_recv(b, &r);
        order = order * 10 + r;
    }
    CHECK(order == 12, "fifo %d", order);

    // select with default, then a select woken by a sleeping goroutine
    GoChan *s1 = go_chan_make(sizeof(int), 0), *s2 = go_chan_make(sizeof(int), 0);
    int v1 = 0, v2 = 0;
    bool ok1 = false, ok2 = false;
    GoSelCase cs[2] = {{s1, 2, &v1, &ok1}, {s2, 2, &v2, &ok2}};
    CHECK(go_select(cs, 2, true) == -1, "default taken");
    go_spawn(sleeper, s2);
    int k = go_select(cs, 2, false);
    CHECK(k == 1 && v2 == 42 && ok2, "select woke on case %d value %d", k, v2);

    // the collector keeps data referenced only from a suspended goroutine
    GcEnv *g = (GcEnv *)gc_alloc(sizeof *g);
    g->c = go_chan_make(sizeof(int), 0);
    go_spawn(gc_holder, g);
    go_yield();  // let it allocate and block
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 20000; j++) {
            int *junk = (int *)gc_alloc(256);
            memset(junk, 0xAB, 256);
        }
        gc_collect();
    }
    int one = 1;
    go_chan_send(g->c, &one);
    int ok = 0;
    go_chan_recv(g->c, &ok);
    CHECK(ok == 1, "data on a suspended goroutine's stack survived collection");
    CHECK(go_num_goroutines() == 1, "goroutines left: %d", go_num_goroutines());
}
