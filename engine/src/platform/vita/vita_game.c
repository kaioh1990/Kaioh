// PS Vita: the game (VITA_PORT.md milestone M2). Runs the translated Ikemen
// GO main() - Lua menus, character select, fights - with cvfVitaGL set, so
// the GLES renderer takes its Vita paths (GLSL 1.x shaders, vitaGL
// workarounds) and the window, swap and frame timing come here:
//
//   window  SDL (audio, controllers, events; video only for front touch,
//           which SDL polls only with a window) and vitaGL's 960x544 display
//   swap    vglSwapBuffers, or the render thread (CVF_VITA_RENDER_THREAD=1,
//           gles_thread.c)
//   perf    per-tick logic / drawing / swap times and memory, written to
//           ux0:data/CvFNative/frames.csv and perf.log (format in
//           docs/VITA_PORT.md section 10)
//
// Settings come from ux0:data/CvFNative/cvf_env.txt (vita_main.c loads it
// into the environment before this runs).
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int chdir(const char *path);  // <unistd.h> clashes with generated names

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>

#include "core/gc.h"
#include "core/sched.h"
#include "ik/ik_sim.h"
#include "platform/vita/vita_game.h"

#define DIR "ux0:data/CvFNative"

// vitaGL (vitaGL.h pulls in GL headers whose names clash with ik_sim.h)
unsigned char vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, int msaa);
void vglSwapBuffers(unsigned char has_commondialog);
void vglWaitVblankStart(unsigned char enable);
size_t vglMemFree(int type);
enum { VGL_MEM_VRAM, VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET, VGL_MEM_EXTERNAL, VGL_MEM_ALL };

extern void (*cvf_vita_window_hook)(void);
extern void (*cvf_vita_swap_hook)(void);
extern void (*cvf_vita_perf_hook)(int ev, int arg);

static int env_int(const char *k, int def) {
    const char *v = getenv(k);
    return v && *v ? atoi(v) : def;
}

// ---------------------------------------------------------------- settings
static struct {
    int fps;            // CVF_VITA_FPS: 30 (default) or 60 drawn frames per second in fights
    int render_thread;  // CVF_VITA_RENDER_THREAD: vitaGL submission on a second core
    int frame_log;      // CVF_FRAME_LOG: frames.csv (default 1)
    int vgl_threshold;  // CVF_VGL_RAM_THRESHOLD_MB: main RAM vitaGL leaves free (16)
    int vsync;          // CVF_VITA_VSYNC: vglWaitVblankStart (1)
} cfg;

// ---------------------------------------------------------------- frame timing
// Section times of the current tick (ev 1 begin, ev 2 end of section 0 logic /
// 1 drawing / 2 flush+swap); ev 3 closes the tick. Times are CPU-side wall
// times on the engine thread (the Vita does not preempt it for other work of
// the game); with the render thread, vitaGL's own work is measured there
// (render_us) and section 2 is only the hand-over.
typedef struct {
    uint32_t tick;
    uint32_t t_ms;       // since the game started
    uint32_t logic_us, draw_us, swap_us, frame_us, render_us;
    uint8_t drawn, fight;
} FrameRec;

static int64_t t_start, t_begin, t_last_tick;
static uint32_t sec_us[3];
static bool swapped;
static uint32_t ticks;
static FILE *csv, *perf;
static long csv_bytes;

enum { WIN = 300 };  // summary every 300 ticks (5 s)
static FrameRec win[WIN];
static int win_n;

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static uint32_t pct(uint32_t *v, int n, int p) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_u32);
    int i = (n * p + 99) / 100 - 1;
    return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

static void perf_line(const char *fmt, ...) {
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fputs(b, stderr);
    if (perf) fputs(b, perf), fflush(perf);
}

void vita_game_mem_line(const char *tag) {
    GcStats g = gc_stats();
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    sceKernelGetFreeMemorySize(&fm);
    struct mallinfo mi = mallinfo();
    perf_line("mem %s: gc live %.1f heap %.1f hwm %.1f MB, %llu collections, max pause %.0f ms; "
              "newlib in use %.1f MB; kernel free user %.1f cdram %.1f phycont %.1f MB; "
              "vitaGL free ram %.1f vram %.1f MB\n",
              tag, g.live_bytes / 1048576.0, g.heap_bytes / 1048576.0, g.hwm_bytes / 1048576.0,
              (unsigned long long)g.collections, g.max_pause_ms, mi.uordblks / 1048576.0,
              fm.size_user / 1048576.0, fm.size_cdram / 1048576.0, fm.size_phycont / 1048576.0,
              vglMemFree(VGL_MEM_RAM) / 1048576.0, vglMemFree(VGL_MEM_VRAM) / 1048576.0);
}

