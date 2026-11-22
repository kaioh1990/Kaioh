// cvf_vitasim: the game with the PS Vita renderer paths (cvfVitaGL) on a
// desktop OpenGL 2.1 compatibility context (Mesa under Xvfb works), to test
// what M2 changes without a Vita (VITA_PORT.md section 10):
//
//   the GLSL 1.x Vita shaders, clipping in the sprite shader instead of
//   scissor, client-side vertex arrays and sprite batching, 1024 atlases,
//   the 256² palette texture, drawing to the display buffer at 960x540, the
//   30/60 fps fight drawing modes, the Vita window/swap/perf hooks, and the
//   render thread (gles_thread.h).
//
// Usage (like cvf):  cvf_vitasim GAMEDIR [ikemen args...]
// Environment:
//   CVF_VITA_FPS=30|60          fight drawing mode (default 30)
//   CVF_VITA_RENDER_THREAD=1    GL calls on the render thread
//   CVF_SHOT_DIR=dir            save frames as PPM (960x544)
//   CVF_SHOT_EVERY=n            every n-th drawn frame (default 0: off)
//   CVF_SHOT_TICKS=n            in fights: the drawn frames of fight ticks
//                               t with t % n < 2, named by the tick (the same
//                               names in two runs show the same game state)
//   CVF_SIM_FRAMES=n            exit after n drawn frames
//   CVF_SIM_LOCKSTEP=n          test only: draw exactly the fight ticks t with
//                               t % n == 0 and every frame outside fights,
//                               whatever the timing (overrides the engine's
//                               skip decision), so that two runs draw the
//                               same frames
// It prints a "perf" line every 300 ticks (logic, drawing, swap, drawn frames).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int chdir(const char *path);

#include "core/sched.h"
#include "ik/ik_sim.h"
#include "platform/gles/gles_thread.h"

extern void (*cvf_vita_window_hook)(void);
extern void (*cvf_vita_swap_hook)(void);
extern void (*cvf_vita_perf_hook)(int ev, int arg);

static SDL_Window *win;
static SDL_GLContext ctx;
static int threaded, shot_every, shot_ticks, sim_frames, lockstep, drawn;
static const char *shot_dir;

static int host_init(void *ud) {
    (void)ud;
    return SDL_GL_MakeCurrent(win, ctx);
}
static void host_swap(void *ud) {
    (void)ud;
    SDL_GL_SwapWindow(win);
}

static void window_hook(void) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) fprintf(stderr, "vitasim: SDL video: %s\n", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    win = SDL_CreateWindow("cvf_vitasim (960x544)", 0, 0, 960, 544, SDL_WINDOW_OPENGL);
    if (!win || !(ctx = SDL_GL_CreateContext(win))) {
        fprintf(stderr, "vitasim: GL: %s\n", SDL_GetError());
        exit(2);
    }
    SDL_GL_SetSwapInterval(0);
    // vitaGL reads texture rows tightly packed (it has no UNPACK_ALIGNMENT,
    // so the renderer's Vita path never sets it); desktop GL's default of 4
    // would shear one-byte sprite rows of odd width
    void (*pixel_store)(unsigned, int) = (void (*)(unsigned, int))SDL_GL_GetProcAddress("glPixelStorei");
    if (pixel_store) pixel_store(0x0CF5, 1);
    if (threaded) {
        SDL_GL_MakeCurrent(win, NULL);
        GlesThreadHost h = {host_init, host_swap, NULL};
        if (gles_thread_start(&h) != 0) {
            fprintf(stderr, "vitasim: render thread failed\n");
            exit(2);
        }
    }
    fprintf(stderr, "vitasim: window 960x544, %s\n", threaded ? "render thread" : "engine thread");
}

static void save_shot(const char *name) {
    static uint8_t px[960 * 544 * 4];
    gles2_ReadPixels(0, 0, 960, 544, 0x1908, 0x1401, px);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n960 544\n255\n");
    for (int y = 543; y >= 0; y--)
        for (int x = 0; x < 960; x++) fwrite(px + (y * 960 + x) * 4, 1, 3, f);
    fclose(f);
}

