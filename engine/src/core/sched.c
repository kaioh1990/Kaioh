// Cooperative goroutines and channels; see sched.h.
#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(_WIN32)
#define _XOPEN_SOURCE 700  // ucontext fallback
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L  // nanosleep, clock_gettime
#define _DEFAULT_SOURCE           // MAP_ANONYMOUS (glibc)
#define _DARWIN_C_SOURCE          // MAP_ANON (Darwin)
#endif
#include "core/sched.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/gc.h"

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod (winmm)
#elif !defined(__x86_64__) && !defined(__aarch64__)
#include <ucontext.h>
#endif

// Goroutine stacks: mmap with a PROT_NONE guard page below each one, so an
// overflow faults at the overflowing frame instead of corrupting the heap.
#if !defined(_WIN32) && (defined(__APPLE__) || defined(__linux__))
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
static size_t page_size(void) {
    static size_t pg;
    if (!pg) pg = (size_t)sysconf(_SC_PAGESIZE);
    return pg;
}
static uint8_t *stack_alloc(size_t n) {
    size_t pg = page_size();
    uint8_t *p = (uint8_t *)mmap(NULL, n + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    mprotect(p, pg, PROT_NONE);
    return p + pg;
}
static void stack_free(uint8_t *s, size_t n) {
    if (s) munmap(s - page_size(), n + page_size());
}
#else
#if !defined(_WIN32)  // Windows: CreateFiberEx allocates the stack, DeleteFiber frees it
static uint8_t *stack_alloc(size_t n) { return (uint8_t *)malloc(n); }
#endif
static void stack_free(uint8_t *s, size_t n) {
    (void)n;
    free(s);
}
#endif

// ---------------------------------------------------------------- context switch
// ctx_switch(save, to): saves the callee-saved registers on the current
// stack, stores the stack pointer in *save and resumes the context whose
// stack pointer is `to`.
#if defined(__APPLE__)
#define CSYM(n) "_" #n
#else
#define CSYM(n) #n
#endif

#if defined(_WIN32)
// Windows: fibers.
#elif defined(__x86_64__)
void ctx_switch(void **save, void *to);
__asm__(".text\n"
        ".globl " CSYM(ctx_switch) "\n"
#if !defined(__APPLE__)
        ".type ctx_switch,@function\n"
#endif
        CSYM(ctx_switch) ":\n"
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n");
#define CTX_FRAME_WORDS 6
#elif defined(__aarch64__)
void ctx_switch(void **save, void *to);
__asm__(".text\n"
        ".globl " CSYM(ctx_switch) "\n"
        ".p2align 2\n"
        CSYM(ctx_switch) ":\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n"
        "  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n"
        "  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n"
        "  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mov x2, sp\n"
        "  str x2, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n"
        "  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n"
        "  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n"
        "  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n"
        "  ret\n");
#endif

// ---------------------------------------------------------------- goroutines
enum { G_RUNNABLE, G_RUNNING, G_BLOCKED, G_SLEEPING, G_DEAD };

struct GoPanicFrame;
extern struct GoPanicFrame *go_panic_top;

typedef struct Coro {
    void *sp;          // saved stack pointer while suspended
    uint8_t *stack;    // lowest address (NULL: the main thread's stack)
    size_t stack_size;
    void (*fn)(void *);
    void *env;         // kept here so the collector sees it (Coro is GC memory)
    int state;
    bool waiting;      // parked on channel/select waiters
    int chosen;        // select: case that woke us
    int64_t wake;      // sleeping until (go_mono_ns)
    struct Coro *next; // run queue / sleep list
    struct Coro *allnext;
    struct GoPanicFrame *panic_top;  // innermost recover frame of this goroutine (gort.h)
#if defined(_WIN32)
    void *fiber;
    uintptr_t fiber_top;  // its stack's upper end (TEB StackBase), once started
#elif !defined(__x86_64__) && !defined(__aarch64__)
    ucontext_t uc;
#endif
} Coro;

// Roots: these statics live in the data segment, which the collector scans.
static Coro main_coro;
static Coro *cur = &main_coro;
static Coro *all_coros;  // every live goroutine (keeps their Coro and env alive)
static Coro *runq_head, *runq_tail, *sleepers;
static Coro *zombie;     // finished goroutine whose stack is freed by the next one to run
static int ngoroutines = 1;

int64_t go_mono_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    // split to avoid overflowing c * 1e9 (counters run at 10 MHz or more)
    int64_t s = c.QuadPart / freq.QuadPart, r = c.QuadPart % freq.QuadPart;
    return s * 1000000000 + r * 1000000000 / freq.QuadPart;
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

// Block the OS thread for d ns (nothing runnable until the next sleeper).
#if defined(_WIN32)
// Sleep() rounds up to the system timer tick (15.6 ms by default), far too
// coarse for the 1 ms socket polls and frame pacing: use a high-resolution
// waitable timer (Windows 10 1803+), else raise the timer resolution to 1 ms.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static void os_sleep_ns(int64_t d) {
    static HANDLE timer;
    static bool inited;
    if (!inited) {
        inited = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timeBeginPeriod(1);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(d / 100 > 0 ? d / 100 : 1);  // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) return;
    }
    Sleep((DWORD)((d + 999999) / 1000000));
}
#else
static void os_sleep_ns(int64_t d) {
    struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
    nanosleep(&ts, NULL);
}
#endif

static void runq_push(Coro *c) {
    c->next = NULL;
    if (runq_tail) runq_tail->next = c;
    else runq_head = c;
    runq_tail = c;
}

static Coro *runq_pop(void) {
    Coro *c = runq_head;
    if (c) {
        runq_head = c->next;
        if (!runq_head) runq_tail = NULL;
        c->next = NULL;
    }
    return c;
}

static void wake(Coro *c) {
    c->state = G_RUNNABLE;
    c->waiting = false;
    runq_push(c);
}

static void reap(void) {
    if (zombie && zombie != cur) {
#if defined(_WIN32)
        DeleteFiber(zombie->fiber);  // frees its stack (not the running fiber: zombie != cur)
        zombie->fiber = NULL;
        zombie->sp = NULL, zombie->fiber_top = 0;
#endif
        stack_free(zombie->stack, zombie->stack_size);  // NULL on Windows (the fiber owned it)
        zombie->stack = NULL;
        zombie = NULL;
    }
}

static void switch_to(Coro *next) {
    if (next == cur) {
        cur->state = G_RUNNING;
        return;
    }
    Coro *prev = cur;
    prev->panic_top = go_panic_top;
    go_panic_top = next->panic_top;
    cur = next;
    next->state = G_RUNNING;
#if defined(_WIN32)
    {
        // SwitchToFiber keeps the callee-saved registers in the fiber object,
        // where the collector cannot see them: spill them onto this stack
        // (setjmp) and scan a suspended goroutine from that buffer up to its
        // stack top, as ctx_switch's pushed registers are scanned elsewhere.
        // (Pointers in the frames above live in those frames or in the
        // registers saved here.)
        jmp_buf regs;
        setjmp(regs);
        prev->sp = (void *)&regs;
        SwitchToFiber(next->fiber);
        prev->sp = NULL;  // running again: the collector scans the live stack itself
    }
#elif defined(__x86_64__) || defined(__aarch64__)
    ctx_switch(&prev->sp, next->sp);
#else
    swapcontext(&prev->uc, &next->uc);
#endif
    reap();
}

// Runs the next runnable goroutine; the current one must already be queued,
// parked or dead.
static void schedule(void) {
    for (;;) {
        Coro *n = runq_pop();
        if (n) {
            switch_to(n);
            return;
        }
        if (!sleepers) {
            fprintf(stderr, "fatal error: all goroutines are asleep - deadlock!\n");
            abort();
        }
        // nothing runnable: wait for the earliest sleeper
        Coro **best = &sleepers;
        for (Coro **p = &sleepers; *p; p = &(*p)->next)
            if ((*p)->wake < (*best)->wake) best = p;
        Coro *s = *best;
        *best = s->next;
        int64_t d = s->wake - go_mono_ns();
        if (d > 0) os_sleep_ns(d);
        wake(s);
    }
}

static void wake_due_sleepers(void) {
    if (!sleepers) return;
    int64_t now = go_mono_ns();
    for (Coro **p = &sleepers; *p;) {
        if ((*p)->wake <= now) {
            Coro *s = *p;
            *p = s->next;
            wake(s);
        } else {
            p = &(*p)->next;
        }
    }
}

static void park(void) {
    cur->state = G_BLOCKED;
    cur->waiting = true;
    schedule();
}

void go_yield(void) {
    wake_due_sleepers();
    if (!runq_head) return;
    cur->state = G_RUNNABLE;
    runq_push(cur);
    schedule();
}

void go_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        go_yield();
        return;
    }
    cur->wake = go_mono_ns() + ns;
    cur->state = G_SLEEPING;
    cur->next = sleepers;
    sleepers = cur;
    schedule();
}

