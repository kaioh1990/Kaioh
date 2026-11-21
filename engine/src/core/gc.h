// Conservative, non-moving mark-sweep collector for the translated Go code.
//
// Every allocation the translated simulation and its runtime make goes
// through here. The heap is one reserved address range split into 8 KiB
// pages; small objects (<= 4 KiB) live in size-classed pages, larger ones in
// runs of whole pages. Interior pointers keep objects alive (Go slices point
// into the middle of their arrays). Roots are the stack, the registers and
// the program's data/bss segments, plus anything registered explicitly.
//
// The collector is single-threaded: only the simulation thread may hold
// pointers into the heap.
#ifndef CVF_GC_H
#define CVF_GC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Call once, early in main, with the address of a local in main.
void gc_init(void *stack_base);

void *gc_alloc(size_t n);         // zeroed; scanned for pointers
void *gc_alloc_atomic(size_t n);  // zeroed; never scanned (bytes, numbers)
void *gc_realloc(void *p, size_t n);
void gc_free(void *p);  // optional early release of a block known to be dead
char *gc_strdup(const char *s);
static inline void *gc_calloc(size_t n, size_t sz) { return gc_alloc(n * sz); }
static inline void *gc_calloc_atomic(size_t n, size_t sz) { return gc_alloc_atomic(n * sz); }

void gc_add_roots(void *lo, void *hi);  // extra root range [lo, hi)
void gc_collect(void);                  // full collection now
size_t gc_release_free_pages(void);     // return free pages to the OS (after gc_collect); bytes
// Frame boundary. While `fighting`, allocation never triggers a collection
// unless the fight budget (CVF_GC_FIGHT_BUDGET_MB, default 256 MB on 64-bit,
// 48 MB on 32-bit) is exceeded; otherwise a due collection runs here.
void gc_safepoint(bool fighting);
void gc_disable(void);                  // nestable: no automatic collections
void gc_enable(void);

// Goroutine support (core/sched.c): top of the running goroutine's stack
// (0 = the main thread's), and a callback that scans suspended stacks.
extern uintptr_t (*gc_stack_top_hook)(void);
extern void (*gc_extra_roots_hook)(void (*scan)(uintptr_t lo, uintptr_t hi));
uintptr_t gc_main_stack_top(void);
// The main goroutine runs on a stack of its own (iOS, IOS_PORT.md 3.3 plan
// B): its top replaces the main thread's stack top for scanning.
void gc_set_main_stack(void *top);

typedef struct GcStats {
    size_t heap_bytes;       // pages in use (small + large)
    size_t live_bytes;       // marked at the last collection
    size_t alloc_bytes;      // allocated since the last collection
    size_t total_alloc;      // allocated over the program's life
    uint64_t collections;
    double last_pause_ms, total_pause_ms, max_pause_ms;
} GcStats;
GcStats gc_stats(void);

// Startup self-check of the data roots: objects referenced only from an
// initialised global, a zero-initialised one and a tentative definition
// must survive a collection. Returns the number of failures (0 = pass).
int gc_selftest(void);

#endif