static void swap_hook(void) {
    drawn++;
    char name[64];
    if (shot_dir && shot_every > 0 && drawn % shot_every == 0) {
        snprintf(name, sizeof name, "frame_%05d", drawn);
        save_shot(name);
    }
    if (shot_dir && shot_ticks > 0 && g_cvfVitaInMatch && g_sys.tickCount % shot_ticks < 2) {
        snprintf(name, sizeof name, "tick_%06d", (int)g_sys.tickCount);
        save_shot(name);
    }
    if (threaded) gles_thread_swap();
    else SDL_GL_SwapWindow(win);
    if (sim_frames > 0 && drawn >= sim_frames) {
        if (threaded) gles_thread_stop();
        fprintf(stderr, "vitasim: %d frames drawn, exiting\n", drawn);
        fflush(stdout), fflush(stderr);
        _Exit(0);
    }
}

static int64_t t_begin, t_last;
static uint64_t sum[3], frame_sum, frame_max;
static int ticks, ticks_drawn, swapped;
static void perf_hook(int ev, int arg) {
    int64_t now = go_mono_ns();
    if (ev == 1) {
        t_begin = now;
        return;
    }
    if (ev == 2) {
        if (arg >= 0 && arg < 3 && t_begin) sum[arg] += (uint64_t)(now - t_begin) / 1000;
        if (arg == 2) swapped = 1;
        t_begin = 0;
        return;
    }
    uint64_t fr = t_last ? (uint64_t)(now - t_last) / 1000 : 0;
    t_last = now;
    frame_sum += fr;
    if (fr > frame_max) frame_max = fr;
    ticks++, ticks_drawn += swapped, swapped = 0;
    if (lockstep > 0) g_sys.frameSkip = g_cvfVitaInMatch && (g_sys.tickCount + 1) % lockstep != 0;
    if (ticks == 300) {
        fprintf(stderr, "perf: ticks 300 drawn %d fight %d | logic %.2f ms/tick | draw %.2f swap %.2f ms/drawn | frame avg %.2f max %.2f ms\n",
                ticks_drawn, (int)g_cvfVitaInMatch, sum[0] / 300.0 / 1000, sum[1] / 1000.0 / (ticks_drawn ? ticks_drawn : 1),
                sum[2] / 1000.0 / (ticks_drawn ? ticks_drawn : 1), frame_sum / 300.0 / 1000, frame_max / 1000.0);
        if (threaded) fprintf(stderr, "perf: render thread %.2f ms/drawn\n", gles_thread_take_render_us() / 1000.0 / (ticks_drawn ? ticks_drawn : 1));
        ticks = ticks_drawn = 0, sum[0] = sum[1] = sum[2] = frame_sum = frame_max = 0;
    }
}

int main(int argc, char **argv) {
    gc_init(&argc);
    if (argc < 2 || chdir(argv[1]) != 0) {
        fprintf(stderr, "usage: cvf_vitasim GAMEDIR [ikemen args...]\n");
        return 2;
    }
    threaded = getenv("CVF_VITA_RENDER_THREAD") && atoi(getenv("CVF_VITA_RENDER_THREAD"));
    shot_dir = getenv("CVF_SHOT_DIR");
    shot_every = getenv("CVF_SHOT_EVERY") ? atoi(getenv("CVF_SHOT_EVERY")) : 0;
    shot_ticks = getenv("CVF_SHOT_TICKS") ? atoi(getenv("CVF_SHOT_TICKS")) : 0;
    sim_frames = getenv("CVF_SIM_FRAMES") ? atoi(getenv("CVF_SIM_FRAMES")) : 0;
    lockstep = getenv("CVF_SIM_LOCKSTEP") ? atoi(getenv("CVF_SIM_LOCKSTEP")) : 0;
    ik_init_globals();
    g_cvfVitaGL = true;
    g_cvfVitaFPS = getenv("CVF_VITA_FPS") && atoi(getenv("CVF_VITA_FPS")) == 60 ? 60 : 30;
    cvf_vita_window_hook = window_hook;
    cvf_vita_swap_hook = swap_hook;
    cvf_vita_perf_hook = perf_hook;
    Slice_GoString args = {(GoString *)gc_alloc(sizeof(GoString) * (size_t)argc), 0, argc};
    args.p[args.len++] = GOSTR("cvf");
    for (int i = 2; i < argc; i++) args.p[args.len++] = go_cstr(argv[i]);
    os_Args = args;
    fn_main();
    if (threaded) gles_thread_stop();
    return 0;
}
