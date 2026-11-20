# SPTM/TXM Bypass Analysis — iOS 27.0.1 / A17 Pro (iPad16,1)

## Executive Summary

**SPTM bypass is the single hardest blocker for the jailbreak chain.** After extensive
research, we have identified **three viable attack paths**, ranked by feasibility:

1. **Path A: Titan-style AGX coprocessor attack** — HIGH potential, needs iOS 27 adaptation
2. **Path B: Data-only exploitation (cs_flags + AMFI)** — MEDIUM potential, avoids SPTM entirely  
3. **Path C: TXM interface abuse via txm_kernel_call()** — LOW potential, TXM validates thoroughly

---

## Architecture Overview

### What SPTM Does (sigreturn.com, DFF/newosxbook)
- Runs at GL2 (Guarded Level 2), above XNU's EL1
- **Only software that can write page table entries**
- XNU calls SPTM via `GENTER` instruction with selector in X16
- SPTM manages physical frame types: XNU_DEFAULT, XNU_PAGE_TABLE, XNU_USER_EXEC, etc.
- Frame retyping is domain-scoped: XNU can't retype TXM frames, and vice versa

### What TXM Does
- Runs at GL0, below SPTM but above XNU
- **Owns code signing, trust caches, entitlements, provisioning profiles**
- XNU calls TXM through SPTM via `txm_kernel_call()` / `txm_enter()`
- 40+ selectors including LoadTrustCache, QueryTrustCache, RegisterCodeSignature
- Trust cache data sits in TXM-domain frames — kernel R/W cannot touch them

### SPTM Subsystem Dispatch (from DFF disassembly)
| # | Subsystem | Description |
|---|-----------|-------------|
| 0 | XNU | 22 calls: retype, map_table, unmap_table, nest_region, sign/auth_user_pointer |
| 1 | TXM | 5 calls: TXM dispatch |
| 2 | Unknown | 3 calls |
| 3 | t8110dart | 17 calls: DART IOMMU |
| 4 | Unknown | Unknown |
| 5 | SART | 3 calls: SART IOMMU |
| 6 | NVMe | 5 calls: NVMe subsystem |
| 7 | UAT | 13 calls: GPU Unified Address Translation |

---

## Path A: Titan-Style AGX Coprocessor Attack

### How Titan Works (alfiecg24, confirmed on iOS 16.1–17.3.1, A14–A17)
1. **Get kernel R/W** (from IOSurface race, CVE-2026-112477)
2. **Corrupt AGX power thread state** — overwrite thread state pointer in GPU firmware
3. **AGX executes attacker-controlled ROP chain** at coprocessor privilege
4. **Modify AGX hibernation context** — replace `TTBR1_EL1` with attacker page tables
5. **Map XNU page table into AGX address space** — coprocessor sees kernel page tables
6. **Write self-referencing PTE** — from AGX ROP, write to mapped kernel page table
7. **Full physical memory R/W** — self-referencing PTE gives arbitrary page mapping
8. **SPTM defeated** — can now write to any physical page including TXM frames

### Why Titan Was Patched
- **iOS 17.4 beta 4**: Apple hardcoded `TTBR1_EL1` restore value during hibernation
  - `TTBR0_EL1` no longer restored at all
  - Hibernation context no longer controllable
- **iOS 18.1 beta 4**: Added instruction PAC to AGX (PACGA verification of thread state)
  - Thread state signature verified before restore
  - PC, LR, CPSR, X16, X17 all PAC-protected

### Adaptation for iOS 27.0.1 / A17 Pro

**Key question: What is the AGX PAC/hibernation status on iOS 27.0.1 on A17 Pro?**

The A17 Pro shipped with iOS 17.0, which predates the 17.4 hibernation fix.
However, iOS 27.0.1 is a much newer kernel — it likely includes both fixes.

**What we need to investigate:**
1. Extract AGX firmware from iPad16,1 27.0.1 IPSW (`audit_rtkit_pac.py`)
2. Check if AGX firmware has PAC instructions (PACGA, PACIASP, etc.)
3. Check if hibernation context `TTBR1_EL1` is still restored from memory
4. If PAC present: look for PAC key management weaknesses
5. If hibernation fixed: look for other coprocessor entry points