static void goroutine_exit(void) {
    Coro *me = cur;
    me->state = G_DEAD;
    me->fn = NULL;
    me->env = NULL;
    for (Coro **p = &all_coros; *p; p = &(*p)->allnext)
        if (*p == me) {
            *p = me->allnext;
            break;
        }
    ngoroutines--;
    zombie = me;  // its stack is freed by the next goroutine to run
    schedule();
    abort();  // unreachable
}

#if defined(_WIN32)
static void WINAPI fiber_entry(void *p) {
    (void)p;
    cur->fiber_top = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    reap();
    cur->fn(cur->env);
    goroutine_exit();
}
#else
static void coro_entry(void) {
    reap();
    cur->fn(cur->env);
    goroutine_exit();
}
#endif

static size_t stack_size(void) {
    static size_t sz;
    if (!sz) {
        sz = (sizeof(void *) == 8 ? 2048 : 512) * 1024;  // the char compiler recurses deeply
        const char *e = getenv("CVF_GOROUTINE_STACK_KB");
        if (e && atoi(e) > 0) sz = (size_t)atoi(e) * 1024;
    }
    return sz;
}

void go_spawn(void (*fn)(void *), void *env) {
    Coro *c = (Coro *)gc_alloc(sizeof(Coro));
    c->fn = fn;
    c->env = env;
    c->allnext = all_coros;
    all_coros = c;
    ngoroutines++;
#if defined(_WIN32)
    if (!main_coro.fiber) main_coro.fiber = ConvertThreadToFiber(NULL);
    c->stack_size = stack_size();
    // reserve stack_size, commit on demand (CreateFiber would commit it all)
    c->fiber = CreateFiberEx(64 * 1024, c->stack_size, 0, fiber_entry, NULL);
    if (!c->fiber) {
        fprintf(stderr, "fatal error: cannot allocate a goroutine stack\n");
        abort();
    }
#else
    c->stack_size = stack_size();
    c->stack = stack_alloc(c->stack_size);
    if (!c->stack) {
        fprintf(stderr, "fatal error: cannot allocate a goroutine stack\n");
        abort();
    }
    uintptr_t top = ((uintptr_t)c->stack + c->stack_size) & ~(uintptr_t)15;
#if defined(__x86_64__)
    void **sp = (void **)top;
    *--sp = NULL;                 // fake return address of coro_entry (keeps alignment)
    *--sp = (void *)coro_entry;   // ctx_switch returns here
    for (int i = 0; i < CTX_FRAME_WORDS; i++) *--sp = NULL;
    c->sp = sp;
#elif defined(__aarch64__)
    uint64_t *frame = (uint64_t *)(top - 176);
    memset(frame, 0, 176);
    frame[11] = (uint64_t)(uintptr_t)coro_entry;  // x30 (lr) at [sp, #88]
    c->sp = frame;
#else
    getcontext(&c->uc);
    c->uc.uc_stack.ss_sp = c->stack;
    c->uc.uc_stack.ss_size = c->stack_size;
    c->uc.uc_link = NULL;
    makecontext(&c->uc, coro_entry, 0);
#endif
#endif
    c->state = G_RUNNABLE;
    runq_push(c);
}

