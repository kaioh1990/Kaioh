/*
 * IOSurfaceRootUserClient Comprehensive Selector Scanner
 * Educational jailbreak research only. For use on devices you own.
 *
 * Target: iPhone Duo (A20 Pro), iOS 27.1
 * Purpose: Enumerate ALL accessible selectors on IOSurfaceRootUserClient
 *
 * Build: clang -arch arm64e -framework IOKit -framework IOSurface \
 *        -framework CoreFoundation -o iosurface_selector_scan iosurface_selector_scan.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#define MAX_SELECTOR     60
#define MAX_INPUT_SIZE   256

static const char *selector_names[] = {
    [0]  = "s_create_surface",
    [1]  = "s_release_surface",
    [2]  = "s_lock_surface",
    [3]  = "s_unlock_surface",
    [4]  = "s_lookup_by_id",
    [5]  = "s_lookup_by_mach_port",
    [6]  = "s_get_value",
    [7]  = "s_remove_all_values",
    [8]  = "s_copy_all_values",
    [9]  = "s_set_value",
    [10] = "s_remove_value",
    [11] = "s_increment_use_count",
    [12] = "s_decrement_use_count",
    [13] = "s_get_use_count",
    [14] = "s_set_client_ref_count",
    [15] = "s_get_client_ref_count",
    [16] = "s_get_surface_count",
    [17] = "s_set_surface_notify",
    [18] = "s_get_surface_notify",
    [19] = "s_unknown_19",
    [20] = "s_unknown_20",
    [21] = "s_set_bulk_attach",
    [54] = "s_set_corevideo_bridged_keys",
};

static const char *
kr_name(kern_return_t kr)
{
    switch (kr) {
    case 0x0:        return "SUCCESS";
    case 0xe00002bc: return "BAD_ARGUMENT";
    case 0xe00002c2: return "NOT_PERMITTED";
    case 0xe00002c7: return "UNSUPPORTED";
    case 0xe00002ed: return "NOT_FOUND";
    case 0xe00002be: return "NO_MEMORY";
    case 0xe00002cd: return "INVALID";
    case 0xe00002ce: return "STILL_OPEN";
    case 0xe00002d1: return "BUSY";
    case 0xe00002eb: return "TIMEOUT";
    case 0xe0000001: return "GENERAL_ERROR";
    default:         return "OTHER";
    }
}

typedef struct {
    uint32_t sel;
    kern_return_t kr;
    const char *name;
    int accessible;
} scan_result_t;

static io_connect_t
open_iosurface_uc(void)
{
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "[-] IOSurfaceRoot not found\n");
        return MACH_PORT_NULL;
    }
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[-] IOServiceOpen failed: 0x%x\n", kr);
        return MACH_PORT_NULL;
    }
    return conn;
}

static uint32_t
create_test_surface(void)
{
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int32_t w = 64, h = 64, bpe = 4;
    int32_t pitch = w * bpe, alloc = w * h * bpe;
    CFNumberRef n;
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &w);
    CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pitch);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &alloc);
    CFDictionarySetValue(props, kIOSurfaceAllocSize, n); CFRelease(n);
    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    if (!surf) return 0;
    uint32_t sid = IOSurfaceGetID(surf);
    printf("[+] Test surface created: ID=%u\n", sid);
    return sid;
}

static kern_return_t
probe_selector(io_connect_t conn, uint32_t sel, uint32_t surface_id,
               const void *input, size_t input_size)
{
    uint8_t out[256] = {0};
    size_t out_size = sizeof(out);
    uint64_t out_scalars[4] = {0};
    uint32_t out_scalar_cnt = 4;
    uint64_t in_scalars[2] = { surface_id, 0 };
    uint32_t in_scalar_cnt = (surface_id != 0) ? 1 : 0;
    return IOConnectCallMethod(
        conn, sel,
        in_scalars, in_scalar_cnt,
        input, input_size,
        out_scalars, &out_scalar_cnt,
        out, &out_size);
}

static void
phase1_scan(io_connect_t conn, uint32_t sid, scan_result_t *results)
{
    printf("\n========== PHASE 1: Initial Selector Scan ==========\n");
    printf("%-4s %-32s %-20s %s\n", "Sel", "Name", "Return Code", "Status");
    printf("---- -------------------------------- -------------------- ------\n");

    for (uint32_t sel = 0; sel <= MAX_SELECTOR; sel++) {
        uint8_t input[24] = {0};
        memcpy(input, &sid, sizeof(sid));
        kern_return_t kr = probe_selector(conn, sel, sid, input, sizeof(input));
        const char *name = (sel < sizeof(selector_names)/sizeof(selector_names[0]) && selector_names[sel])
            ? selector_names[sel] : "???";
        int accessible = (kr == 0x0 || kr == 0xe00002bc || kr == 0xe00002be ||
                          kr == 0xe00002cd || kr == 0xe00002d1);
        results[sel] = (scan_result_t){ sel, kr, name, accessible };
        printf("%-4u %-32s 0x%08x %-12s %s\n",
               sel, name, kr, kr_name(kr),
               accessible ? "[ACCESSIBLE]" :
               (kr == 0xe00002c2 ? "[BLOCKED]" : ""));
    }
}

static void
phase2_input_sizing(io_connect_t conn, uint32_t sid, scan_result_t *results)
{
    printf("\n========== PHASE 2: Input Size Mapping ==========\n");
    static const size_t sizes[] = {0, 4, 8, 12, 16, 20, 24, 32, 48, 64, 96, 128, 192, 256};
    int nsizes = sizeof(sizes)/sizeof(sizes[0]);

    for (uint32_t sel = 0; sel <= MAX_SELECTOR; sel++) {
        if (!results[sel].accessible) continue;
        printf("\nSelector %u (%s) - input size sweep:\n", sel, results[sel].name);
        for (int i = 0; i < nsizes; i++) {
            uint8_t input[256] = {0};
            memcpy(input, &sid, sizeof(sid));
            if (sizes[i] >= 8) { uint32_t s2 = sid; memcpy(input + 4, &s2, 4); }
            kern_return_t kr = probe_selector(conn, sel, sid, input, sizes[i]);
            printf("  size=%-4zu -> 0x%08x (%s)%s\n",
                   sizes[i], kr, kr_name(kr),
                   kr == 0x0 ? " <-- SUCCESS" : "");
        }
    }
}

static void
phase3_scalar_variations(io_connect_t conn, uint32_t sid, scan_result_t *results)
{
    printf("\n========== PHASE 3: Scalar Input Variations ==========\n");
    for (uint32_t sel = 0; sel <= MAX_SELECTOR; sel++) {
        if (!results[sel].accessible) continue;
        printf("\nSelector %u (%s) - scalar count sweep:\n", sel, results[sel].name);
        for (uint32_t sc = 0; sc <= 4; sc++) {
            uint64_t scalars[4] = { sid, 0, 0, 0 };
            uint8_t out[256] = {0};
            size_t out_size = sizeof(out);
            uint64_t out_scalars[4] = {0};
            uint32_t out_cnt = 4;
            kern_return_t kr = IOConnectCallMethod(
                conn, sel, scalars, sc, NULL, 0,
                out_scalars, &out_cnt, out, &out_size);
            printf("  scalars=%u, no struct -> 0x%08x (%s)", sc, kr, kr_name(kr));
            if (kr == 0x0)
                printf(" <-- SUCCESS (out_scalars=%u, out_struct=%zu)", out_cnt, out_size);
            printf("\n");
        }
    }
}

static void
print_summary(scan_result_t *results)
{
    printf("\n========== SUMMARY ==========\n");
    int accessible = 0, blocked = 0, unsupported = 0;
    printf("\nAccessible selectors:\n");
    for (uint32_t sel = 0; sel <= MAX_SELECTOR; sel++) {
        if (results[sel].accessible) {
            printf("  [%u] %s (kr=0x%x)\n", sel, results[sel].name, results[sel].kr);
            accessible++;
        }
        if (results[sel].kr == 0xe00002c2) blocked++;
        if (results[sel].kr == 0xe00002c7) unsupported++;
    }
    printf("\nBlocked (entitlement-gated) selectors:\n");
    for (uint32_t sel = 0; sel <= MAX_SELECTOR; sel++) {
        if (results[sel].kr == 0xe00002c2)
            printf("  [%u] %s\n", sel, results[sel].name);
    }
    printf("\nTotals: %d accessible, %d blocked, %d unsupported\n",
           accessible, blocked, unsupported);
}

int main(void)
{
    printf("=== IOSurfaceRootUserClient Selector Scanner ===\n");
    printf("Target: iOS 27.1, A20 Pro (arm64e)\n\n");
    io_connect_t conn = open_iosurface_uc();
    if (conn == MACH_PORT_NULL) return 1;
    printf("[+] User client opened: 0x%x\n", conn);
    uint32_t sid = create_test_surface();
    if (sid == 0) { fprintf(stderr, "[-] Failed to create test surface\n"); IOServiceClose(conn); return 1; }
    scan_result_t results[MAX_SELECTOR + 1] = {0};
    phase1_scan(conn, sid, results);
    phase2_input_sizing(conn, sid, results);
    phase3_scalar_variations(conn, sid, results);
    print_summary(results);
    IOServiceClose(conn);
    return 0;
}