**Alternative coprocessors** (if AGX is hardened):
- DCP (Display Coprocessor)
- AOP (Always-On Processor) 
- ANE (Neural Engine)
- ISP (Image Signal Processor)
- SmartIO / SIO

Each runs RTKit firmware. If ANY of them lacks PAC and can DMA to kernel pages,
the same technique applies. This is exactly what `audit_rtkit_pac.py` checks.

**Titan requires:**
- `kalloc` — permanent kernel allocations (we have via IOSurface)
- `kmap` — map arbitrary physical pages to userspace (available via IOSurface properties)
- IPC port stealing — patched in iOS 26 via data PAC on `ipc_port->ip_nsrequest`

The IPC port stealing patch is a problem. Need alternative primitive or bypass.

### Verdict: HIGH POTENTIAL but needs IPSW analysis
- Must download iPad16,1 27.0.1 IPSW and audit coprocessor firmware
- If any coprocessor lacks PAC → Titan-style attack is viable
- Even with PAC, coprocessor attack surface is large

---

## Path B: Data-Only Exploitation

### Concept
Instead of injecting trust caches (which requires TXM cooperation), modify kernel
data structures that control code execution policy WITHOUT touching TXM memory.

### Attack Vector B1: cs_flags Manipulation
On pre-SPTM devices, setting `CS_DEBUGGED` on a process's `cs_flags` allows it to
run with invalid code pages. The question: **are cs_flags in XNU-writable memory?**

**Evidence:**
- cs_blobs.h defines CS_DEBUGGED, CS_PLATFORM_BINARY, CS_GET_TASK_ALLOW
- A Play--CodeGen PR says on TXM/SPTM devices (iOS 26, A15+), CS_DEBUGGED still
  gates JIT page creation, and an attached debugger sets it
- If cs_flags are in `proc` struct (XNU_DEFAULT memory), kernel R/W can set them
- If cs_flags moved to TXM-owned memory, this path is dead

**What to check in XNU source:**
```c
// In bsd/sys/proc_internal.h:
struct proc {
    ...
    uint32_t p_csflags;  // <-- Is this still in proc, or delegated to TXM?
    ...
};
```

**Risk:** Even if cs_flags are writable, AMFI may re-check with TXM on each exec.
Modern AMFI defers to TXM for code-signing verdicts on SPTM devices.

### Attack Vector B2: AMFI Exception Mechanisms
- `amfi_get_out_of_my_way=1` boot-arg: Historically disables AMFI enforcement
- On SPTM devices, TXM also processes this boot-arg (found in TXM binary by DFF)
- Cannot set boot-args without iBoot control — not viable from kernel R/W alone

### Attack Vector B3: Developer Mode / CS_DEBUGGED + dyld
- If we can set CS_DEBUGGED via kernel write on our process
- Then set `DYLD_INSERT_LIBRARIES` or modify dyld config
- Load unsigned dylib into our process
- Dylib gets to run because CS_DEBUGGED allows invalid pages

**Problem:** On SPTM devices, TXM is the authority. It may override cs_flags.

### Attack Vector B4: MAP_JIT + Code Generation
- MAP_JIT regions allow JIT compilation (used by JSC, V8)
- If we can mark our process as JIT-capable via kernel write
- We could allocate MAP_JIT pages and write executable code
- This is how WebKit's JavaScript JIT works — the mechanism exists

**Key insight from sigreturn.com:**
> Row 5 of the SPRR table: MAP_JIT pages give userland rw- or r-x depending
> on the per-thread toggle. The kernel sees read-only. GL2 sees no access.

So MAP_JIT pages are toggled between writable and executable by a per-thread
toggle, NOT by page table modification. This means SPTM doesn't need to be
involved for the toggle — it's a register write.

**Attack:** If we can get our process to have MAP_JIT entitlement, the JIT toggle
mechanism gives us W^X without SPTM involvement.

### Attack Vector B5: Kernel Object Spray for Trust Cache Forgery
Quarkslab's analysis of modern jailbreaks notes that data-only attacks patch
kernel state directly in `__DATA` segments. If we can:
1. Find where AMFI caches code-signing decisions in kernel memory
2. Modify the cached decision to "trusted" for our binaries
3. AMFI returns cached "yes" without re-checking TXM