static void summary(void) {
    static uint32_t a[WIN];
    int n = win_n, drawn = 0, fight = 0, late = 0;
    uint64_t sl = 0, sd = 0, ss = 0, sf = 0, sr = 0;
    uint32_t ml = 0, md = 0, ms = 0, mf = 0, mr = 0;
    for (int i = 0; i < n; i++) {
        FrameRec *r = &win[i];
        sl += r->logic_us, sf += r->frame_us, sr += r->render_us;
        if (r->logic_us > ml) ml = r->logic_us;
        if (r->frame_us > mf) mf = r->frame_us;
        if (r->render_us > mr) mr = r->render_us;
        if (r->frame_us > 17500) late++;
        fight += r->fight;
        if (r->drawn) {
            drawn++;
            sd += r->draw_us, ss += r->swap_us;
            if (r->draw_us > md) md = r->draw_us;
            if (r->swap_us > ms) ms = r->swap_us;
        }
    }
    for (int i = 0; i < n; i++) a[i] = win[i].logic_us;
    uint32_t l95 = pct(a, n, 95);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (win[i].drawn) a[k++] = win[i].draw_us + win[i].swap_us;
    uint32_t d95 = pct(a, k, 95);
    double secs = (go_mono_ns() - t_start) / 1e9;
    double span = 0;
    for (int i = 0; i < n; i++) span += win[i].frame_us;
    double dfps = span > 0 ? drawn / (span / 1e6) : 0;
    double nd = drawn ? drawn : 1;
    perf_line("perf t=%.0fs ticks %d (fight %d) drawn %d = %.1f fps, mode %d%s | logic avg %.2f p95 %.2f max %.2f ms/tick"
              " | draw avg %.2f max %.2f ms, swap avg %.2f max %.2f ms, draw+swap p95 %.2f ms/drawn frame"
              " | frame avg %.2f max %.2f ms, %d over 17.5 ms\n",
              secs, n, fight, drawn, dfps, cfg.fps, cfg.render_thread ? " render-thread" : "",
              sl / 1000.0 / (n ? n : 1), l95 / 1000.0, ml / 1000.0,
              sd / 1000.0 / nd, md / 1000.0, ss / 1000.0 / nd, ms / 1000.0, d95 / 1000.0,
              sf / 1000.0 / (n ? n : 1), mf / 1000.0, late);
    if (cfg.render_thread)
        perf_line("perf render thread: vitaGL avg %.2f max %.2f ms/drawn frame\n", sr / 1000.0 / nd, mr / 1000.0);
    vita_game_mem_line("5s");
    if (csv) fflush(csv);
    win_n = 0;
}

static void perf_hook(int ev, int arg) {
    int64_t now = go_mono_ns();
    if (ev == 1) {
        t_begin = now;
        return;
    }
    if (ev == 2) {
        if (arg >= 0 && arg < 3 && t_begin) sec_us[arg] += (uint32_t)((now - t_begin) / 1000);
        if (arg == 2) swapped = true;
        t_begin = 0;
        return;
    }
    // ev 3: end of the tick
    FrameRec r;
    memset(&r, 0, sizeof r);
    r.tick = ticks++;
    r.t_ms = (uint32_t)((now - t_start) / 1000000);
    r.logic_us = sec_us[0], r.draw_us = sec_us[1], r.swap_us = sec_us[2];
    r.frame_us = t_last_tick ? (uint32_t)((now - t_last_tick) / 1000) : 0;
    r.render_us = cfg.render_thread ? gles_thread_take_render_us() : 0;
    r.drawn = swapped;  // flushed and swapped this tick
    swapped = false;
    r.fight = g_cvfVitaInMatch;
    (void)arg;
    sec_us[0] = sec_us[1] = sec_us[2] = 0;
    t_last_tick = now;
    if (csv && csv_bytes < (32L << 20)) {
        csv_bytes += fprintf(csv, "%u,%u,%u,%u,%u,%u,%u,%u,%u\n", r.tick, r.t_ms, r.logic_us, r.draw_us, r.swap_us,
                             r.render_us, r.frame_us, r.drawn, r.fight);
    }
    win[win_n++] = r;
    if (win_n == WIN) summary();
}

// ---------------------------------------------------------------- display
static void touch_tap(float x, float y) {
    // menus (cvfTouchTap in the Lua scripts): x, y in 0..1 of the screen
    fn_cvfHostTap(x, y);
}

static int SDLCALL event_watch(void *ud, SDL_Event *e) {
    (void)ud;
    // front panel only (touch device 1; the rear panel is device 2)
    if (e->type == SDL_FINGERDOWN && e->tfinger.touchId == 1) touch_tap(e->tfinger.x, e->tfinger.y);
    return 1;
}

// render thread (gles_thread.h): vitaGL lives on that thread
static int host_init(void *ud) {
    (void)ud;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    int64_t t0 = go_mono_ns();
    int r = vglInitExtended(0, 960, 544, cfg.vgl_threshold << 20, 0);
    vglWaitVblankStart(cfg.vsync ? 1 : 0);
    fprintf(stderr, "vita: render thread: vglInitExtended -> %d (%.0f ms)\n", r, (go_mono_ns() - t0) / 1e6);
    return r ? 0 : -1;
}
static void host_swap(void *ud) {
    (void)ud;
    vglSwapBuffers(0);
}