int go_num_goroutines(void) { return ngoroutines; }

// ---------------------------------------------------------------- collector hooks
static uintptr_t stack_top_of(Coro *c) {
#if defined(_WIN32)
    return c == &main_coro ? 0 : c->fiber_top;
#else
    return c->stack ? (((uintptr_t)c->stack + c->stack_size) & ~(uintptr_t)15) : 0;
#endif
}

static uintptr_t cur_stack_top(void) { return stack_top_of(cur); }  // 0: the main stack

static void scan_suspended(void (*scan)(uintptr_t, uintptr_t)) {
    // suspended goroutines: from their saved stack pointer to the top
    for (Coro *c = all_coros; c; c = c->allnext)
        if (c != cur && c != &main_coro && stack_top_of(c) && c->sp) scan((uintptr_t)c->sp, stack_top_of(c));
    if (cur != &main_coro && main_coro.sp) scan((uintptr_t)main_coro.sp, gc_main_stack_top());
}

__attribute__((constructor)) static void sched_install(void) {
    gc_stack_top_hook = cur_stack_top;
    gc_extra_roots_hook = scan_suspended;
}

// ---------------------------------------------------------------- channels
typedef struct Waiter {
    Coro *co;
    void *val;  // send: value to send; receive: destination (may be NULL)
    bool *ok;   // receive: set to true (value) or false (closed)
    int idx;    // select case index
    struct Waiter *next;
} Waiter;

