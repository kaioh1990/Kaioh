#define _GNU_SOURCE  // pthread_getattr_np
// Conservative mark-sweep collector; see gc.h for the model.
#include "core/gc.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__linux__)
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <pthread.h>
#endif
#endif

// Address space reserved for the heap (MAP_NORESERVE: costs nothing until
// used). iOS gives an app far less virtual address space than a desktop,
// so the iOS build sets 1024 (CMakeLists.txt); CVF_GC_ARENA_MB overrides.
#ifndef CVF_GC_DEFAULT_ARENA_MB
#define CVF_GC_DEFAULT_ARENA_MB (sizeof(void *) == 8 ? 4096 : 384)
#endif

#define PAGE_SHIFT 13
#define PAGE_SIZE ((size_t)1 << PAGE_SHIFT)
#define SMALL_MAX 4096
#define MAX_PER_PAGE (PAGE_SIZE / 16)
#define MARK_WORDS (MAX_PER_PAGE / 64)

enum { P_FREE = 0, P_SMALL, P_LARGE, P_TAIL };

typedef struct Page {
    uint8_t kind, cls, atomic, lmark;  // lmark: mark bit of a large object (head page)
    uint32_t n;                        // large head: page count; tail: distance to head
} Page;

typedef struct Range {
    uintptr_t lo, hi;
} Range;

typedef struct Run {
    size_t start, len;
} Run;

#define NCLS 48
static const uint16_t cls_size[NCLS] = {
    16,   32,   48,   64,   80,   96,   112,  128,  144,  160,  176,  192,  208,  224,  240,  256,
    288,  320,  352,  384,  416,  448,  480,  512,  576,  640,  704,  768,  832,  896,  960,  1024,
    1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048, 2304, 2560, 2816, 3072, 3328, 3584, 3840, 4096};

// All collector state lives outside the data segment (which is scanned as a
// root): a free-list head stored there would keep every free block alive.
typedef struct GcState {
    uintptr_t lo;      // arena base
    size_t npages;     // reserved pages
    size_t hwm;        // pages ever handed out (scan bound)
    size_t committed;  // pages committed (Windows)
    Page *pages;
    uint64_t (*marks)[MARK_WORDS];
    void *freelist[2][NCLS];  // [atomic][class]
    uint8_t size_to_cls[SMALL_MAX / 16 + 1];
    uint16_t cls_count[NCLS];
    uint32_t cls_recip[NCLS];  // ceil(2^32 / size): offset * recip >> 32 == offset / size

    Run *runs;
    size_t nruns, capruns;

    Range *mstack;
    size_t msp, mcap;

    Range roots[64];
    int nroots;

    uintptr_t stack_base;
    size_t trigger;
    int gogc;
    size_t fight_budget;  // allocation allowed between collections while fighting
    bool fighting;
    int disabled;
    bool collecting;
    GcStats st;
} GcState;

static GcState *S;

// AddressSanitizer may move locals to a heap "fake stack" the collector does
// not scan; sanitizer builds keep locals on the real stack.
#if defined(__SANITIZE_ADDRESS__)
#define GC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GC_ASAN 1
#endif
#endif
#ifdef GC_ASAN
const char *__asan_default_options(void) { return "detect_stack_use_after_return=0"; }
#endif
#ifdef GC_PROFILE
static size_t prof_skip = (size_t)-1;
#endif

// ---------------------------------------------------------------- platform
static void *os_reserve(size_t n) {
#if defined(_WIN32)
    return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(MAP_ANONYMOUS) || defined(MAP_ANON)
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#else
    return calloc(1, n);  // consoles: a fixed block
#endif
}

static void os_commit(void *p, size_t n) {
#if defined(_WIN32)
    VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE);
#else
    (void)p;
    (void)n;
