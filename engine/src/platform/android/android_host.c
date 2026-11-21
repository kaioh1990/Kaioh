// Android: the engine side of the platform layer (android_sys.h). Runs on
// the engine thread only: sets up the collector and calls the translated
// entry point, and once per frame (cvfGcSafepoint) applies what the app's UI
// thread left in the mailbox and publishes what the UI thread reads.
//
// Test modes (save/cvf_env.txt, as on iOS):
//   CVF_HEADLESS_DIR=<dir> CVF_HEADLESS_ARGS="-p1 ..."  headless reference
//       fight in <dir> (statedump.txt there), "winner: N" in the log, exit
//   CVF_HTTP_TEST=<lobby base URL>  lobby round trip through net/http
//       (HttpURLConnection), result in the log, exit
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ik/ik_sim.h"
#include "platform/android/android_sys.h"

extern void (*cvf_safepoint_hook)(bool fighting);  // ik_native.c

static Slice_GoString go_args(int n) {
    return (Slice_GoString){(GoString *)gc_alloc(sizeof(GoString) * (size_t)(n > 0 ? n : 1)), 0, n};
}

static void run_headless(const char *dir, const char *argstr) {
    fprintf(stderr, "cvf: headless run in %s: %s\n", dir, argstr ? argstr : "");
    if (cvf_android_chdir(dir) != 0) {
        perror(dir);
        exit(2);
    }
    int fails = gc_selftest();
    fprintf(stderr, "cvf: gc selftest %s\n", fails ? "FAILED" : "ok");
    char *buf = strdup(argstr ? argstr : "");
    int n = 0;
    char *words[64];
    for (char *t = strtok(buf, " "); t && n < 64; t = strtok(NULL, " ")) words[n++] = t;
    ik_init_globals();
    Slice_GoString args = go_args(n);
    for (int i = 0; i < n; i++) args.p[args.len++] = go_cstr(gc_strdup(words[i]));
    free(buf);
    i32 w = fn_cvfHeadlessMain(args);
    printf("winner: %d\n", w);
    fflush(NULL);
    exit(w < 0 || fails ? 1 : 0);
}

// Once per frame on the engine thread (cvfGcSafepoint hook).
static void cvf_host_drain(bool fighting) {
    (void)fighting;
    CvfAndroidMailbox *m = &cvf_android_mbox;
    int tm = atomic_exchange(&m->touch_menus, -1);
    if (tm >= 0) fn_cvfHostSetTouchMenus(tm != 0);
    if (atomic_exchange_explicit(&m->tap_pending, 0, memory_order_acquire))
        fn_cvfHostTap(atomic_load(&m->tap_x), atomic_load(&m->tap_y));
    atomic_store(&m->in_match, fn_cvfHostInMatch() ? 1 : 0);
    atomic_store(&m->tap_listening, fn_cvfHostTapListening() ? 1 : 0);
    cvf_android_frame_tick();
}

void cvf_android_engine_main(int argc, char **argv) {
    gc_init(&argc);  // this thread's stack is the main goroutine's
    const char *hdir = getenv("CVF_HEADLESS_DIR");
    if (hdir && *hdir) run_headless(hdir, getenv("CVF_HEADLESS_ARGS"));
    const char *http = getenv("CVF_HTTP_TEST");
    if (http && *http) {
        ik_init_globals();
        GoString r = fn_cvfHTTPSelfTest(go_cstr(gc_strdup(http)));
        printf("cvf: http test %s\n%.*s", http, r.len, r.p);
        fflush(NULL);
        exit(0);
    }

    ik_init_globals();
    Slice_GoString osargs = go_args(1);
    osargs.p[osargs.len++] = GOSTR("ikemen-go");
    os_Args = osargs;
    Slice_GoString args = go_args(argc);
    for (int i = 0; i < argc; i++) args.p[args.len++] = go_cstr(gc_strdup(argv[i] ? argv[i] : ""));

    // Ikemen's Android branch initialises the joystick and game controller
    // subsystems with sdl.InitSubSystem, which the translation lacks (a
    // placeholder, GAPS.md): do it here, before the engine's SDL_Init (SDL
    // counts subsystem references). Controllers then come through SDL's
    // Android joystick driver (SDLControllerManager, HIDAPI).
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0)
        fprintf(stderr, "cvf: SDL joystick init: %s\n", SDL_GetError());

    cvf_safepoint_hook = cvf_host_drain;
    atomic_store(&cvf_android_mbox.engine_up, 1);
    fn_cvfIOSMain(args);  // --assets <game> --screen WxH -> realMain (Android branch)
    atomic_store(&cvf_android_mbox.engine_up, 0);
    cvf_safepoint_hook = NULL;
}