struct GoChan {
    size_t elem;
    int cap, len, head;
    bool closed;
    uint8_t *buf;
    Waiter *recvq, *sendq;  // FIFO
};

GoChan *go_chan_make(size_t elem, int cap) {
    GoChan *c = (GoChan *)gc_alloc(sizeof(GoChan));
    c->elem = elem;
    c->cap = cap < 0 ? 0 : cap;
    if (c->cap) c->buf = (uint8_t *)gc_alloc(elem * (size_t)c->cap + 1);
    return c;
}

static void q_push(Waiter **q, Waiter *w) {
    w->next = NULL;
    while (*q) q = &(*q)->next;
    *q = w;
}

static void q_remove(Waiter **q, Waiter *w) {
    for (; *q; q = &(*q)->next)
        if (*q == w) {
            *q = w->next;
            return;
        }
}

// first waiter whose goroutine is still waiting (select waiters of a
// goroutine that another channel already woke are skipped)
static Waiter *q_take(Waiter **q) {
    while (*q) {
        Waiter *w = *q;
        *q = w->next;
        if (w->co->waiting) return w;
    }
    return NULL;
}

static uint8_t *slot(GoChan *c, int i) { return c->buf + (size_t)((c->head + i) % c->cap) * c->elem; }

_Noreturn static void chan_panic(const char *m) {
    fprintf(stderr, "panic: %s\n", m);
    abort();
}

// non-blocking attempts; true when done
static bool try_send(GoChan *c, const void *v) {
    if (c->closed) chan_panic("send on closed channel");
    Waiter *w = q_take(&c->recvq);
    if (w) {
        if (w->val) memcpy(w->val, v, c->elem);
        if (w->ok) *w->ok = true;
        w->co->chosen = w->idx;
        wake(w->co);
        return true;
    }
    if (c->len < c->cap) {
        memcpy(slot(c, c->len), v, c->elem);
        c->len++;
        return true;
    }
    return false;
}