#endif
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void add_data_roots(void) {
#if defined(__linux__)
    extern char __data_start[], _end[];
    gc_add_roots(__data_start, _end);
#elif defined(__APPLE__)
    // Every writable segment (__DATA, __DATA_DIRTY, __DATA_CONST...) of the
    // image that contains the collector: the executable when the engine is
    // linked statically (desktop, the iOS app), a framework otherwise.
    // Newer linkers place writable data outside __DATA,__data/__bss/__common.
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    Dl_info info;
    if (dladdr((const void *)&gc_init, &info) && info.dli_fbase) mh = (const struct mach_header_64 *)info.dli_fbase;
    intptr_t slide = 0;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++)
        if ((const void *)_dyld_get_image_header(i) == (const void *)mh) slide = _dyld_get_image_vmaddr_slide(i);
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        if (seg->cmd != LC_SEGMENT_64 || !(seg->initprot & VM_PROT_WRITE) || !seg->vmsize) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)(seg->vmaddr + (uint64_t)slide);
        gc_add_roots(lo, lo + seg->vmsize);
    }
#elif defined(_WIN32)
    extern IMAGE_DOS_HEADER __ImageBase;
    uint8_t *base = (uint8_t *)&__ImageBase;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE)
            gc_add_roots(base + sh[i].VirtualAddress, base + sh[i].VirtualAddress + sh[i].Misc.VirtualSize);
#else
#error "gc: register the data/bss segment bounds for this platform"
#endif
}

static uintptr_t find_stack_base(void *hint) {
    uintptr_t b = hint ? (uintptr_t)hint : (uintptr_t)__builtin_frame_address(0);
#if defined(__linux__) && defined(__GLIBC__)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void *addr;
        size_t sz;
        if (pthread_attr_getstack(&a, &addr, &sz) == 0) b = (uintptr_t)addr + sz;
        pthread_attr_destroy(&a);
    }
#elif defined(__APPLE__)
    b = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#else
    b += 4096;  // cover main's own frame
#endif
    return b;
}

// ---------------------------------------------------------------- owner thread
// Only the thread that called gc_init may allocate or collect (the
// collector is single-threaded and scans only that thread's stacks).
// Checked on every collection; on every allocation too in debug builds.
#if !defined(_WIN32)
static pthread_t gc_owner;
static bool gc_owner_set;
static void owner_check(const char *what) {
    if (gc_owner_set && !pthread_equal(pthread_self(), gc_owner)) {
        fprintf(stderr, "gc: %s from a thread other than the engine thread\n", what);
        abort();
    }
}
#else
static void owner_check(const char *what) { (void)what; }
#endif

