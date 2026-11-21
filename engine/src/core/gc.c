#define _GNU_SOURCE  // pthread_getattr_np
// Conservative mark-sweep collector; see gc.h for the model.
#include "core/gc.h"
#include "core/sched.h"  // go_mono_ns

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__vita__)
// PS Vita (vitasdk newlib): no mmap; the arena is a memory block of its own
// (physical memory, committed at once), threads are Sce threads
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>  // sysconf
#if defined(__ANDROID__)
#include <link.h>  // dl_iterate_phdr
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
#if defined(__vita__)
// the Vita commits the whole arena in RAM: the reference match peaks at
// ~95 MB of heap pages (hwm, VITA_PORT.md); about 365 MB of main memory is
// the application's
#define CVF_GC_DEFAULT_ARENA_MB 192
#else
#define CVF_GC_DEFAULT_ARENA_MB (sizeof(void *) == 8 ? 4096 : 384)
#endif
#endif

#define PAGE_SHIFT 13
#define PAGE_SIZE ((size_t)1 << PAGE_SHIFT)
#define SMALL_MAX 4096
#define MAX_PER_PAGE (PAGE_SIZE / 16)
#define MARK_WORDS (MAX_PER_PAGE / 64)

enum { P_FREE = 0, P_SMALL, P_LARGE, P_TAIL };

typedef struct Page {
    uint8_t kind, cls, atomic, lmark;  // lmark: mark bit of a large object (head page); small: queued for sweep
    uint32_t n;                        // large head: page count; tail: distance to head; small: sweep queue link
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
    uint32_t sweepq[2][NCLS]; // pages waiting for a lazy sweep (page + 1, linked through Page.n)
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
    bool release;         // release_excess after collections (CVF_GC_RELEASE=0: never)
    size_t taken;         // pages take_pages handed out since the last collection
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

// 32-bit targets: the collector is conservative, so any scanned word whose
// value falls inside the used part of the heap keeps a block alive. A 32-bit
// float is such a word when the heap sits where floats live: Linux armhf
// under qemu maps the arena at 0x40800000, which is every float from ~4.0
// to ~1e15 (positions, velocities), and false hits kept whole sprite and
// sound buffers alive (live set 86 MB instead of 48 MB, VITA_PORT.md).
// Addresses with the top bit set read as negative integers below -2^31+2^28
// and as tiny negative floats, values game state rarely holds: ask for the
// arena there first. (The Vita's user memory blocks are at 0x81000000 up.)
static void *os_reserve_heap(size_t n) {
#if defined(__vita__)
    // user memory blocks are mapped from 0x81000000 up (the program image
    // first): negative as integers, tiny negative floats, as wanted above
    n = (n + 0xfffff) & ~(size_t)0xfffff;  // whole MBs (4 KB is the minimum)
    SceUID b = sceKernelAllocMemBlock("cvf_gc_arena", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, n, NULL);
    void *p = NULL;
    if (b < 0 || sceKernelGetMemBlockBase(b, &p) < 0) return NULL;
    memset(p, 0, n);  // take_pages assumes fresh pages are zero
    return p;
#endif
#if UINTPTR_MAX == 0xffffffffu && !defined(_WIN32) && (defined(MAP_ANONYMOUS) || defined(MAP_ANON))
    static const uintptr_t hints[] = {0x90000000u, 0xa0000000u, 0x80000000u};
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
        void *p = mmap((void *)hints[i], n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p >= 0x80000000u) return p;
        munmap(p, n);  // placed elsewhere: try the next hint
    }
#endif
    return os_reserve(n);
}

static void os_commit(void *p, size_t n) {
#if defined(_WIN32)
    VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE);
#else
    (void)p;
    (void)n;
#endif
}

// CPU time of the calling thread (ms; 0 where unavailable): the trace
// shows it beside each pause, which on a loaded machine includes preemption
static double thread_cpu_ms(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32) && !defined(__vita__)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
    return 0;
}

static double now_ms(void) {
#if defined(_WIN32) || defined(__vita__)
    // mingw-w64 on msvcrt (its default) has no timespec_get/TIME_UTC
    return (double)go_mono_ns() / 1e6;
#else
    struct timespec ts;
#if defined(__ANDROID__)
    clock_gettime(CLOCK_MONOTONIC, &ts);  // timespec_get needs API 29
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

#if defined(__ANDROID__)
// The engine is a shared library (libmain.so) loaded by the app: every
// writable PT_LOAD segment (.data, .bss, relro data) of the module that
// contains the collector. Also right for a static test executable.
static int add_module_roots(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    uintptr_t me = (uintptr_t)data;
    bool mine = false;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = (uintptr_t)info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && me >= lo && me < lo + ph->p_memsz) mine = true;
    }
    if (!mine) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W) || !ph->p_memsz) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)(info->dlpi_addr + ph->p_vaddr);
        gc_add_roots(lo, lo + ph->p_memsz);
    }
    return 1;
}
#endif

#if defined(__vita__)
__attribute__((section(".noinit"), used)) static char gc_vita_data_end[4];
#endif

static void add_data_roots(void) {
#if defined(__vita__)
    // vitasdk's linker script (armvita.x): .data, .data1, .persistent, .bss,
    // then .noinit. The end is a real object in .noinit, not __bss_end__: a
    // symbol at the very end of the segment is not relocated in the Vita
    // image (vita-elf-create; seen in vemu: __data_start moved with the
    // segment, __bss_end__ kept its link address, and objects referenced
    // only from .bss were freed - gc_selftest)
    extern char __data_start[];
    gc_add_roots(__data_start, gc_vita_data_end + sizeof gc_vita_data_end);
#elif defined(__ANDROID__)
    if (!dl_iterate_phdr(add_module_roots, (void *)(uintptr_t)&gc_init)) {
        fprintf(stderr, "gc: data segments of the engine not found\n");
        abort();
    }
#elif defined(__linux__)
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
#if defined(__linux__) && (defined(__GLIBC__) || defined(__BIONIC__))
    // the calling thread's stack (Android: the engine's own thread)
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void *addr;
        size_t sz;
        if (pthread_attr_getstack(&a, &addr, &sz) == 0) b = (uintptr_t)addr + sz;
        pthread_attr_destroy(&a);
    }