This is speculative but worth investigating in the AMFI kext.

### CRITICAL FINDING: cs_flags ARE in proc_ro (ROZONE)

**BUT ROZONE is writable via physical aperture!**

XNU source reveals:
```c
// proc_ro.h: p_csflags is in struct proc_ro
struct proc_ro {
    ...
    uint32_t p_csflags;    // IN ROZONE!
    ...
};

// kern_proc.c: Updates go through zalloc_ro_update_field
void proc_csflags_update(proc_t p, uint64_t flags) {
    zalloc_ro_update_field(ZONE_ID_PROC_RO, proc_get_ro(p), p_csflags, &csflags);
}

// SPTM pmap.c: RO zone write uses PHYSICAL APERTURE
// pmap_ro_zone_prepare_write() is a NO-OP on SPTM!!
MARK_AS_PMAP_TEXT void pmap_ro_zone_memcpy_internal(...) {
    const pmap_paddr_t pa = kvtophys_nofail(va + offset);
    pmap_ro_zone_prepare_write();   // <-- EMPTY FUNCTION ON SPTM!
    memcpy((void*)phystokv(pa), (void*)new_data, new_data_size);
    pmap_ro_zone_complete_write();  // <-- EMPTY FUNCTION ON SPTM!
}
```

The physical aperture (`phystokv()`) maps ALL physical memory RW.
SPTM protects the *virtual* mapping of ROZONE (read-only PTEs), but
the physical aperture is a SEPARATE RW mapping that the kernel uses
for legitimate ROZONE writes.

**Attack: If kernel R/W can access any kernel VA, we can write ROZONE
through physmap the same way the kernel does:**
1. Read `proc_get_ro(our_proc)` to get proc_ro address
2. `kvtophys(proc_ro_addr + offsetof(p_csflags))` to get physical addr
3. `phystokv(phys_addr)` to get the physical aperture VA
4. Write CS_DEBUGGED | CS_PLATFORM_BINARY to that address
5. Our process now has code-signing bypassed

**This avoids TXM and SPTM entirely** — we're using the kernel's own
physical aperture, which is RW at EL1.

BUT WAIT: Does SPTM restrict physical aperture writes for specific
frame types? The XNU source suggests NOT — `prepare_write()` is a no-op.
The SPTM's protection is at the page TABLE level, not the physmap.
Verification needed with IPSW kernelcache analysis.

### Verdict: HIGH POTENTIAL (upgraded from MEDIUM)
- cs_flags in proc_ro BUT writable via physical aperture
- Physical aperture bypasses SPTM page table restrictions
- Same mechanism the kernel uses for legitimate ROZONE updates
- MAP_JIT toggle path also interesting if cs_flags path works
- **This may be the simplest path to code execution without trust cache**

---

## Path C: TXM Interface Abuse

### How Trust Cache Loading Works (from kern_trustcache.c)
```c
// On CONFIG_SPTM devices:
txm_call_t txm_call = {
    .selector = kTXMKernelSelectorLoadTrustCache,
    .num_input_args = 7,
    .num_output_args = 1,
};
// 1. Allocate kernel memory for payload + manifest
// 2. Copy trust cache payload and Image4 manifest
// 3. txm_transfer_region() — transfers memory ownership to TXM domain
// 4. txm_kernel_call() — TXM validates manifest, loads trust cache
ret = txm_kernel_call(&txm_call, type, payload_addr, payload_len,
                       manifest_addr, manifest_len, 0, 0);
```

### Why Direct Abuse is Difficult
1. **Image4 manifest validation**: TXM validates Apple-signed Image4 manifests
   - Manifest contains device-specific ECID, nonce, chip ID
   - Personalized by Apple's signing server for each restore
   - Cannot forge without Apple's private key

2. **txm_transfer_region()**: Transfers memory ownership to TXM domain
   - After transfer, XNU can't modify the pages
   - SPTM retype changes frame from XNU_DEFAULT to TXM-owned