// ---------------------------------------------------------------- init
void gc_init(void *stack_base) {
    if (S) return;
    S = (GcState *)os_reserve(sizeof(GcState));
    if (!S) abort();
    os_commit(S, sizeof(GcState));
    memset(S, 0, sizeof *S);

    size_t mb = CVF_GC_DEFAULT_ARENA_MB;
    const char *e = getenv("CVF_GC_ARENA_MB");
    if (e && atoi(e) > 0) mb = (size_t)atoi(e);
    S->npages = (mb << 20) >> PAGE_SHIFT;
    void *arena = os_reserve(S->npages * PAGE_SIZE + PAGE_SIZE);
    if (!arena) {
        fprintf(stderr, "gc: cannot reserve %zu MB\n", mb);
        abort();
    }
    S->lo = ((uintptr_t)arena + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    S->pages = (Page *)os_reserve(S->npages * sizeof(Page));
    S->marks = (uint64_t(*)[MARK_WORDS])os_reserve(S->npages * sizeof *S->marks);
    if (!S->pages || !S->marks) abort();
    os_commit(S->pages, S->npages * sizeof(Page));
    os_commit(S->marks, S->npages * sizeof *S->marks);

    for (int c = 0, n = 0; n <= SMALL_MAX / 16; n++) {
        while (cls_size[c] < n * 16) c++;
        S->size_to_cls[n] = (uint8_t)c;
    }
    for (int c = 0; c < NCLS; c++) {
        S->cls_count[c] = (uint16_t)(PAGE_SIZE / cls_size[c]);
        S->cls_recip[c] = (uint32_t)(((uint64_t)1 << 32) / cls_size[c] + 1);
    }

    S->gogc = 100;
    if ((e = getenv("CVF_GOGC"))) S->gogc = atoi(e);
    S->trigger = (size_t)32 << 20;
    S->fight_budget = (size_t)(sizeof(void *) == 8 ? 256 : 48) << 20;
    if ((e = getenv("CVF_GC_FIGHT_BUDGET_MB")) && atoi(e) > 0) S->fight_budget = (size_t)atoi(e) << 20;

    S->stack_base = find_stack_base(stack_base);
#if !defined(_WIN32)
    gc_owner = pthread_self(), gc_owner_set = true;
#endif
#ifdef GC_PROFILE
    prof_skip = getenv("CVF_GC_PROF_SKIP_MB") ? (size_t)atoi(getenv("CVF_GC_PROF_SKIP_MB")) << 20 : 0;
#endif
    add_data_roots();
}

void gc_add_roots(void *lo, void *hi) {
    if (!S) gc_init(NULL);
    if (S->nroots == (int)(sizeof S->roots / sizeof S->roots[0])) abort();
    S->roots[S->nroots++] = (Range){(uintptr_t)lo, (uintptr_t)hi};
}

void gc_disable(void) {
    if (!S) gc_init(NULL);
    S->disabled++;
}
void gc_enable(void) { S->disabled--; }
GcStats gc_stats(void) { return S ? S->st : (GcStats){0}; }

// ---------------------------------------------------------------- pages
static inline uint8_t *page_addr(size_t pg) { return (uint8_t *)(S->lo + (pg << PAGE_SHIFT)); }

static void commit_to(size_t hwm) {
#if defined(_WIN32)
    if (hwm > S->committed) {
        size_t to = (hwm + 127) & ~(size_t)127;  // 1 MiB steps
        if (to > S->npages) to = S->npages;
        os_commit(page_addr(S->committed), (to - S->committed) * PAGE_SIZE);
        S->committed = to;
    }
#else
    (void)hwm;
#endif
}

// n contiguous free pages, or SIZE_MAX
static size_t take_pages(size_t n) {
    for (size_t i = 0; i < S->nruns; i++) {
        Run *r = &S->runs[i];
        if (r->len >= n) {
            size_t pg = r->start;
            r->start += n;
            r->len -= n;
            if (!r->len) memmove(r, r + 1, (--S->nruns - i) * sizeof *r);
            memset(page_addr(pg), 0, n * PAGE_SIZE);  // freed pages hold stale data
            return pg;
        }
    }
    if (S->hwm + n > S->npages) return SIZE_MAX;
    size_t pg = S->hwm;
    S->hwm += n;
    commit_to(S->hwm);
    return pg;  // fresh pages are zero
}

static void runs_push(size_t start, size_t len) {
    if (S->nruns == S->capruns) {
        S->capruns = S->capruns ? S->capruns * 2 : 256;
        S->runs = (Run *)realloc(S->runs, S->capruns * sizeof(Run));
    }
    S->runs[S->nruns++] = (Run){start, len};
}

// ---------------------------------------------------------------- mark
static inline void push_range(uintptr_t lo, uintptr_t hi) {
    if (S->msp == S->mcap) {
        S->mcap = S->mcap ? S->mcap * 2 : 4096;
        S->mstack = (Range *)realloc(S->mstack, S->mcap * sizeof(Range));
        if (!S->mstack) abort();
    }
    S->mstack[S->msp++] = (Range){lo, hi};
}

static inline void mark_ptr(uintptr_t p) {
    uintptr_t off = p - S->lo;
    if (off >= (S->hwm << PAGE_SHIFT)) return;
    size_t pg = off >> PAGE_SHIFT;
    Page *P = &S->pages[pg];
    switch (P->kind) {
        case P_SMALL: {
            uint32_t in = (uint32_t)(off & (PAGE_SIZE - 1));
            uint32_t idx = (uint32_t)(((uint64_t)in * S->cls_recip[P->cls]) >> 32);
            if (idx >= S->cls_count[P->cls]) return;
            uint64_t bit = (uint64_t)1 << (idx & 63), *w = &S->marks[pg][idx >> 6];
            if (*w & bit) return;
            *w |= bit;
            if (!P->atomic) {
                uintptr_t o = (uintptr_t)page_addr(pg) + (uintptr_t)idx * cls_size[P->cls];
                push_range(o, o + cls_size[P->cls]);
            }
            return;
        }
        case P_TAIL:
            pg -= P->n;
            P = &S->pages[pg];
            /* fallthrough */
        case P_LARGE:
            if (P->lmark) return;
            P->lmark = 1;
            if (!P->atomic) push_range((uintptr_t)page_addr(pg), (uintptr_t)page_addr(pg + P->n));
            return;
        default:
            return;
    }
}

// Conservative scanning reads whole segments and stack frames, including
// sanitizer redzones: exempt it from instrumentation.
#if defined(__clang__) || defined(__GNUC__)
#define GC_NO_SANITIZE __attribute__((no_sanitize_address, no_sanitize("undefined")))
#else
#define GC_NO_SANITIZE
#endif
GC_NO_SANITIZE static void scan_range(uintptr_t lo, uintptr_t hi) {
    lo = (lo + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    for (uintptr_t *w = (uintptr_t *)lo; (uintptr_t)(w + 1) <= hi; w++) mark_ptr(*w);
}

static void drain(void) {
    while (S->msp) {
        Range r = S->mstack[--S->msp];
        scan_range(r.lo, r.hi);
    }
}

uintptr_t (*gc_stack_top_hook)(void);
void (*gc_extra_roots_hook)(void (*scan)(uintptr_t lo, uintptr_t hi));
uintptr_t gc_main_stack_top(void) { return S ? S->stack_base : 0; }
void gc_set_main_stack(void *top) {
    if (!S) gc_init(top);
    S->stack_base = (uintptr_t)top;
}

static void scan_and_drain(uintptr_t lo, uintptr_t hi) {
    scan_range(lo, hi);
    drain();
}

GC_NO_SANITIZE static __attribute__((noinline)) void mark_stack(void) {
    jmp_buf jb;  // spills callee-saved registers onto the stack
    setjmp(jb);
    volatile uintptr_t here = (uintptr_t)&jb;
    uintptr_t lo = here < (uintptr_t)__builtin_frame_address(0) ? here : (uintptr_t)__builtin_frame_address(0);
    uintptr_t top = gc_stack_top_hook ? gc_stack_top_hook() : 0;
    scan_range(lo, top ? top : S->stack_base);  // the running goroutine's stack
    drain();
    if (gc_extra_roots_hook) gc_extra_roots_hook(scan_and_drain);  // suspended goroutines
}

// ---------------------------------------------------------------- sweep
static void sweep(void) {
    memset(S->freelist, 0, sizeof S->freelist);
    S->nruns = 0;
    size_t live = 0, heap = 0, live_atomic = 0, live_large = 0;
    // pages are visited high to low so each free list ends up in address order
    for (size_t pg = S->hwm; pg-- > 0;) {
        Page *P = &S->pages[pg];
        if (P->kind == P_SMALL) {
            int c = P->cls, cnt = S->cls_count[c], sz = cls_size[c];
            uint64_t *m = S->marks[pg];
            int nlive = 0;
            for (int w = 0; w < MARK_WORDS; w++) nlive += __builtin_popcountll(m[w]);
            if (!nlive) {
                P->kind = P_FREE;
                continue;
            }
            void **head = &S->freelist[P->atomic][c];
            uint8_t *base = page_addr(pg);
            for (int i = cnt; i-- > 0;)
                if (!(m[i >> 6] >> (i & 63) & 1)) {
                    void **o = (void **)(base + (size_t)i * sz);
                    *o = *head;
                    *head = o;
                }
            live += (size_t)nlive * sz;
            if (P->atomic) live_atomic += (size_t)nlive * sz;
            heap += PAGE_SIZE;
            memset(m, 0, sizeof S->marks[pg]);
        } else if (P->kind == P_LARGE) {
            if (!P->lmark) {
                for (size_t k = 0; k < P->n; k++) S->pages[pg + k].kind = P_FREE;
            } else {
                P->lmark = 0;
                live += P->n * PAGE_SIZE;
                live_large += P->n * PAGE_SIZE;
                if (P->atomic) live_atomic += P->n * PAGE_SIZE;
                heap += P->n * PAGE_SIZE;
            }
        }
    }
    // free page runs, low to high, coalesced (take_pages re-zeroes on reuse)
    for (size_t pg = 0; pg < S->hwm;) {
        Page *P = &S->pages[pg];
        if (P->kind != P_FREE) {
            pg += P->kind == P_LARGE ? P->n : 1;
            continue;
        }
        size_t s = pg;
        while (pg < S->hwm && S->pages[pg].kind == P_FREE) pg++;
        runs_push(s, pg - s);
    }
    S->st.live_bytes = live;
    S->st.heap_bytes = heap;
    if (getenv("CVF_GC_TRACE"))
        fprintf(stderr, "gc live: atomic %.1f MB, large %.1f MB, scanned %.1f MB\n", live_atomic / 1048576.0,
                live_large / 1048576.0, (live - live_atomic) / 1048576.0);
}

void gc_collect(void) {
    if (!S || S->collecting) return;
    owner_check("collection");
    S->collecting = true;
    double t0 = now_ms();
    size_t rootbytes = 0;
    for (int i = 0; i < S->nroots; i++) {
        scan_range(S->roots[i].lo, S->roots[i].hi);
        rootbytes += S->roots[i].hi - S->roots[i].lo;
    }
    double t1 = now_ms();
    drain();
    double t2 = now_ms();
    mark_stack();
    double t3 = now_ms();
    sweep();
    double dt = now_ms() - t0;
    if (getenv("CVF_GC_TRACE"))
        fprintf(stderr, "gc phases: roots %.2f ms (%zu KB) mark-from-roots %.2f ms stack %.2f ms sweep %.2f ms\n", t1 - t0,
                rootbytes >> 10, t2 - t1, t3 - t2, now_ms() - t3);
    S->st.collections++;
    S->st.last_pause_ms = dt;
    S->st.total_pause_ms += dt;
    if (dt > S->st.max_pause_ms) S->st.max_pause_ms = dt;
    S->st.alloc_bytes = 0;
    size_t t = S->st.live_bytes / 100 * (size_t)(S->gogc > 0 ? S->gogc : 100);
    S->trigger = t > ((size_t)32 << 20) ? t : ((size_t)32 << 20);
    S->collecting = false;
    if (getenv("CVF_GC_TRACE"))
        fprintf(stderr, "gc %llu: total %.1f MB live %.1f MB heap %.1f MB pause %.2f ms\n", (unsigned long long)S->st.collections, S->st.total_alloc / 1048576.0,
                S->st.live_bytes / 1048576.0, S->st.heap_bytes / 1048576.0, dt);
}

static inline bool should_collect(void) {
    if (S->disabled || S->gogc < 0) return false;
    if (S->fighting) return S->st.alloc_bytes >= S->trigger + S->fight_budget;
    return S->st.alloc_bytes >= S->trigger;
}

void gc_safepoint(bool fighting) {
    if (!S) return;
    S->fighting = fighting;
    if (!fighting && !S->disabled && S->gogc >= 0 && S->st.alloc_bytes >= S->trigger) gc_collect();
}

// ---------------------------------------------------------------- alloc
static void *refill(int atomic, int c) {
    if (should_collect()) {
        gc_collect();
        if (S->freelist[atomic][c]) return S->freelist[atomic][c];
    }
    size_t pg = take_pages(1);
    if (pg == SIZE_MAX) {
        gc_collect();
        if (S->freelist[atomic][c]) return S->freelist[atomic][c];
        pg = take_pages(1);
        if (pg == SIZE_MAX) {
            fprintf(stderr, "gc: out of memory\n");
            abort();
        }
    }
    Page *P = &S->pages[pg];
    P->kind = P_SMALL;
    P->cls = (uint8_t)c;
    P->atomic = (uint8_t)atomic;
    memset(S->marks[pg], 0, sizeof S->marks[pg]);
    uint8_t *base = page_addr(pg);
    int sz = cls_size[c];
    void *head = NULL;
    for (int i = S->cls_count[c]; i-- > 0;) {
        void **o = (void **)(base + (size_t)i * sz);
        *o = head;
        head = o;
    }
    S->freelist[atomic][c] = head;
    return head;
}

static void *alloc_large(size_t n, int atomic) {
    size_t np = (n + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (should_collect()) gc_collect();
    size_t pg = take_pages(np);
    if (pg == SIZE_MAX) {
        gc_collect();
        pg = take_pages(np);
        if (pg == SIZE_MAX) {
            fprintf(stderr, "gc: out of memory (%zu bytes)\n", n);
            abort();
        }
    }
    S->pages[pg] = (Page){P_LARGE, 0, (uint8_t)atomic, 0, (uint32_t)np};
    for (size_t k = 1; k < np; k++) S->pages[pg + k] = (Page){P_TAIL, 0, 0, 0, (uint32_t)k};
    S->st.alloc_bytes += np * PAGE_SIZE;
    S->st.total_alloc += np * PAGE_SIZE;
    return page_addr(pg);
}

#ifdef GC_PROFILE
// Allocation-site sampling (build with -DGC_PROFILE -g -fno-omit-frame-pointer):
// every 64th allocation records its call stack; dumped to gcprof.txt at exit.
#include <execinfo.h>
#define PROF_SLOTS 65536
#define PROF_DEPTH 6
typedef struct { void *pc[PROF_DEPTH]; uint64_t n, bytes; } ProfEnt;
static ProfEnt *prof;
static uint64_t prof_tick;
static void prof_dump(void) {
    FILE *f = fopen("gcprof.txt", "w");
    if (!f) return;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (prof[i].n) {
            fprintf(f, "%llu %llu", (unsigned long long)prof[i].n * 64, (unsigned long long)prof[i].bytes * 64);
            for (int k = 0; k < PROF_DEPTH; k++) fprintf(f, " %p", prof[i].pc[k]);
            fputc('\n', f);
        }
    fclose(f);
}
static __attribute__((noinline)) void prof_sample(size_t n) {
    if (!prof) {
        prof = (ProfEnt *)calloc(PROF_SLOTS, sizeof *prof);
        atexit(prof_dump);
    }
    void *bt[PROF_DEPTH + 3] = {0};
    int k = backtrace(bt, PROF_DEPTH + 3);
    (void)k;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < PROF_DEPTH; i++) h = (h ^ (uintptr_t)bt[i + 3]) * 1099511628211ull;
    for (size_t j = h % PROF_SLOTS;; j = (j + 1) % PROF_SLOTS) {
        if (!prof[j].n) memcpy(prof[j].pc, bt + 3, sizeof prof[j].pc);
        if (!memcmp(prof[j].pc, bt + 3, sizeof prof[j].pc)) {
            prof[j].n++;
            prof[j].bytes += n;
            break;
        }
    }
}
#define PROF(n) (++prof_tick % 64 == 0 && S->st.total_alloc >= prof_skip ? prof_sample(n) : (void)0)
#else
#define PROF(n) ((void)0)
#endif

static inline void *alloc_impl(size_t n, int atomic) {
    if (__builtin_expect(!S, 0)) gc_init(NULL);  // first allocation before main's gc_init
#ifndef NDEBUG
    owner_check("allocation");
#endif
    PROF(n);
    if (n > SMALL_MAX) return alloc_large(n, atomic);
    int c = S->size_to_cls[(n + 15) >> 4];
    void **o = (void **)S->freelist[atomic][c];
    if (!o) o = (void **)refill(atomic, c);
    S->freelist[atomic][c] = *o;
    memset(o, 0, cls_size[c]);
    S->st.alloc_bytes += cls_size[c];
    S->st.total_alloc += cls_size[c];
    return o;
}

void *gc_alloc(size_t n) { return alloc_impl(n, 0); }
void *gc_alloc_atomic(size_t n) { return alloc_impl(n, 1); }

static size_t block_size(void *p, int *atomic) {
    uintptr_t off = (uintptr_t)p - S->lo;
    size_t pg = off >> PAGE_SHIFT;
    Page *P = &S->pages[pg];
    if (P->kind == P_TAIL) P -= P->n;
    *atomic = P->atomic;
    return P->kind == P_SMALL ? cls_size[P->cls] : (size_t)P->n * PAGE_SIZE;
}

void *gc_realloc(void *p, size_t n) {
    if (!p) return gc_alloc(n);
    int atomic;
    size_t old = block_size(p, &atomic);
    if (n <= old) return p;
    void *q = alloc_impl(n, atomic);
    memcpy(q, p, old);
    return q;
}

void gc_free(void *p) { (void)p; }  // the collector reclaims it

char *gc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)gc_alloc_atomic(n);
    memcpy(p, s, n);
    return p;
}

