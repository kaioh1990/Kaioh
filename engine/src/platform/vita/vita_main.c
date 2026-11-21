// PS Vita entry point of the native engine's headless application
// (VITA_PORT.md milestone M1): runs the unit tests (cvf_tests), then the
// reference fight (cvf_headless), and shows the results on screen. The real
// mains are compiled as cvf_tests_main (tests/test_main.c) and
// cvf_headless_main (src/app/headless_main.c); this file sets up what Linux
// gives them for free.
//
// Files (ux0:data/CvFNative/, created if missing):
//   cvf_env.txt     optional KEY=VALUE lines -> environment (CVF_GC_TRACE=1, ...)
//   headless_args.txt  optional, one line: <gamedir> <ikemen args...>
//                   (default: the reference fight in ux0:data/CvFNative/refgame)
//   tests.log, headless.log    stdout and stderr of the two runs
//   result.txt      what the screen shows
// The game directory holds the reference data (cvf-refdata.tgz plus
// data/Xsound.snd) and gets statedump.txt, as on the PC.
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "core/sched.h"  // go_vita_fp_ieee, go_mono_ns
#include "platform/vita/debugScreen.h"

#define DIR "ux0:data/CvFNative"

// main thread: the char compiler recurses deeply (the desktop main thread
// has 8 MB); newlib's malloc heap: goroutine stacks (512 KB each), stdio,
// the collector's side tables - the collector's arena is a memory block of
// its own (core/gc.c, 192 MB)
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
unsigned int _newlib_heap_size_user = 48 * 1024 * 1024;

int cvf_headless_main(int argc, char **argv);
int cvf_tests_main(int argc, char **argv);

static FILE *result_file;
static void vshow(bool to_log, const char *fmt, va_list ap) {
    char b[512];
    vsnprintf(b, sizeof b, fmt, ap);
    psvDebugScreenPuts(b);
    if (to_log) fputs(b, stderr);
    if (result_file) fputs(b, result_file), fflush(result_file);
}
// screen, result.txt and the current log
static void show(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vshow(true, fmt, ap);
    va_end(ap);
}
// screen and result.txt only: for lines read back from the current log
// (writing them to the log while reading it would never end)
static void show_nolog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vshow(false, fmt, ap);
    va_end(ap);
}

// stdout and stderr into one log file through one FILE: two streams on one
// file overwrite each other on the Vita (no O_APPEND semantics); newlib's
// stdout is _REENT->_stdout, so it can be pointed at stderr
static void log_to(const char *path) {
    fflush(stdout), fflush(stderr);
    freopen(path, "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    stdout = stderr;
}

// printf / puts / putchar of vitasdk's newlib write somewhere other than
// the FILE that `stdout` names (seen in vemu: neither stdout = stderr nor
// freopen of stdout catch them; fprintf(stdout) works). The engine's
// printf output (cvf_tests' results, "winner: N") goes to the log through
// these definitions, which take precedence over the C library's.
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
int puts(const char *s) { return fputs(s, stdout) < 0 || fputc('\n', stdout) < 0 ? EOF : 1; }
int putchar(int c) { return fputc(c, stdout); }

static void load_env(void) {
    FILE *f = fopen(DIR "/cvf_env.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}

// RFC 1321 MD5 of a file (the statedump), so the device shows the md5 to
// compare with the reference without copying the dump off it
typedef struct { uint32_t a, b, c, d; uint64_t n; uint8_t buf[64]; } Md5;
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static void md5_block(Md5 *m, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
        0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
        0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
        0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = d;
        d = c, c = b, b = b + rol(a + f + K[i] + w[g], R[i]), a = t;
    }
    m->a += a, m->b += b, m->c += c, m->d += d;
}
static bool md5_file(const char *path, char out[33], long *lines) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    Md5 m = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}};
    static uint8_t chunk[1 << 16];
    size_t k, used = 0;
    *lines = 0;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) {
        for (size_t i = 0; i < k; i++) {
            if (chunk[i] == '\n') ++*lines;
            m.buf[used++] = chunk[i];
            if (used == 64) md5_block(&m, m.buf), used = 0;
        }
        m.n += k;
    }
    fclose(f);
    uint64_t bits = m.n * 8;
    m.buf[used++] = 0x80;
    if (used > 56) {
        while (used < 64) m.buf[used++] = 0;
        md5_block(&m, m.buf), used = 0;
    }
    while (used < 56) m.buf[used++] = 0;
    for (int i = 0; i < 8; i++) m.buf[56 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(&m, m.buf);
    uint32_t v[4] = {m.a, m.b, m.c, m.d};
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", (v[i / 4] >> (8 * (i % 4))) & 0xff);
    return true;
}

