// cvf_headless: runs a quick match without window or audio.
//   cvf_headless <gamedir> -p1 A -p2 B [-p1.ai N -p2.ai N] -s stage [-rounds N]
// With <gamedir>/vita_statedump.txt present, writes the per-tick state dump
// (statedump.txt) used to verify the engine against Ikemen GO.
// CVF_FRAME_STATS=1: frame times during the fight (between safepoints while
// a round is in progress), printed at the end; with CVF_GC_TRACE=1 the
// collector's summary too. (The PS Vita build calls this main as
// cvf_headless_main from src/platform/vita/vita_main.c.)
#include <stdio.h>
int chdir(const char *path);  // <unistd.h> clashes with generated names

#include "ik/ik_sim.h"

static struct {
    i64 last, total, max, load_end;
    size_t alloc0, alloc1;  // collector's total allocation at the first / last fight frame
    i64 hist[8];  // < 1, 2, 4, 8, 16, 33, 66 ms, more
    int n;
} fs;

static void frame_hook(bool fighting) {
    i64 now = go_mono_ns();
    if (fighting && fs.last) {
        i64 d = now - fs.last;
        fs.total += d, fs.n++;
        if (d > fs.max) fs.max = d;
        static const i64 lim[7] = {1000000, 2000000, 4000000, 8000000, 16666667, 33333333, 66666667};
        int b = 0;
        while (b < 7 && d >= lim[b]) b++;
        fs.hist[b]++;
    }
    if (fighting && !fs.load_end) fs.load_end = now, fs.alloc0 = gc_stats().total_alloc;
    if (fighting) fs.alloc1 = gc_stats().total_alloc;
    fs.last = fighting ? now : 0;
}

int main(int argc, char **argv) {
    gc_init(&argc);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <gamedir> [ikemen args...]\n", argv[0]);
        return 2;
    }
    if (chdir(argv[1]) != 0) {
        perror(argv[1]);
        return 2;
    }
    bool stats = getenv("CVF_FRAME_STATS") != NULL;
    if (stats) gc_frame_hook = frame_hook;
    i64 t0 = go_mono_ns();
    ik_init_globals();
    Slice_GoString args = {(GoString *)gc_alloc(sizeof(GoString) * (size_t)(argc > 2 ? argc - 2 : 1)), 0, argc - 2};
    for (int i = 2; i < argc; i++) args.p[args.len++] = go_cstr(argv[i]);
    i32 w = fn_cvfHeadlessMain(args);
    printf("winner: %d\n", w);
    if (stats) {
        i64 t1 = go_mono_ns();
        fprintf(stderr, "frames: %d fight frames, avg %.0f us, max %.0f us; to first fight frame %.2f s, total %.2f s\n", fs.n,
                fs.n ? fs.total / 1e3 / fs.n : 0.0, fs.max / 1e3, fs.load_end ? (fs.load_end - t0) / 1e9 : 0.0, (t1 - t0) / 1e9);
        fprintf(stderr, "frames: %.1f MB allocated from the first to the last fight frame (%.1f KB per frame)\n",
                (fs.alloc1 - fs.alloc0) / 1048576.0, fs.n ? (fs.alloc1 - fs.alloc0) / 1024.0 / fs.n : 0.0);
        fprintf(stderr, "frames: <1ms %lld, <2 %lld, <4 %lld, <8 %lld, <16.7 %lld, <33.3 %lld, <66.7 %lld, more %lld\n",
                (long long)fs.hist[0], (long long)fs.hist[1], (long long)fs.hist[2], (long long)fs.hist[3], (long long)fs.hist[4],
                (long long)fs.hist[5], (long long)fs.hist[6], (long long)fs.hist[7]);
    }
    if (getenv("CVF_GC_TRACE")) {
        GcStats st = gc_stats();
        fprintf(stderr, "gc summary: %llu collections, total alloc %.1f MB, live %.1f MB, hwm %.1f MB, pauses total %.1f ms max %.2f ms\n",
                (unsigned long long)st.collections, st.total_alloc / 1048576.0, st.live_bytes / 1048576.0,
                st.hwm_bytes / 1048576.0, st.total_pause_ms, st.max_pause_ms);
    }
    return w < 0 ? 1 : 0;
}
