// SDL_main of the iOS app: the native engine's entry point, called by the
// app's ViewController on the main thread (from a run-loop timer) with
//   --assets <game root> --screen <W>x<H>
// as the Go engine's SDL_main export was.
//
// Headless reference run (IOS_PORT.md M3), for the Simulator:
//   SIMCTL_CHILD_CVF_HEADLESS_DIR=<game dir> SIMCTL_CHILD_CVF_HEADLESS_ARGS="-p1 ..."
//   xcrun simctl launch --console <device> com.kaioh1990.cvfonline
// runs cvfHeadlessMain in that directory (statedump.txt is written there),
// prints "winner: N" and exits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ik/ik_sim.h"
#include "platform/ios/include/cvf_host.h"
#include "platform/ios/ios_sys.h"

extern void (*cvf_safepoint_hook)(void);  // ik_native.c

static Slice_GoString go_args(int n) {
    return (Slice_GoString){(GoString *)gc_alloc(sizeof(GoString) * (size_t)(n > 0 ? n : 1)), 0, n};
}

static int run_headless(const char *dir, const char *argstr) {
    fprintf(stderr, "cvf: headless run in %s: %s\n", dir, argstr ? argstr : "");
    if (cvf_ios_chdir(dir) != 0) {
        perror(dir);
        return 2;
    }
    int fails = gc_selftest();
    fprintf(stderr, "cvf: gc selftest %s\n", fails ? "FAILED" : "ok");
    // split the argument string at spaces (no quoting: names have no spaces)
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
    fflush(stdout);
    exit(w < 0 || fails ? 1 : 0);
}

typedef struct MainArgs {
    int argc;
    char **argv;
} MainArgs;

// Runs on the engine stack (cvf_ios_run_on_engine_stack).
static void engine_main(void *p) {
    MainArgs *m = (MainArgs *)p;
    int argc = m->argc;
    char **argv = m->argv;
    gc_init(&argc);
    gc_set_main_stack(cvf_ios_engine_stack_top());  // scan this stack, not the main thread's
    const char *hdir = getenv("CVF_HEADLESS_DIR");
    if (hdir && *hdir) run_headless(hdir, getenv("CVF_HEADLESS_ARGS"));

    const char *assets = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--assets")) assets = argv[i + 1];
    cvf_ios_prepare(assets);
    fprintf(stderr, "cvf: native engine starting (assets %s)\n", assets ? assets : "(none)");

    ik_init_globals();
    // os.Args = ["ikemen-go"]; the app's own arguments go to cvfIOSMain
    Slice_GoString osargs = go_args(1);
    osargs.p[osargs.len++] = GOSTR("ikemen-go");
    os_Args = osargs;
    Slice_GoString args = go_args(argc);
    for (int i = 0; i < argc; i++) args.p[args.len++] = go_cstr(gc_strdup(argv[i] ? argv[i] : ""));

    cvf_ios_install_app_watch();
    cvf_safepoint_hook = cvf_host_drain;
    cvf_host_set_running(1);
    fn_cvfIOSMain(args);
    cvf_host_set_running(0);
    cvf_safepoint_hook = NULL;
    fprintf(stderr, "cvf: engine returned\n");
}

int SDL_main(int argc, char **argv) {
    MainArgs m = {argc, argv};
    cvf_ios_run_on_engine_stack(engine_main, &m);
    return 0;
}