static void wait_start(int seconds) {
    show("\nSTART: exit (or wait %d s)\n", seconds);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    for (int i = 0; i < seconds * 20; i++) {
        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & SCE_CTRL_START)) break;
        sceKernelDelayThread(50000);
    }
}

int main(void) {
    go_vita_fp_ieee();  // threads start in flush-to-zero mode on the Vita
    psvDebugScreenInit();
    mkdir(DIR, 0777);
    load_env();
    // the sound cache counts against the collector's arena: the TinyGo Vita
    // build's 24 MB (cvf_sound_lazy.go's isVita value, translated out here)
    if (!getenv("CVF_SOUND_CACHE_MB")) setenv("CVF_SOUND_CACHE_MB", "24", 1);
    int mhz = getenv("CVF_ARM_MHZ") ? atoi(getenv("CVF_ARM_MHZ")) : 444;
    scePowerSetArmClockFrequency(mhz);
    result_file = fopen(DIR "/result.txt", "w");
    show("CvF native engine (M1 headless), ARM %d MHz\n\n", scePowerGetArmClockFrequency());

    // 1. unit tests (detmath golden values, RNG, Atof, formats, scheduler,
    //    collector self-test): output in tests.log
    log_to(DIR "/tests.log");
    char *targv[] = {"cvf_tests", NULL};
    int tr = cvf_tests_main(1, targv);
    fflush(stdout), fflush(stderr);
    char line[1024], last[256] = "";
    FILE *lf = fopen(DIR "/tests.log", "r");
    while (lf && fgets(line, sizeof line, lf))
        if (strstr(line, " checks, ")) snprintf(last, sizeof last, "%s", line);
    if (lf) fclose(lf);
    show("unit tests: %s%s\n", last[0] ? last : "no result\n", tr ? "  FAILED, see " DIR "/tests.log\n" : "");

    // 2. the reference fight: output in headless.log
    log_to(DIR "/headless.log");
    if (!getenv("CVF_FRAME_STATS")) setenv("CVF_FRAME_STATS", "1", 1);
    if (!getenv("CVF_GC_TRACE")) setenv("CVF_GC_TRACE", "1", 1);
    snprintf(line, sizeof line, "%s", DIR "/refgame -p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1");
    FILE *af = fopen(DIR "/headless_args.txt", "r");
    if (af) {
        char l2[1024];
        if (fgets(l2, sizeof l2, af) && l2[0] > ' ') snprintf(line, sizeof line, "%s", l2);
        fclose(af);
    }
    line[strcspn(line, "\r\n")] = 0;
    char *argv[64];
    int argc = 0;
    argv[argc++] = "cvf_headless";
    for (char *t = strtok(line, " \t"); t && argc < 63; t = strtok(NULL, " \t")) argv[argc++] = t;
    argv[argc] = NULL;
    int r = tr;
    struct stat st;
    if (argc < 2 || stat(argv[1], &st) != 0) {
        show("\nno game directory %s:\nreference fight skipped (copy cvf-refdata + data/Xsound.snd there)\n", argc > 1 ? argv[1] : "");
    } else {
        show("\nreference fight in %s\n(a minute or two; log: " DIR "/headless.log)...\n", argv[1]);
        // a dump left by an earlier run must not be reported for this one
        char dump[512];
        snprintf(dump, sizeof dump, "%s/statedump.txt", argv[1]);
        remove(dump);
        int64_t t0 = go_mono_ns();
        r = cvf_headless_main(argc, argv);  // chdirs to the game directory
        double secs = (go_mono_ns() - t0) / 1e9;
        fflush(stdout), fflush(stderr);
        char md5[33] = "";
        long lines = 0;
        if (md5_file("statedump.txt", md5, &lines))
            show("exit %d after %.1f s\nstatedump.txt: %ld lines\nmd5 %s\n", r, secs, lines, md5);
        else
            show("exit %d after %.1f s\nno statedump.txt (needs vita_statedump.txt in the game directory)\n", r, secs);
        if (!strcmp(md5, "54f8ae06d74648af2182587b82a390d6")) show("= REFERENCE (cvf-online-2 rules)\n");
        else if (!strcmp(md5, "4334350cdebd48dc0caa04cf78e0d9e3")) show("= REFERENCE (cvf-online-5 rules)\n");
        else if (md5[0]) show("DIFFERENT from both reference dumps\n");
        lf = fopen(DIR "/headless.log", "r");
        while (lf && fgets(line, sizeof line, lf))
            if (!strncmp(line, "frames:", 7) || !strncmp(line, "winner:", 7) || !strncmp(line, "gc summary", 10)) show_nolog("%s", line);
        if (lf) fclose(lf);
    }
    if (result_file) fclose(result_file), result_file = NULL;
    wait_start(60);
    sceKernelExitProcess(r);
    return r;
}
