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

// ---------------------------------------------------------------- main stack report
// The main goroutine runs on the main thread's stack (1 MB on iOS, together
// with UIKit's frames). The part below the current frame is filled with a
// pattern at startup; the deepest overwritten word is the high-water mark.
#define STACK_MAGIC 0xC5F0C5F0C5F0C5F0ull
static uintptr_t stk_lo, stk_hi, stk_peak_logged;
static unsigned stk_frames;

__attribute__((noinline)) static void stack_paint(void) {
    pthread_t self = pthread_self();
    stk_hi = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    stk_lo = stk_hi - size + 32 * 1024;  // keep clear of the guard page
    uintptr_t top = (sp - 4096) & ~(uintptr_t)7;
    for (uintptr_t a = stk_lo; a < top; a += 8) *(volatile uint64_t *)a = STACK_MAGIC;
    fprintf(stderr, "cvf: main thread stack %zu KB, %zu KB in use at engine start\n", size / 1024,
            (size_t)(stk_hi - sp) / 1024);
}

static size_t stack_peak(void) {
    uintptr_t a = stk_lo;
    while (a < stk_hi && *(volatile uint64_t *)a == STACK_MAGIC) a += 8;
    return (size_t)(stk_hi - a);
}

void cvf_ios_frame_tick(void) {
    if (!stk_hi || ++stk_frames % 120) return;
    size_t peak = stack_peak();
    if (peak > stk_peak_logged + 16 * 1024) {
        stk_peak_logged = peak;
        fprintf(stderr, "cvf: main thread stack peak %zu KB (of %zu KB)\n", peak / 1024,
                (size_t)(stk_hi - stk_lo + 32 * 1024) / 1024);
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
    stack_paint();
}

// ---------------------------------------------------------------- background
static _Atomic int in_background;

static int SDLCALL app_watch(void *user, SDL_Event *e) {
    (void)user;
    switch (e->type) {
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
        atomic_store(&in_background, 1);
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