3. **TXM entry is through SPTM**: Can't call TXM directly
   - XNU → GENTER → SPTM → dispatches to TXM
   - SPTM validates the selector and arguments

### Possible Attacks on TXM Path

**C1: Existing trust cache augmentation**
- iOS ships with trust caches already loaded (static trust cache from boot)
- Can we find the loaded trust cache in TXM memory and add entries?
- **NO** — TXM memory is not writable from kernel R/W

**C2: TOCTOU on txm_transfer_region()**
- Race between preparing trust cache payload in XNU memory and transferring to TXM
- Modify payload bytes after validation but before transfer
- **UNLIKELY** — transfer is atomic from XNU's perspective (single GENTER)

**C3: Reuse existing signed trust cache**
- Extract trust cache from IPSW (it's Apple-signed)
- Modify it to include our CDHashes
- **FAILS** — modifying content invalidates the Image4 signature

**C4: Query selectors for information leak**
- `kTXMKernelSelectorQueryTrustCache` — query if a CDHash is trusted
- `kTXMKernelSelectorGetTrustCacheInfo` — get trust cache metadata
- Could leak TXM internal state, but doesn't help inject entries

**C5: Engineering trust cache (kTCTypeDTRS)**
- XNU source shows: `load_trust_cache_with_type(kTCTypeDTRS, ...)`
- Engineering trust caches for developer/test builds
- Gated by `TRUST_CACHE_INCLUDE_INTERNAL_CODE` compile flag
- **Not compiled into release kernels**

### Verdict: LOW POTENTIAL
- TXM validation is comprehensive
- Image4 signing prevents forgery
- No known weak selectors

---

## Recommended Strategy

### Phase 1: IPSW Analysis (Can do NOW)
1. Download iPad16,1 27.0.1 IPSW
2. Extract all coprocessor firmware (AGX, DCP, AOP, ANE, ISP, SIO)
3. Run `audit_rtkit_pac.py` to check for PAC-free firmware
4. Extract SPTM and TXM binaries for A17 Pro
5. Disassemble SPTM dispatch table for iOS 27 changes

### Phase 2: Kernel Analysis (Can do NOW)
1. Extract kernelcache from IPSW
2. Find `proc->p_csflags` offset — confirm it's in XNU_DEFAULT memory
3. Find AMFI code-signing decision cache locations
4. Map TXM selector handler addresses
5. Check for new SPTM subsystems added in iOS 27

### Phase 3: Exploit Development (After analysis)
- **If coprocessor lacks PAC**: Titan-style attack (Path A)
- **If cs_flags writable**: Data-only cs_flags + JIT approach (Path B)
- **If neither**: Wait for Calif.io release or find novel bypass

---

## Key Sources

- [Titan (alfiecg24)](https://github.com/alfiecg24/Titan) — SPTM bypass iOS 16–17
- [sigreturn.com: SPTM, TXM and memory tagging](https://sigreturn.com/blog/sptm-txm-memory-tagging/)
- [DFF: SPTM Round 2](https://newosxbook.com/bonus/sptm2.html)
- [Coruna analysis (littlelailo)](https://littlelailo.github.io/writeups/coruna.html)
- [PPL/SPTM Bypasses - Apple Wiki](https://theapplewiki.com/wiki/PPL/SPTM_Bypasses)
- [Modern iOS Security (arXiv)](https://arxiv.org/abs/2510.09272)
- [iOS code signing: AMFI, amfid, trust cache (sigreturn)](https://sigreturn.com/blog/ios-code-signing-pipeline/)
- [Rocket writeup (nvme0n1p)](https://nvme0n1p.dev/en/posts/rocket-writeup)
- [DFF: SPTM Round 3](https://df-f.com/blog/sptm3)
- [Quarkslab: Modern Jailbreaks Post-Exploitation](https://blog.quarkslab.com/modern-jailbreaks-post-exploitation.html)
- [Inside Binary: Why Modern Jailbreaks Ignore PAC](https://insidebinary.com/blog/why-modern-ios-jailbreaks-ignore-pointer-authentication)
- [XNU source: kern_trustcache.c](https://newosxbook.com/src.php?file=%2Fbsd%2Fkern%2Fkern_trustcache.c&tree=xnu)