static bool try_recv(GoChan *c, void *out, bool *ok) {
    if (c->len > 0) {
        if (out) memcpy(out, slot(c, 0), c->elem);
        memset(slot(c, 0), 0, c->elem);
        c->head = (c->head + 1) % c->cap;
        c->len--;
        Waiter *w = q_take(&c->sendq);  // a blocked sender refills the buffer
        if (w) {
            memcpy(slot(c, c->len), w->val, c->elem);
            c->len++;
            w->co->chosen = w->idx;
            wake(w->co);
        }
        if (ok) *ok = true;
        return true;
    }
    Waiter *w = q_take(&c->sendq);
    if (w) {
        if (out) memcpy(out, w->val, c->elem);
        w->co->chosen = w->idx;
        wake(w->co);
        if (ok) *ok = true;
        return true;
    }
    if (c->closed) {
        if (out) memset(out, 0, c->elem);
        if (ok) *ok = false;
        return true;
    }
    return false;
}

void go_chan_send(GoChan *c, const void *v) {
    if (!c) {
        park();  // nil channel: blocks forever
        abort();
    }
    if (try_send(c, v)) return;
    Waiter w = {cur, (void *)v, NULL, 0, NULL};
    q_push(&c->sendq, &w);
    cur->chosen = 0;
    park();
    if (cur->chosen < 0) chan_panic("send on closed channel");  // woken by close
}

bool go_chan_recv(GoChan *c, void *out) {
    bool ok = false;
    if (!c) {
        park();
        abort();
    }
    if (try_recv(c, out, &ok)) return ok;
    Waiter w = {cur, out, &ok, 0, NULL};
    q_push(&c->recvq, &w);
    park();
    return ok;
}

void go_chan_close(GoChan *c) {
    if (!c) chan_panic("close of nil channel");
    if (c->closed) chan_panic("close of closed channel");
    c->closed = true;
    Waiter *w;
    while ((w = q_take(&c->recvq))) {
        if (w->val) memset(w->val, 0, c->elem);
        if (w->ok) *w->ok = false;
        w->co->chosen = w->idx;
        wake(w->co);
    }
    while ((w = q_take(&c->sendq))) {
        w->co->chosen = -1;  // the sender panics
        wake(w->co);
    }
}

int go_chan_len(GoChan *c) { return c ? c->len : 0; }
int go_chan_cap(GoChan *c) { return c ? c->cap : 0; }

int go_select(GoSelCase *cs, int n, bool has_default) {
    // cases are tried in order (deterministic; Go picks randomly)
    for (int i = 0; i < n; i++) {
        GoChan *c = cs[i].c;
        if (!c) continue;
        if (cs[i].dir == 1 ? try_send(c, cs[i].val) : try_recv(c, cs[i].val, cs[i].ok)) return i;
    }
    if (has_default) return -1;
    Waiter ws[16];
    Waiter *wp = n <= 16 ? ws : (Waiter *)malloc(sizeof(Waiter) * (size_t)n);
    int live = 0;
    for (int i = 0; i < n; i++) {
        wp[i] = (Waiter){cur, cs[i].val, cs[i].ok, i, NULL};
        if (!cs[i].c) continue;
        q_push(cs[i].dir == 1 ? &cs[i].c->sendq : &cs[i].c->recvq, &wp[i]);
        live++;
    }
    if (!live) {
        park();  // select {} or only nil channels: blocks forever
        abort();
    }
    cur->chosen = -2;
    park();
    int k = cur->chosen;
    for (int i = 0; i < n; i++)
        if (cs[i].c) q_remove(cs[i].dir == 1 ? &cs[i].c->sendq : &cs[i].c->recvq, &wp[i]);
    if (wp != ws) free(wp);
    if (k == -1) chan_panic("send on closed channel");
    return k;
}
