// Cooperative goroutines and channels for the translated Go code.
//
// All goroutines run on one OS thread, switching only at channel
// operations, select, runtime.Gosched and sleeps - the model of the Go
// port's TinyGo runtime on the Vita, which the engine code is written for.
// Each goroutine has its own stack; the garbage collector scans all of them.
#ifndef CVF_SCHED_H
#define CVF_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GoChan GoChan;

GoChan *go_chan_make(size_t elem, int cap);
void go_chan_send(GoChan *c, const void *v);  // blocks (nil channel: forever)
bool go_chan_recv(GoChan *c, void *out);      // false when closed and drained (out zeroed); out may be NULL
void go_chan_close(GoChan *c);
int go_chan_len(GoChan *c);
int go_chan_cap(GoChan *c);

// select: dir 1 = send *val, 2 = receive into val (may be NULL), with *ok
// set for receives. Returns the chosen case, or -1 for the default clause.
typedef struct GoSelCase {
    GoChan *c;
    int dir;
    void *val;
    bool *ok;
} GoSelCase;
int go_select(GoSelCase *cases, int n, bool has_default);

void go_spawn(void (*fn)(void *env), void *env);  // `go fn(...)`: runnable, runs when this goroutine yields
void go_yield(void);                               // runtime.Gosched
void go_sleep_ns(int64_t ns);                      // time.Sleep
int go_num_goroutines(void);
int64_t go_mono_ns(void);                          // monotonic clock

#if defined(__vita__)
// The Vita starts threads (and fibers) in flush-to-zero / default-NaN mode
// (FPSCR 0x03000000), unlike every other platform: denormals would differ
// from the PC. Call at the start of each thread that runs game code; every
// goroutine fiber does it itself.
void go_vita_fp_ieee(void);
#endif

#endif