static void window_hook(void) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) fprintf(stderr, "vita: SDL video: %s\n", SDL_GetError());
    // SDL polls the touch panels only while a window exists (SDL_vitatouch.c);
    // a window without SDL_WINDOW_OPENGL creates no GXM or display state
    if (!SDL_CreateWindow("CvF", 0, 0, 960, 544, 0)) fprintf(stderr, "vita: SDL window: %s\n", SDL_GetError());
    SDL_AddEventWatch(event_watch, NULL);
    vita_game_mem_line("before vitaGL");
    int64_t t0 = go_mono_ns();
    if (cfg.render_thread) {
        // pin the engine thread to core 0 and the render thread to core 1
        // (game threads may use cores 0-2)
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        GlesThreadHost h = {host_init, host_swap, NULL};
        if (gles_thread_start(&h) != 0) {
            fprintf(stderr, "vita: render thread failed to start, drawing on the engine thread\n");
            cfg.render_thread = 0;
        }
    }
    if (!cfg.render_thread) {
        int r = vglInitExtended(0, 960, 544, cfg.vgl_threshold << 20, 0);
        vglWaitVblankStart(cfg.vsync ? 1 : 0);
        fprintf(stderr, "vita: vglInitExtended -> %d (%.0f ms)\n", r, (go_mono_ns() - t0) / 1e6);
    }
    vita_game_mem_line("after vitaGL");
}

static void swap_hook(void) {
    if (cfg.render_thread) gles_thread_swap();
    else vglSwapBuffers(0);
}

// ---------------------------------------------------------------- main
// cvf_args.txt: optional extra Ikemen arguments, one line (e.g. a quick
// match: -p1 Alucard -p2 Richter -s stages/ParanoiaBG.def)
static int read_args(char *buf, size_t n, char **argv, int max) {
    int argc = 0;
    FILE *f = fopen(DIR "/cvf_args.txt", "r");
    if (!f) return 0;
    if (fgets(buf, (int)n, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        for (char *t = strtok(buf, " \t"); t && argc < max; t = strtok(NULL, " \t")) argv[argc++] = t;
    }
    fclose(f);
    return argc;
}

int vita_game_run(const char *gamedir) {
    int stack_base;
    gc_init(&stack_base);
    cfg.fps = env_int("CVF_VITA_FPS", 30) == 60 ? 60 : 30;
    cfg.render_thread = env_int("CVF_VITA_RENDER_THREAD", 0) != 0;
    cfg.frame_log = env_int("CVF_FRAME_LOG", 1) != 0;
    cfg.vgl_threshold = env_int("CVF_VGL_RAM_THRESHOLD_MB", 16);
    cfg.vsync = env_int("CVF_VITA_VSYNC", 1) != 0;
    perf = fopen(DIR "/perf.log", "w");
    if (cfg.frame_log) {
        csv = fopen(DIR "/frames.csv", "w");
        if (csv) {
            setvbuf(csv, NULL, _IOFBF, 64 * 1024);
            fputs("tick,t_ms,logic_us,draw_us,swap_us,render_us,frame_us,drawn,fight\n", csv);
        }
    }
    perf_line("CvF native M2: ARM %d MHz, bus %d, GPU %d; draw mode %d fps in fights%s; game %s\n",
              scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
              cfg.fps, cfg.render_thread ? ", render thread" : "", gamedir);
    if (chdir(gamedir) != 0) {
        perf_line("cannot enter %s (errno %d)\n", gamedir, errno);
        return 2;
    }
    t_start = go_mono_ns();
    ik_init_globals();
    g_cvfVitaGL = true;
    g_cvfVitaFPS = cfg.fps;
    g_vitaFBOForced = env_int("CVF_VITA_FBO", 0) != 0;
    cvf_vita_window_hook = window_hook;
    cvf_vita_swap_hook = swap_hook;
    cvf_vita_perf_hook = perf_hook;

    static char argbuf[1024];
    char *extra[48];
    int ne = read_args(argbuf, sizeof argbuf, extra, 48);
    Slice_GoString args = {(GoString *)gc_alloc(sizeof(GoString) * (size_t)(ne + 1)), 0, ne + 1};
    args.p[args.len++] = GOSTR("cvf");
    for (int i = 0; i < ne; i++) args.p[args.len++] = go_cstr(extra[i]);
    os_Args = args;
    fn_main();
    if (win_n) summary();
    vita_game_mem_line("exit");
    if (cfg.render_thread) gles_thread_stop();
    if (csv) fclose(csv), csv = NULL;
    if (perf) fclose(perf), perf = NULL;
    return 0;
}