#elif defined(__APPLE__)
    b = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#elif defined(_WIN32)
    b = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;  // the thread's stack top: hint + 4096 may lie above it
#elif defined(__vita__)
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0 && ti.stack)
        b = (uintptr_t)ti.stack + (uintptr_t)ti.stackSize;
#else
    b += 4096;  // cover main's own frame
#endif
    return b;
}

// ---------------------------------------------------------------- owner thread
// Only the thread that called gc_init may allocate or collect (the
// collector is single-threaded and scans only that thread's stacks).
// Checked on every collection and every free-list refill / large allocation
// (all builds), and on every allocation in debug builds.
#if defined(__vita__)
static SceUID gc_owner;
static bool gc_owner_set;
static void owner_set(void) { gc_owner = sceKernelGetThreadId(), gc_owner_set = true; }
static bool owner_ok(void) { return !gc_owner_set || sceKernelGetThreadId() == gc_owner; }
#elif !defined(_WIN32)
static pthread_t gc_owner;
static bool gc_owner_set;
static void owner_set(void) { gc_owner = pthread_self(), gc_owner_set = true; }
static bool owner_ok(void) { return !gc_owner_set || pthread_equal(pthread_self(), gc_owner); }
#else
static DWORD gc_owner;
static bool gc_owner_set;
static void owner_set(void) { gc_owner = GetCurrentThreadId(), gc_owner_set = true; }
static bool owner_ok(void) { return !gc_owner_set || GetCurrentThreadId() == gc_owner; }
#endif
static void owner_check(const char *what) {
    if (!owner_ok()) {
        fprintf(stderr, "gc: %s from a thread other than the engine thread\n", what);
        abort();
    }
}

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
    void *arena = os_reserve_heap(S->npages * PAGE_SIZE + PAGE_SIZE);
    if (!arena) {
        fprintf(stderr, "gc: cannot reserve %lu MB\n", (unsigned long)mb);
        abort();
    }
    S->lo = ((uintptr_t)arena + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    if (getenv("CVF_GC_TRACE")) fprintf(stderr, "gc arena: %lu MB at %p\n", (unsigned long)mb, (void *)S->lo);
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
    S->fight_budget = (size_t)(sizeof(void *) == 8 ? 96 : 80) << 20;
    if ((e = getenv("CVF_GC_FIGHT_BUDGET_MB")) && atoi(e) > 0) S->fight_budget = (size_t)atoi(e) << 20;

#if defined(__vita__)
    S->release = false;  // the arena is one committed memory block: nothing to give back
#else
    S->release = true;
#endif
    if ((e = getenv("CVF_GC_RELEASE"))) S->release = e[0] != '0';
    S->stack_base = find_stack_base(stack_base);
    owner_set();
#ifdef GC_PROFILE
    prof_skip = getenv("CVF_GC_PROF_SKIP_MB") ? (size_t)atoi(getenv("CVF_GC_PROF_SKIP_MB")) << 20 : 0;
#endif
    add_data_roots();
    if (getenv("CVF_GC_TRACE"))
        for (int i = 0; i < S->nroots; i++)
            fprintf(stderr, "gc roots: %p-%p\n", (void *)S->roots[i].lo, (void *)S->roots[i].hi);
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
uint64_t gc_collections(void) { return S ? S->st.collections : 0; }
GcStats gc_stats(void) {
    if (!S) return (GcStats){0};
    GcStats st = S->st;
    st.hwm_bytes = S->hwm << PAGE_SHIFT;
    return st;
}

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
    S->taken += n;
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

#ifdef CVF_GC_WHY
static uintptr_t why_new;  // object start marked by the last mark_ptr call (0: none)
#define WHY_NEW(o) (why_new = (o))
#else
#define WHY_NEW(o) ((void)0)
#endif

// mark_ptr_in: lo / lim are S->lo and the used size of the arena, passed
// in by the scan loops (which keep them in registers: the stores to the mark
// stack would make the compiler reload them from S for every word).
static inline void mark_ptr_in(uintptr_t p, uintptr_t lo, uintptr_t lim) {
    uintptr_t off = p - lo;
    if (off >= lim) return;
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
            uintptr_t o = (uintptr_t)page_addr(pg) + (uintptr_t)idx * cls_size[P->cls];
            WHY_NEW(o);
            if (!P->atomic) push_range(o, o + cls_size[P->cls]);
            return;
        }
        case P_TAIL:
            pg -= P->n;
            P = &S->pages[pg];
            /* fallthrough */
        case P_LARGE:
            if (P->lmark) return;
            P->lmark = 1;
            WHY_NEW((uintptr_t)page_addr(pg));
            if (!P->atomic) push_range((uintptr_t)page_addr(pg), (uintptr_t)page_addr(pg + P->n));
            return;
        default:
            return;
    }
}
static inline void mark_ptr(uintptr_t p) { mark_ptr_in(p, S->lo, S->hwm << PAGE_SHIFT); }

// Conservative scanning reads whole segments and stack frames, including
// sanitizer redzones: exempt it from instrumentation.
#if defined(__clang__) || defined(__GNUC__)
#define GC_NO_SANITIZE __attribute__((no_sanitize_address, no_sanitize("undefined")))
#else
#define GC_NO_SANITIZE
#endif
#ifdef CVF_GC_WHY
// Retention diagnostics (a build with -DCVF_GC_WHY; development only).
// CVF_GC_WHY=MB: while the live set of the previous collection exceeded MB,
// every collection records, for each object it marks, the address of the
// word that marked it, and reports the roots that hold the most live bytes
// (first-come attribution: data segments, then stacks) with a sample chain
// from a big object up to its root. Data roots are printed relative to
// __data_start (resolve with nm), stack roots by stack range.
static uintptr_t *why_k, *why_v;  // open addressing: object start -> marking word (bit 0: derived)
static size_t why_cap;
static size_t why_mb;
static bool why_armed;
static uintptr_t why_w;  // the stack word mark_derived works for
static inline size_t why_slot(uintptr_t k) {
    size_t h = (size_t)((k >> 4) * 0x9E3779B97F4A7C15ull) & (why_cap - 1);
    while (why_k[h] && why_k[h] != k) h = (h + 1) & (why_cap - 1);
    return h;
}
static inline void why_note(uintptr_t w, uintptr_t tag) {
    if (!why_new) return;
    if (why_armed) {
        size_t h = why_slot(why_new);
        why_k[h] = why_new, why_v[h] = w | tag;
    }
    why_new = 0;
}
#define WHY_NOTE(w, tag) why_note((uintptr_t)(w), (tag))
#else
#define WHY_NOTE(w, tag) ((void)0)
#endif

