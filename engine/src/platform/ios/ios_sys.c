// iOS platform helpers (see ios_sys.h).
#include "platform/ios/ios_sys.h"

#include <SDL.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int cvf_ios_on_main_thread(void) { return pthread_main_np() != 0; }

int cvf_ios_chdir(const char *dir) { return chdir(dir); }

static void mkdirs(const char *path) {
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

// save/cvf_env.txt: KEY=VALUE lines (tuning knobs for a sideloaded build,
// which has no way to set environment variables).
static void read_env_file(const char *assets) {
    char path[4096];
    snprintf(path, sizeof path, "%s/save/cvf_env.txt", assets);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "cvf: env %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

// ---------------------------------------------------------------- engine stack
// The main goroutine needs about 1 MB of stack (measured in the Simulator,
// loading the title screen), as much as the whole iOS main thread stack,
// which UIKit's frames share. The engine therefore runs on a stack of its
// own (IOS_PORT.md 3.3, plan B): 8 MB, as the macOS main thread, mapped
// zero-fill-on-demand with a PROT_NONE guard page below it. Frames of UIKit
// callbacks during the event pump (touches) run on it too.
#define ENGINE_STACK (8u << 20)
static uintptr_t stk_lo, stk_hi, stk_peak_logged;
static unsigned stk_frames;

// cvf_call_on_stack(top, fn, arg): calls fn(arg) with sp = top, then
// returns on the original stack. x19 (callee-saved) holds the old sp.
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _cvf_call_on_stack\n"
    ".private_extern _cvf_call_on_stack\n"
    "_cvf_call_on_stack:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  stp x19, x20, [sp, #-16]!\n"
    "  mov x19, sp\n"
    "  mov sp, x0\n"
    "  mov x0, x2\n"
    "  blr x1\n"
    "  mov sp, x19\n"
    "  ldp x19, x20, [sp], #16\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n");
void cvf_call_on_stack(void *top, void (*fn)(void *), void *arg);

int cvf_ios_run_on_engine_stack(void (*fn)(void *), void *arg) {
    // 64 KB guard below the stack (at least one page): a large frame cannot
    // step over it into the next mapping (see core/sched.c stack_alloc)
    size_t pg = (size_t)getpagesize(), guard = (65536 + pg - 1) / pg * pg;
    uint8_t *p = (uint8_t *)mmap(NULL, ENGINE_STACK + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "cvf: cannot map the engine stack, using the main thread's\n");
        fn(arg);
        return 0;
    }
    mprotect(p, guard, PROT_NONE);
    stk_lo = (uintptr_t)p + guard;
    stk_hi = stk_lo + ENGINE_STACK;
    cvf_call_on_stack((void *)stk_hi, fn, arg);
    return 1;
}
void *cvf_ios_engine_stack_top(void) { return (void *)stk_hi; }

// High-water mark: the lowest word written (fresh pages read as zero).
static size_t stack_peak(void) {
    uintptr_t a = stk_lo;
    while (a < stk_hi && *(volatile uint64_t *)a == 0) a += 4096;  // untouched pages
    if (a > stk_lo) a -= 4096;
    while (a < stk_hi && *(volatile uint64_t *)a == 0) a += 8;
    return (size_t)(stk_hi - a);
}

void cvf_ios_frame_tick(void) {
    if (!stk_hi || ++stk_frames % 120) return;
    size_t peak = stack_peak();
    if (peak > stk_peak_logged + 16 * 1024) {
        stk_peak_logged = peak;
        fprintf(stderr, "cvf: engine stack peak %zu KB (of %u KB)\n", peak / 1024, ENGINE_STACK / 1024);
        fflush(stderr);
    }
}

void cvf_ios_prepare(const char *assets) {
    if (assets && *assets) {
        char logs[4096];
        snprintf(logs, sizeof logs, "%s/save/logs", assets);
        mkdirs(logs);
        // readable in the Files app; not when started from Xcode with a console
        if (!getenv("CVF_LOG_CONSOLE")) {
            char path[4200];
            snprintf(path, sizeof path, "%s/cvf.log", logs);
            FILE *f = fopen(path, "w");  // a fresh log per run; both streams append to it
            if (f) fclose(f);
            if (freopen(path, "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
            if (freopen(path, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        }
        read_env_file(assets);
    }
    pthread_t self = pthread_self();
    fprintf(stderr, "cvf: main thread stack %zu KB; engine stack %u KB\n", pthread_get_stacksize_np(self) / 1024,
            stk_hi ? ENGINE_STACK / 1024 : 0);
}

// ---------------------------------------------------------------- background
static _Atomic int in_background, low_memory;
int cvf_ios_take_low_memory(void) { return atomic_exchange(&low_memory, 0); }

static int SDLCALL app_watch(void *user, SDL_Event *e) {
    (void)user;
    switch (e->type) {
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        atomic_store(&in_background, 1);
        break;
    case SDL_APP_LOWMEMORY:  // applicationDidReceiveMemoryWarning
        atomic_store(&low_memory, 1);
        break;
    case SDL_APP_WILLENTERFOREGROUND:
    case SDL_APP_DIDENTERFOREGROUND:
        atomic_store(&in_background, 0);
        break;
    }
    return 1;
}

void cvf_ios_install_app_watch(void) { SDL_AddEventWatch(app_watch, NULL); }

// GL calls from a backgrounded app get it killed: stall the frame loop
// (no swap, no drawing) until the app is back in the foreground.
void cvf_ios_wait_foreground(void) {
    if (!atomic_load(&in_background)) return;
    fprintf(stderr, "cvf: in background, rendering paused\n");
    while (atomic_load(&in_background)) {
        SDL_PumpEvents();  // runs the UIKit run loop: the foreground notification arrives here
        SDL_Delay(50);
    }
    fprintf(stderr, "cvf: back in foreground\n");
}