// ---------------------------------------------------------------- self-test
// Each object is reachable only from one global of a different section
// kind (__data / __bss / common on Mach-O, .data / .bss on ELF).
#define ST_N 3
#define ST_SIZE 200
void *gc_st_tentative;                  // tentative definition (common or bss)
static void *gc_st_bss;                 // zero-initialised
static void *gc_st_data = (void *)1;    // initialised
static void **const gc_st_slot[ST_N] = {&gc_st_data, &gc_st_bss, &gc_st_tentative};

__attribute__((noinline)) static void st_fill(void) {
    for (int i = 0; i < ST_N; i++) {
        uint8_t *p = (uint8_t *)gc_alloc(ST_SIZE);
        memset(p, 0xA5 ^ i, ST_SIZE);
        *gc_st_slot[i] = p;
    }
}
__attribute__((noinline)) static void st_churn(void) {
    // garbage of the same size: a freed test object would be handed out again
    for (int k = 0; k < 20000; k++) {
        uint8_t *p = (uint8_t *)gc_alloc(ST_SIZE);
        memset(p, 0x5A, ST_SIZE);
    }
}
int gc_selftest(void) {
    if (!S) gc_init(NULL);
    st_fill();
    volatile uint8_t scrub[512];  // overwrite stale copies left on the stack
    memset((void *)scrub, 0, sizeof scrub);
    gc_collect();
    st_churn();
    gc_collect();
    st_churn();
    int fails = 0;
    for (int i = 0; i < ST_N; i++) {
        const uint8_t *p = (const uint8_t *)*gc_st_slot[i];
        for (int j = 0; j < ST_SIZE; j++)
            if (p[j] != (uint8_t)(0xA5 ^ i)) {
                fprintf(stderr, "gc selftest: object %d (root kind %d) was freed\n", i, i);
                fails++;
                break;
            }
        *gc_st_slot[i] = i == 0 ? (void *)1 : NULL;
    }
    return fails;
}