GC_NO_SANITIZE static void scan_range(uintptr_t lo, uintptr_t hi) {
    lo = (lo + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    const uintptr_t alo = S->lo, alim = S->hwm << PAGE_SHIFT;
    for (uintptr_t *w = (uintptr_t *)lo; (uintptr_t)(w + 1) <= hi; w++) {
        mark_ptr_in(*w, alo, alim);
        WHY_NOTE(w, 0);
    }
}

// Derived pointers below an object (stacks and registers only). An
// optimising compiler may keep, across a call that collects, only a pointer
// *before* an object: GCC's pre-indexed loads on 32-bit ARM (`ldr r1, [r4,
// #4]!` walking an array leave `r4 = data - 4`). Such a word points into the
// previous block, or outside the heap, and alone would let the object be
// freed (test_sched.c's suspended-goroutine check under qemu-arm). So a
// stack or register word also marks every block that starts within
// DERIVED_BELOW bytes above it: 256 covers Thumb-2's and ARM's pre-indexed
// writeback with an 8-bit offset (255; ldr's 12-bit ARM form is not used for
// induction variables below the base) and every element stride a loop over
// slices of small structs uses. Heap and data-segment words are not treated
// this way: only compiler temporaries hold such pointers, and the translated
// code itself never stores them. The cost is retention of a neighbour now
// and then by a stack word, nothing on the 64-bit layouts' results.
#define DERIVED_BELOW 256
static void mark_derived(uintptr_t p) {
    uintptr_t lim = p + DERIVED_BELOW;
    uintptr_t end = S->lo + (S->hwm << PAGE_SHIFT);
    if (lim <= S->lo || p >= end) return;
    // first page whose start could hold such a block: p's own (p may lie
    // just below the arena too)
    size_t pg = p >= S->lo ? (p - S->lo) >> PAGE_SHIFT : 0;
    for (; pg < S->hwm; pg++) {
        uintptr_t base = (uintptr_t)page_addr(pg);
        if (base > lim) break;
        Page *P = &S->pages[pg];
        if (P->kind == P_SMALL) {
            uintptr_t sz = cls_size[P->cls];
            // slots starting in (p, lim]
            uintptr_t first = p < base ? 0 : (p - base) / sz + 1;
            for (uintptr_t i = first; i < S->cls_count[P->cls]; i++) {
                uintptr_t o = base + i * sz;
                if (o > lim) break;
                mark_ptr(o);  // a free slot is no object: marking it only delays its reuse
                WHY_NOTE(why_w, 1);
            }
        } else if (P->kind == P_LARGE && base > p) {
            mark_ptr(base);
            WHY_NOTE(why_w, 1);
        }
    }
}

GC_NO_SANITIZE static void scan_stack_range(uintptr_t lo, uintptr_t hi) {
    lo = (lo + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    for (uintptr_t *w = (uintptr_t *)lo; (uintptr_t)(w + 1) <= hi; w++) {
        mark_ptr(*w);
        WHY_NOTE(w, 0);
#ifdef CVF_GC_WHY
        why_w = (uintptr_t)w;
#endif
        mark_derived(*w);
    }
}

// Marking is bound by cache misses on the objects it visits: ranges taken
// off the mark stack wait in a small FIFO with their first line prefetched
// (as in the Boehm collector), so the load overlaps the scan of earlier ones.
#define PREFETCH_FIFO 16
// returns early (true) once the mark stack holds `handoff` ranges, with the
// FIFO's ranges put back: enough work for the parallel markers
static bool drain_seq(size_t handoff) {
    Range fifo[PREFETCH_FIFO];
    unsigned head = 0, n = 0;
    for (;;) {
        while (n < PREFETCH_FIFO && S->msp) {
            Range r = S->mstack[--S->msp];
            __builtin_prefetch((const void *)r.lo);
            fifo[(head + n++) % PREFETCH_FIFO] = r;
        }
        if (!n) return false;
        if (S->msp >= handoff) {
            for (; n; n--, head = (head + 1) % PREFETCH_FIFO) push_range(fifo[head].lo, fifo[head].hi);
            return true;
        }
        Range r = fifo[head];
        head = (head + 1) % PREFETCH_FIFO, n--;
        scan_range(r.lo, r.hi);
    }
}

// ---------------------------------------------------------------- parallel mark
// The pause is almost all marking, and marking is memory latency: helper
// threads mark beside the engine thread during the pause (the mutator is
// stopped: it is the thread that collects). Each marker has its own stack;
// spare work goes through a shared queue under a lock. Mark bits are set
// with atomic OR (32-bit words: the 64-bit mark words viewed as pairs on
// these little-endian targets), a large object's mark with an atomic
// exchange, so every object is pushed by exactly one marker. Nothing else
// changes: the roots and stacks are scanned by the engine thread as before,
// and sweeping starts after every helper has stopped. CVF_GC_THREADS=N sets
// the number of markers (default: half the CPU count, at most 4; 1 = none).
// Not on the Vita (one engine thread budget) or in diagnostic builds.
#if !defined(__vita__) && !defined(CVF_GC_WHY) && !defined(CVF_GC_NO_PAR)
#define GC_PAR 1
#endif

#ifdef GC_PAR
#if defined(_WIN32)
typedef SRWLOCK par_lock_t;
typedef CONDITION_VARIABLE par_cond_t;
#define PAR_LOCK(l) AcquireSRWLockExclusive(l)
#define PAR_UNLOCK(l) ReleaseSRWLockExclusive(l)
#define PAR_WAIT(c, l) SleepConditionVariableSRW(c, l, INFINITE, 0)
#define PAR_SIGNAL(c) WakeConditionVariable(c)
#define PAR_BROADCAST(c) WakeAllConditionVariable(c)
#define PAR_INIT(l, c) (InitializeSRWLock(l), InitializeConditionVariable(c))
#else
typedef pthread_mutex_t par_lock_t;
typedef pthread_cond_t par_cond_t;
#define PAR_LOCK(l) pthread_mutex_lock(l)
#define PAR_UNLOCK(l) pthread_mutex_unlock(l)
#define PAR_WAIT(c, l) pthread_cond_wait(c, l)
#define PAR_SIGNAL(c) pthread_cond_signal(c)
#define PAR_BROADCAST(c) pthread_cond_broadcast(c)
#define PAR_INIT(l, c) (pthread_mutex_init(l, NULL), pthread_cond_init(c, NULL))
#endif

#define PAR_MAX 8
#define PAR_MIN_WORK 256  // ranges on the mark stack before the other markers join
#define PAR_CHUNK 64      // ranges moved to or from the shared queue at once

typedef struct Marker {
    _Alignas(128) Range *st;  // a cache line pair of its own: markers write sp all the time
    size_t sp, cap;
} Marker;

static struct {
    int n;      // markers (engine thread included); < 2: no parallel marking
    bool started;
    par_lock_t lock;
    par_cond_t work, start, done;
    Range *q;   // shared queue
    size_t qn, qcap;
    _Alignas(128) int idle;  // read by every marker while it works
    _Alignas(128) int finished;
    unsigned gen;  // drain generation helpers wait for
    bool over;     // the current drain has no work left
    Marker m[PAR_MAX];
} P_;

static inline void mpush(Marker *m, uintptr_t lo, uintptr_t hi) {
    if (m->sp == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 4096;
        m->st = (Range *)realloc(m->st, m->cap * sizeof(Range));
        if (!m->st) abort();
    }
    m->st[m->sp++] = (Range){lo, hi};
}

static inline void par_mark_ptr(Marker *mk, uintptr_t p, uintptr_t lo, uintptr_t lim) {
    uintptr_t off = p - lo;
    if (off >= lim) return;
    size_t pg = off >> PAGE_SHIFT;
    Page *P = &S->pages[pg];
    switch (P->kind) {
        case P_SMALL: {
            uint32_t in = (uint32_t)(off & (PAGE_SIZE - 1));
            uint32_t idx = (uint32_t)(((uint64_t)in * S->cls_recip[P->cls]) >> 32);
            if (idx >= S->cls_count[P->cls]) return;
            uint32_t bit = (uint32_t)1 << (idx & 31), *w = (uint32_t *)S->marks[pg] + (idx >> 5);
            if (__atomic_load_n(w, __ATOMIC_RELAXED) & bit) return;
            if (__atomic_fetch_or(w, bit, __ATOMIC_RELAXED) & bit) return;
            if (!P->atomic) {
                uintptr_t o = (uintptr_t)page_addr(pg) + (uintptr_t)idx * cls_size[P->cls];
                mpush(mk, o, o + cls_size[P->cls]);
            }
            return;
        }
        case P_TAIL:
            pg -= P->n;
            P = &S->pages[pg];
            /* fallthrough */
        case P_LARGE:
            if (__atomic_load_n(&P->lmark, __ATOMIC_RELAXED)) return;
            if (__atomic_exchange_n(&P->lmark, 1, __ATOMIC_RELAXED)) return;
            if (!P->atomic) mpush(mk, (uintptr_t)page_addr(pg), (uintptr_t)page_addr(pg + P->n));
            return;
        default:
            return;
    }
}

GC_NO_SANITIZE static void par_scan(Marker *mk, uintptr_t lo, uintptr_t hi) {
    lo = (lo + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    const uintptr_t alo = S->lo, alim = S->hwm << PAGE_SHIFT;
    for (uintptr_t *w = (uintptr_t *)lo; (uintptr_t)(w + 1) <= hi; w++) par_mark_ptr(mk, *w, alo, alim);
}

// one marker until no work is left anywhere
static void par_work(Marker *mk) {
    Range fifo[PREFETCH_FIFO];
    unsigned head = 0, n = 0;
    for (;;) {
        while (n < PREFETCH_FIFO && mk->sp) {
            Range r = mk->st[--mk->sp];
            __builtin_prefetch((const void *)r.lo);
            fifo[(head + n++) % PREFETCH_FIFO] = r;
        }
        if (n) {
            Range r = fifo[head];
            head = (head + 1) % PREFETCH_FIFO, n--;
            par_scan(mk, r.lo, r.hi);
            // share the oldest part of a long stack while others wait (racy
            // read of idle: only a hint)
            if (mk->sp >= 2 * PAR_CHUNK && __atomic_load_n(&P_.idle, __ATOMIC_RELAXED)) {
                PAR_LOCK(&P_.lock);
                if (P_.qn + PAR_CHUNK > P_.qcap) {
                    P_.qcap = (P_.qn + PAR_CHUNK) * 2;
                    P_.q = (Range *)realloc(P_.q, P_.qcap * sizeof(Range));
                    if (!P_.q) abort();
                }
                memcpy(P_.q + P_.qn, mk->st, PAR_CHUNK * sizeof(Range));
                memmove(mk->st, mk->st + PAR_CHUNK, (mk->sp - PAR_CHUNK) * sizeof(Range));
                P_.qn += PAR_CHUNK, mk->sp -= PAR_CHUNK;
                PAR_BROADCAST(&P_.work);
                PAR_UNLOCK(&P_.lock);
            }
            continue;
        }
        // out of work: take from the queue, or wait until there is some or
        // every marker is idle (then the drain is over)
        PAR_LOCK(&P_.lock);
        for (;;) {
            if (P_.qn) {
                size_t k = P_.qn < PAR_CHUNK ? P_.qn : PAR_CHUNK;
                for (size_t i = 0; i < k; i++) mpush(mk, P_.q[P_.qn - 1 - i].lo, P_.q[P_.qn - 1 - i].hi);
                P_.qn -= k;
                break;
            }
            if (P_.over) break;
            __atomic_add_fetch(&P_.idle, 1, __ATOMIC_RELAXED);
            if (__atomic_load_n(&P_.idle, __ATOMIC_RELAXED) == P_.n) {
                P_.over = true;
                PAR_BROADCAST(&P_.work);
            } else {
                while (!P_.qn && !P_.over) PAR_WAIT(&P_.work, &P_.lock);
            }
            __atomic_sub_fetch(&P_.idle, 1, __ATOMIC_RELAXED);
        }
        bool over = !mk->sp && P_.over;
        PAR_UNLOCK(&P_.lock);
        if (over) return;
    }
}

#if defined(_WIN32)
static DWORD WINAPI par_helper(LPVOID arg)
#else
static void *par_helper(void *arg)
#endif
{
    Marker *mk = &P_.m[(intptr_t)arg];
    unsigned seen = 0;
    for (;;) {
        PAR_LOCK(&P_.lock);
        while (P_.gen == seen) PAR_WAIT(&P_.start, &P_.lock);
        seen = P_.gen;
        PAR_UNLOCK(&P_.lock);
        par_work(mk);
        PAR_LOCK(&P_.lock);
        if (++P_.finished == P_.n - 1) PAR_SIGNAL(&P_.done);
        PAR_UNLOCK(&P_.lock);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}

static void par_start(void) {
    P_.started = true;
    // half the CPUs, at most 4: a marker that is descheduled holds back the
    // whole pause (its stack is not stolen), so leave room for the render,
    // audio and system threads; on a 2-CPU machine marking stays sequential
    // (measured: 2 markers on 2 busy CPUs made the pause ~20 % longer, on 2
    // idle ones ~35 % shorter)
    int n = cpu_count() / 2;
    if (n > 4) n = 4;
    const char *e = getenv("CVF_GC_THREADS");
    if (e && atoi(e) > 0) n = atoi(e);
    if (n > PAR_MAX) n = PAR_MAX;
    if (n < 2) return;
    PAR_INIT(&P_.lock, &P_.work);
    PAR_INIT(&P_.lock, &P_.start);
    PAR_INIT(&P_.lock, &P_.done);
    int started = 1;
    for (int i = 1; i < n; i++) {
#if defined(_WIN32)
        HANDLE h = CreateThread(NULL, 256 * 1024, par_helper, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) break;
        CloseHandle(h);
#else
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        int r = pthread_create(&t, &a, par_helper, (void *)(intptr_t)i);
        pthread_attr_destroy(&a);
        if (r) break;
#endif
        started++;
    }
    P_.n = started;
    if (getenv("CVF_GC_TRACE")) fprintf(stderr, "gc markers: %d\n", P_.n);
}

// drains the engine thread's mark stack with every marker
static void par_drain(void) {
    PAR_LOCK(&P_.lock);
    if (P_.qcap < S->msp) {
        P_.qcap = S->msp * 2;
        P_.q = (Range *)realloc(P_.q, P_.qcap * sizeof(Range));
        if (!P_.q) abort();
    }
    memcpy(P_.q, S->mstack, S->msp * sizeof(Range));
    P_.qn = S->msp;
    S->msp = 0;
    P_.over = false;
    P_.idle = 0;
    P_.finished = 0;
    P_.gen++;
    PAR_BROADCAST(&P_.start);
    PAR_UNLOCK(&P_.lock);
    par_work(&P_.m[0]);
    PAR_LOCK(&P_.lock);
    while (P_.finished < P_.n - 1) PAR_WAIT(&P_.done, &P_.lock);
    PAR_UNLOCK(&P_.lock);
}
#endif  // GC_PAR

static void drain(void) {
#ifdef GC_PAR
    if (!P_.started) par_start();
    if (P_.n > 1) {
        if (drain_seq(PAR_MIN_WORK)) par_drain();
        return;
    }
#endif
    drain_seq(SIZE_MAX);
}

uintptr_t (*gc_stack_top_hook)(void);
void (*gc_extra_roots_hook)(void (*scan)(uintptr_t lo, uintptr_t hi));
uintptr_t gc_main_stack_top(void) { return S ? S->stack_base : 0; }
void gc_set_main_stack(void *top) {
    if (!S) gc_init(top);
    if (top) S->stack_base = (uintptr_t)top;  // NULL (no engine stack): keep the thread's own
}

static void scan_and_drain(uintptr_t lo, uintptr_t hi) {  // suspended goroutine stacks
    scan_stack_range(lo, hi);
    drain();
}

GC_NO_SANITIZE static __attribute__((noinline)) void mark_stack(void) {
    struct {
        jmp_buf jb;
        uintptr_t regs[12];
    } spill;  // callee-saved registers, on the stack
    setjmp(spill.jb);
    // setjmp may store registers XORed with a cookie (bionic: x19-x30 on
    // arm64, r4-r11 on arm32; glibc: rbp on x86-64, ebp/esp on i686), which
    // hides a pointer held only in a register: store them in clear too.
    // (A register this function uses itself is saved by its prologue, above
    // `spill`, inside the scanned range.)
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x19, x20, [%0, #0]\n stp x21, x22, [%0, #16]\n stp x23, x24, [%0, #32]\n"
        "stp x25, x26, [%0, #48]\n stp x27, x28, [%0, #64]\n stp x29, x30, [%0, #80]\n" ::"r"(spill.regs)
        : "memory");
#elif defined(__x86_64__) && !defined(_WIN32)
    __asm__ volatile(
        "movq %%rbx, 0(%0)\n movq %%rbp, 8(%0)\n movq %%r12, 16(%0)\n movq %%r13, 24(%0)\n"
        "movq %%r14, 32(%0)\n movq %%r15, 40(%0)\n" ::"r"(spill.regs)
        : "memory");
#elif defined(__x86_64__) && defined(_WIN32)
    // msvcrt's setjmp stores them unmangled, but the layout is the CRT's:
    // keep the Win64 callee-saved registers (rsi and rdi too) here as well.
    __asm__ volatile(
        "movq %%rbx, 0(%0)\n movq %%rbp, 8(%0)\n movq %%rsi, 16(%0)\n movq %%rdi, 24(%0)\n"
        "movq %%r12, 32(%0)\n movq %%r13, 40(%0)\n movq %%r14, 48(%0)\n movq %%r15, 56(%0)\n" ::"r"(spill.regs)
        : "memory");
#elif defined(__arm__)
    // AAPCS callee-saved r4-r11 (r7 / r11: the Thumb / ARM frame pointer)
    __asm__ volatile("stmia %0, {r4-r11}\n" ::"r"(spill.regs) : "memory");
#elif defined(__i386__)
    // cdecl callee-saved ebx, esi, edi, ebp
    __asm__ volatile("movl %%ebx, 0(%0)\n movl %%esi, 4(%0)\n movl %%edi, 8(%0)\n movl %%ebp, 12(%0)\n" ::"r"(spill.regs)
                     : "memory");
#endif
    volatile uintptr_t here = (uintptr_t)&spill;
    uintptr_t lo = here < (uintptr_t)__builtin_frame_address(0) ? here : (uintptr_t)__builtin_frame_address(0);
    uintptr_t top = gc_stack_top_hook ? gc_stack_top_hook() : 0;
    scan_stack_range(lo, top ? top : S->stack_base);  // the running goroutine's stack (and the spilled registers)
    drain();
    if (gc_extra_roots_hook) gc_extra_roots_hook(scan_and_drain);  // suspended goroutines
}

#ifdef CVF_GC_WHY
static uintptr_t obj_start(uintptr_t p, size_t *size) {
    size_t pg = (p - S->lo) >> PAGE_SHIFT;
    Page *P = &S->pages[pg];
    if (P->kind == P_TAIL) pg -= P->n, P = &S->pages[pg];
    if (P->kind == P_LARGE) {
        *size = (size_t)P->n * PAGE_SIZE;
        return (uintptr_t)page_addr(pg);
    }
    uint32_t in = (uint32_t)((p - S->lo) & (PAGE_SIZE - 1));
    uint32_t idx = (uint32_t)(((uint64_t)in * S->cls_recip[P->cls]) >> 32);
    *size = cls_size[P->cls];
    return (uintptr_t)page_addr(pg) + (uintptr_t)idx * cls_size[P->cls];
}
static bool in_heap(uintptr_t p) { return p - S->lo < (S->hwm << PAGE_SHIFT); }
typedef struct WhyRoot {
    uintptr_t root;  // marking word outside the heap (bit 0: derived)
    size_t bytes, objs, big_size;
    uintptr_t big;   // the biggest object attributed to it
} WhyRoot;
static uintptr_t *why_r;  // per table slot: resolved root (0: not yet)
static uintptr_t why_root_of(uintptr_t o) {
    // follow marking words up to a word outside the heap, then remember the
    // root for every object on the way
    static uintptr_t *path;
    static size_t cap;
    size_t n = 0;
    uintptr_t root = 3;
    for (;;) {
        size_t h = why_slot(o);
        if (!why_k[h]) {
            root = 2;  // unknown (marked before arming)
            break;
        }
        if (why_r[h]) {
            root = why_r[h];
            break;
        }
        if (n == cap) path = (uintptr_t *)realloc(path, (cap = cap ? cap * 2 : 4096) * sizeof *path);
        path[n++] = h;
        uintptr_t w = why_v[h];
        if (!in_heap(w & ~(uintptr_t)1)) {
            root = w;
            break;
        }
        size_t sz;
        o = obj_start(w & ~(uintptr_t)1, &sz);
    }
    for (size_t i = 0; i < n; i++) why_r[path[i]] = root;
    return root;
}
static void why_report(void) {
    size_t nr = 0, capr = 1 << 16;
    WhyRoot *r = (WhyRoot *)calloc(capr, sizeof *r);
    uintptr_t *rk = (uintptr_t *)calloc(capr * 2, sizeof *rk);
    size_t *ri = (size_t *)calloc(capr * 2, sizeof *ri);
    for (size_t pg = 0; pg < S->hwm; pg++) {
        Page *P = &S->pages[pg];
        size_t n = 0, sz = 0;
        if (P->kind == P_SMALL) n = S->cls_count[P->cls], sz = cls_size[P->cls];
        else if (P->kind == P_LARGE && P->lmark) n = 1, sz = (size_t)P->n * PAGE_SIZE;
        for (size_t i = 0; i < n; i++) {
            if (P->kind == P_SMALL && !(S->marks[pg][i >> 6] >> (i & 63) & 1)) continue;
            uintptr_t o = (uintptr_t)page_addr(pg) + i * sz;
            uintptr_t root = why_root_of(o);
            size_t h = (size_t)((root >> 3) * 0x9E3779B97F4A7C15ull) & (capr * 2 - 1);
            while (rk[h] && rk[h] != root) h = (h + 1) & (capr * 2 - 1);
            if (!rk[h]) {
                if (nr == capr) continue;
                rk[h] = root, ri[h] = nr, r[nr++].root = root;
            }
            WhyRoot *e = &r[ri[h]];
            e->bytes += sz, e->objs++;
            if (sz > e->big_size) e->big_size = sz, e->big = o;
        }
    }
    // top 15 by bytes
    extern char __data_start[];
    fprintf(stderr, "gc why: live %.1f MB, %lu roots; __data_start %p\n", S->st.live_bytes / 1048576.0, (unsigned long)nr,
            (void *)__data_start);
    for (int t = 0; t < 15; t++) {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < nr; i++)
            if (r[i].bytes && (best == SIZE_MAX || r[i].bytes > r[best].bytes)) best = i;
        if (best == SIZE_MAX) break;
        WhyRoot *e = &r[best];
        uintptr_t w = e->root & ~(uintptr_t)1;
        bool data = false;
        for (int i = 0; i < S->nroots; i++) data |= w >= S->roots[i].lo && w < S->roots[i].hi;
        fprintf(stderr, "gc why: %8.2f MB %7lu objs root %s%s %p (data+0x%lx)\n", e->bytes / 1048576.0, (unsigned long)e->objs,
                data ? "data" : (e->root < 4 ? "?" : "stack"), e->root & 1 ? " derived" : "", (void *)w,
                (unsigned long)(w - (uintptr_t)__data_start));
        // chain from the biggest object to the root
        uintptr_t o = e->big;
        for (int hop = 0; hop < 400 && o; hop++) {
            size_t h = why_slot(o), sz;
            obj_start(o, &sz);
            if (!why_k[h]) break;
            uintptr_t mw = why_v[h] & ~(uintptr_t)1;
            if (!in_heap(mw)) {
                fprintf(stderr, "gc why:     %p (%lu B) <- root word %p\n", (void *)o, (unsigned long)sz, (void *)mw);
                break;
            }
            size_t psz;
            uintptr_t po = obj_start(mw, &psz);
            fprintf(stderr, "gc why:     %p (%lu B) <- %p+%lu (%lu B)%s\n", (void *)o, (unsigned long)sz, (void *)po,
                    (unsigned long)(mw - po), (unsigned long)psz, why_v[h] & 1 ? " derived" : "");
            o = po;
        }
        e->bytes = 0;
    }
    free(r), free(rk), free(ri);
}
#endif

// ---------------------------------------------------------------- sweep
// Small pages are swept lazily: the collection only counts each page's
// marks, frees the empty ones and queues the others by size class with
// their mark bits kept; refill() builds a page's free list from them when
// its class needs space. So the pause does not touch the dead objects,
// and a page is swept only if its class allocates again before the next
// collection (whose start clears the marks of pages still queued: their
// unmarked slots are found dead again).
static void unqueue_unswept(void) {
    for (int a = 0; a < 2; a++)
        for (int c = 0; c < NCLS; c++) {
            for (uint32_t pg1 = S->sweepq[a][c]; pg1;) {
                Page *P = &S->pages[pg1 - 1];
                memset(S->marks[pg1 - 1], 0, sizeof S->marks[0]);
                P->lmark = 0;
                pg1 = P->n;
                P->n = 0;
            }
            S->sweepq[a][c] = 0;
        }
}

// builds the free list of a queued page from its marks (slots in address order)
static void *sweep_page(size_t pg) {
    Page *P = &S->pages[pg];
    int c = P->cls, cnt = S->cls_count[c], sz = cls_size[c];
    uint64_t *m = S->marks[pg];
    uint8_t *base = page_addr(pg);
    void *head = NULL;
    for (int i = cnt; i-- > 0;)
        if (!(m[i >> 6] >> (i & 63) & 1)) {
            void **o = (void **)(base + (size_t)i * sz);
            *o = head;
            head = o;
        }
    memset(m, 0, sizeof S->marks[pg]);
    P->lmark = 0;
    return head;
}

static void sweep(void) {
    memset(S->freelist, 0, sizeof S->freelist);
    S->nruns = 0;
    size_t live = 0, heap = 0, live_atomic = 0, live_large = 0;
    // pages are visited high to low so each sweep queue is in address order
    for (size_t pg = S->hwm; pg-- > 0;) {
        Page *P = &S->pages[pg];
        if (P->kind == P_SMALL) {
            int c = P->cls, sz = cls_size[c];
            uint64_t *m = S->marks[pg];
            int nlive = 0;
            for (int w = 0; w < MARK_WORDS; w++) nlive += __builtin_popcountll(m[w]);
            if (!nlive) {
#ifdef CVF_GC_STRESS
                memset(page_addr(pg), 0xA5, PAGE_SIZE);
#endif
                P->kind = P_FREE;
                continue;
            }
#ifdef CVF_GC_STRESS
            // poison the dead objects now (a missed root reads garbage at
            // once), not when the page is swept
            uint8_t *base = page_addr(pg);
            for (int i = 0; i < S->cls_count[c]; i++)
                if (!(m[i >> 6] >> (i & 63) & 1)) memset(base + (size_t)i * sz, 0xA5, (size_t)sz);
#endif
            // queued for refill(): P->lmark flags a queued page, P->n links the queue
            P->lmark = 1;
            P->n = S->sweepq[P->atomic][c];
            S->sweepq[P->atomic][c] = (uint32_t)pg + 1;
            live += (size_t)nlive * sz;
            if (P->atomic) live_atomic += (size_t)nlive * sz;
            heap += PAGE_SIZE;
        } else if (P->kind == P_LARGE) {
            if (!P->lmark) {
#ifdef CVF_GC_STRESS
                memset(page_addr(pg), 0xA5, P->n * PAGE_SIZE);
#endif
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

static void release_excess(void);

void gc_collect(void) {
    if (!S || S->collecting) return;
    owner_check("collection");
    S->collecting = true;
    double t0 = now_ms(), c0 = thread_cpu_ms();
#ifdef CVF_GC_WHY
    if (!why_cap && getenv("CVF_GC_WHY")) {
        why_mb = (size_t)atoi(getenv("CVF_GC_WHY"));
        why_cap = (size_t)1 << 24;
        why_k = (uintptr_t *)os_reserve(why_cap * sizeof *why_k);
        why_v = (uintptr_t *)os_reserve(why_cap * sizeof *why_v);
        why_r = (uintptr_t *)os_reserve(why_cap * sizeof *why_r);
    }
    why_armed = why_cap && S->st.live_bytes >= (why_mb << 20);
    if (why_armed) memset(why_k, 0, why_cap * sizeof *why_k), memset(why_r, 0, why_cap * sizeof *why_r);
    why_new = 0;
#endif
    unqueue_unswept();
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
#ifdef CVF_GC_WHY
    if (why_armed) why_report();
#endif
    sweep();
    double dt = now_ms() - t0, dc = thread_cpu_ms() - c0;
    if (getenv("CVF_GC_TRACE"))
        fprintf(stderr, "gc phases: roots %.2f ms (%lu KB) mark-from-roots %.2f ms stack %.2f ms sweep %.2f ms\n", t1 - t0,
                (unsigned long)(rootbytes >> 10), t2 - t1, t3 - t2, now_ms() - t3);
    S->st.collections++;
    S->st.last_pause_ms = dt;
    S->st.total_pause_ms += dt;
    if (dt > S->st.max_pause_ms) S->st.max_pause_ms = dt;
    S->st.alloc_bytes = 0;
    size_t t = S->st.live_bytes / 100 * (size_t)(S->gogc > 0 ? S->gogc : 100);
    S->trigger = t > ((size_t)32 << 20) ? t : ((size_t)32 << 20);
    release_excess();
    S->collecting = false;
    if (getenv("CVF_GC_TRACE"))
        fprintf(stderr, "gc %llu: total %.1f MB live %.1f MB heap %.1f MB hwm %.1f MB pause %.2f ms (cpu %.2f) released %.1f MB%s\n", (unsigned long long)S->st.collections, S->st.total_alloc / 1048576.0,
                S->st.live_bytes / 1048576.0, S->st.heap_bytes / 1048576.0, (double)(S->hwm << PAGE_SHIFT) / 1048576.0, dt, dc,
                S->st.released_bytes / 1048576.0, S->fighting ? " (during a round)" : "");
}

// Hands free pages back to the OS: a collection alone leaves freed pages
// dirty, so the process footprint (RSS; what iOS's jetsam and Android's
// low-memory killer measure) would stay at the highest heap ever used.
// take_pages re-zeroes a run when it is reused, so the contents may go.
// P_FREE pages use lmark as "already released".
static size_t release_pages(size_t pg, size_t n) {
    size_t done = 0;
    for (size_t k = pg; k < pg + n;) {
        if (S->pages[k].lmark) {
            k++;
            continue;
        }
        size_t e = k;
        while (e < pg + n && !S->pages[e].lmark) S->pages[e++].lmark = 1;
        uint8_t *p = page_addr(k);
        size_t len = (e - k) * PAGE_SIZE;
#if defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
        if (madvise(p, len, MADV_FREE_REUSABLE) == 0) done += len;
#elif defined(__linux__) || defined(__ANDROID__)
        if (madvise(p, len, MADV_DONTNEED) == 0) done += len;
#elif defined(_WIN32)
        if (VirtualAlloc(p, len, MEM_RESET, PAGE_READWRITE)) done += len;
#else
        (void)p, (void)len;  // consoles: the arena is committed memory
#endif
        k = e;
    }
    return done;
}

// memory warnings: every free page
size_t gc_release_free_pages(void) {
    if (!S) return 0;
    size_t released = 0;
    for (size_t i = 0; i < S->nruns; i++) released += release_pages(S->runs[i].start, S->runs[i].len);
    S->st.released_bytes += released;
    return released;
}

// After every collection: keep as many free pages resident as the next
// cycle may allocate (trigger + fight budget), lowest addresses first
// (take_pages is first fit, so those are reused first), release the rest.
static void release_excess(void) {
    if (!S->release) return;
    // what the next cycle may allocate, less what the free slots of the
    // partly used pages hold (lazy sweeping hands those out first), at least
    // 8 MB
    // 8 MB; and at least as many pages as the last cycle took (within that
    // allowance), or each cycle would fault the same pages in again
    size_t allow = S->trigger > S->fight_budget ? S->trigger : S->fight_budget, slack = S->st.heap_bytes - S->st.live_bytes;
    size_t keepb = allow > slack + ((size_t)8 << 20) ? allow - slack : (size_t)8 << 20;
    size_t keep = keepb >> PAGE_SHIFT, kept = 0, taken = S->taken;
    S->taken = 0;
    if (taken > keep) keep = taken < (allow >> PAGE_SHIFT) ? taken : allow >> PAGE_SHIFT;
    for (size_t i = 0; i < S->nruns; i++) {
        Run *r = &S->runs[i];
        size_t k = r->start, end = r->start + r->len;
        for (; k < end && kept < keep; k++) kept += !S->pages[k].lmark;
        if (k < end) S->st.released_bytes += release_pages(k, end - k);
    }
}

static inline bool should_collect(void) {
    if (S->disabled || S->gogc < 0) return false;
    if (S->fighting) return S->st.alloc_bytes >= (S->trigger > S->fight_budget ? S->trigger : S->fight_budget);
    return S->st.alloc_bytes >= S->trigger;
}

void (*gc_frame_hook)(bool fighting);

void gc_safepoint(bool fighting) {
    if (gc_frame_hook) gc_frame_hook(fighting);
    if (!S) return;
    S->fighting = fighting;
    if (!fighting && !S->disabled && S->gogc >= 0 && S->st.alloc_bytes >= S->trigger) gc_collect();
}

// ---------------------------------------------------------------- alloc
// the free list of the next queued page of a class that has free slots
static void *sweep_queued(int atomic, int c) {
    while (S->sweepq[atomic][c]) {
        size_t qp = S->sweepq[atomic][c] - 1;
        S->sweepq[atomic][c] = S->pages[qp].n;
        S->pages[qp].n = 0;
        void *head = sweep_page(qp);
        if (head) return head;
    }
    return NULL;
}

static void *refill(int atomic, int c) {
    owner_check("allocation");  // once per page of a size class: cheap enough for Release
    if (should_collect()) gc_collect();
    void *head = sweep_queued(atomic, c);
    if (head) return S->freelist[atomic][c] = head;
    size_t pg = take_pages(1);
    if (pg == SIZE_MAX) {
        gc_collect();
        if ((head = sweep_queued(atomic, c))) return S->freelist[atomic][c] = head;
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
    P->lmark = 0;
    P->n = 0;
    memset(S->marks[pg], 0, sizeof S->marks[pg]);
    uint8_t *base = page_addr(pg);
    int sz = cls_size[c];
    for (int i = S->cls_count[c]; i-- > 0;) {
        void **o = (void **)(base + (size_t)i * sz);
        *o = head;
        head = o;
    }
    S->freelist[atomic][c] = head;
    return head;
}

static void *alloc_large(size_t n, int atomic) {
    owner_check("allocation");
    size_t np = (n + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (should_collect()) gc_collect();
    size_t pg = take_pages(np);
    if (pg == SIZE_MAX) {
        gc_collect();
        pg = take_pages(np);
        if (pg == SIZE_MAX) {
            fprintf(stderr, "gc: out of memory (%lu bytes)\n", (unsigned long)n);
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

#ifdef CVF_GC_STRESS
// Stress mode (build with -DCVF_GC_STRESS): CVF_GC_STRESS=N collects on
// every Nth allocation, wherever it happens, to expose missed roots.
static long stress_every = -1, stress_count;
static inline void stress_tick(void) {
    if (stress_every < 0) {
        const char *e = getenv("CVF_GC_STRESS");
        stress_every = e && atol(e) > 0 ? atol(e) : 0;
    }
    if (stress_every && ++stress_count >= stress_every && !S->disabled && !S->collecting) {
        stress_count = 0;
        gc_collect();
    }
}
#else
#define stress_tick() ((void)0)
#endif

static inline void *alloc_impl(size_t n, int atomic) {
    if (__builtin_expect(!S, 0)) gc_init(NULL);  // first allocation before main's gc_init
#ifndef NDEBUG
    owner_check("allocation");
#endif
    stress_tick();
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
