# SPTM Bypass Research — iOS 27.0.1 / A17 Pro

## Date: November 20, 2026

---

## 1. SPTM Architecture Summary

SPTM (Secure Page Table Monitor) replaced PPL on A15+ / M2+ chips starting with iOS 17.
It runs at **GL2** (Guarded Execution Level 2), a privilege level provided by Apple's
proprietary GXF (Guarded Execution Facility) extension — orthogonal to ARM's EL0–EL3.

### How XNU Communicates with SPTM

XNU cannot modify page tables directly. Every mapping goes through:
- `sptm_map_page(pmap, va, pa, prot, ...)` — request a new mapping
- `sptm_retype(current_type, new_type, params)` — change a frame's type

SPTM validates each request against:
1. Is the physical address managed?
2. Is the new type valid for this transition?
3. Does the caller's claimed previous type match SPTM's frame table?

### Frame Types

SPTM tags every physical page frame with a type:
- **SPTM_UNTYPED** — can be retyped to any type
- **XNU_DEFAULT** — ordinary kernel heap memory (what kernel R/W can reach)
- **XNU_PAGE_TABLE** — kernel page table pages
- **TXM_SLAB_TRUST_CACHE** — trust cache storage (TXM-managed)
- **TXM_SLAB_CODE_SIGNATURE** — code signature storage
- **SK_SHARED_RW/RO** — secure kernel shared regions

XNU can only map frames typed XNU_DEFAULT and a few shared types.
Trust cache, code signatures, and page tables are out of reach for
kernel R/W alone.

### TXM (Trust eXecution Monitor)

Runs alongside SPTM, handles code signing and trust cache:
- Manages trust cache entries in TXM-typed frames
- Verifies code signatures before allowing execution
- A TXM compromise does NOT automatically translate to SPTM bypass

---

## 2. Known SPTM Bypass Techniques

### A. Rocket / Titan — AGX Coprocessor Hibernation (iOS 16.1–17.3.1)

**CVE-2024-23296** | Patched in iOS 17.4 | A14–A17

**Mechanism:**
1. Gain code execution on AGX GPU coprocessor via ROP chain
2. Modify AGX's hibernation context to load attacker-controlled TTBR1_EL1
3. On resume from sleep, AGX loads fake page tables
4. Fake tables map an XNU page table page into AGX's address space
5. Second ROP chain writes a **self-referencing page table entry**
6. Self-referencing PTE allows mapping any physical page, including SPTM-protected ones

**What was patched:** iOS 17.4 beta 4 — `arm_arch_resume_uat` now skips TTBR0,
TTBR1, and VBAR fields from the hibernation context. The resume path no longer
restores attacker-controlled translation registers.

**iOS 18 follow-up:** iOS 18.1 beta 4 added instruction PAC to AGX firmware,
further blocking the coprocessor ROP chain.

**Applicability to iOS 27:** ❌ Both patches in place. The hibernation resume
path and AGX firmware are hardened.

### B. silentIO — SmartIO RTKit DMA Write (iOS 18.6.2)

**No CVE** | Targets iPhone 16 Pro (A18 Pro / T8140)

**Mechanism:**
1. Prerequisites: kernel R/W (from separate exploit, e.g., DarkSword)
2. Use SmartIO coprocessor's RTKit firmware as a DMA writer
3. SmartIO DMA writes bypass SPTM's page-table protections
4. Write controlled 8 bytes into an XNU page-table page
5. This gives arbitrary physical page mapping

**Key insight:** SPTM monitors CPU memory access via page tables but
coprocessor DMA goes through DART (Apple's IOMMU), which has different
protection rules. The gap between SPTM and DART is the attack surface.

**Applicability to iOS 27 / A17 Pro:** ⚠️ UNCERTAIN
- SmartIO is present on A17 Pro (iPhone 15 Pro has SmartIO for USB/Thunderbolt)
- The DART protection model may differ between A17 and A18
- iOS 27.2 reportedly adds "IOMMU/DART page-table reference-count parity checking"
  — this may close the silentIO-style gap, but on 27.0.1 it's pre-patch
- **This is the most promising lead for our target**

### C. Fentanyl — SPTM Bypass (iOS 18.0–18.6.1)

**No CVE** | Mechanism unknown from search results

A GitHub project (randomproxymaker/Fentanyl) claims SPTM bypass for iOS 18.0–18.6.1.
Could not determine the technique from available information. Needs code review.

### D. pmap_tte_remove — pt_desc Refcount Overflow (A13–A19)

**No CVE** | "Mitigated but not fixed" through iOS 26.6b1

**Mechanism:**
1. `pt_desc` structure uses `uint16_t` refcount (max 65,535)
2. Create 65,537 MAP_SHARED mappings to same physical page → overflow to 0
3. XNU thinks the page table page has zero references → frees it
4. Physical page is freed while still mapped → **physical UAF**
5. Reclaim freed page with controlled data → arbitrary page-table content
6. Bypasses SPTM because the write happens through the stale mapping,
   not through `sptm_map_page`

**Status claims:**
- 64/64 write-through on A13-A19 (unverified, single source)
- Apple added overflow detection in iOS 26.4.2 but never widened the type
- Author claims "mitigated, not fixed" — the fundamental overflow remains

**Applicability to iOS 27 / A17 Pro:** ⚠️ HIGH POTENTIAL
- A17 Pro is in the claimed affected range (A13-A19)
- If the uint16 width persists in iOS 27.0.1, the overflow is still possible
- Apple's mitigations may be runtime checks that can be bypassed
- **Verifiable from XNU source** — pt_desc uses `unsigned short` for refcnt
- Need to test whether the MAP_SHARED overflow path works on iOS 27.0.1

### E. DART Un-Ganged TLB Invalidation Race (iOS 27.0.1)

**No CVE** | Patched in iOS 27.2

One analysis flags `VIOLATION_T8110_DART_UNGANG_GAPF_RACE` as a new SPTM
violation string added in 27.2 — suggesting a DART TLB race was fixed.

**Mechanism (speculative):**
- DART uses ganged TLB invalidation for consistency
- An "un-ganged" invalidation could leave a stale TLB entry
- Stale DART TLB entry allows DMA access to freed/retyped physical pages
- Could be combined with coprocessor DMA to bypass SPTM

**Applicability to iOS 27.0.1:** ⚠️ PROMISING
- The fix is in 27.2, so 27.0.1 is pre-patch
- Needs confirmation of whether this is exploitable (not just a string change)
- T8110 in the violation name maps to A17 Pro's DART variant

---

## 3. Attack Strategy Ranking

### Strategy 1: pt_desc Refcount Overflow (HIGHEST PRIORITY)

**Why:** Works at the pmap layer, below SPTM. The overflow creates a physical
UAF that SPTM cannot prevent because the corrupted page table was legitimately
allocated. SPTM's frame-type tracking doesn't help when the page is still typed
as XNU_PAGE_TABLE but its contents are attacker-controlled.

**Steps:**
1. With kernel R/W (from CVE-2026-112477 IOSurface race), call mmap() to
   create MAP_SHARED mappings
2. Target a known page-table page's physical address
3. Overflow the uint16 refcount → XNU frees the pt_desc
4. Reclaim the physical page via kernel heap spray
5. Write controlled page-table entries
6. Map trust cache physical pages → inject CDHashes

**Risk:** Apple may have added per-call rate limiting or moved to uint32 in
iOS 27. Need to verify from kernelcache.

### Strategy 2: SmartIO/Coprocessor DMA Write (MEDIUM PRIORITY)

**Why:** Coprocessor DMA bypasses SPTM's CPU-side protections. SmartIO is on A17 Pro.

**Steps:**
1. With kernel R/W, locate SmartIO's RTKit mailbox interface
2. Craft a DMA transfer descriptor targeting a page-table physical address
3. SmartIO writes controlled data via DMA → page table corruption
4. SPTM doesn't intercept DMA writes (they go through DART only)

**Risk:** DART protection may be tighter on A17 than on A18. Need to verify
DART configuration for SmartIO on iPad Mini.

### Strategy 3: DART TLB Invalidation Race (MEDIUM-LOW PRIORITY)

**Why:** Present on 27.0.1, patched in 27.2. T8110 matches A17 Pro.

**Steps:**
1. With kernel R/W, manipulate DART page tables
2. Race the TLB invalidation to retain a stale entry
3. Stale entry allows DMA to freed physical page
4. Reclaim page → controlled content written via DMA

**Risk:** Race conditions are inherently unreliable. The violation string
may indicate a detection mechanism rather than a fix for the actual bug.

### Strategy 4: Data-Only Trust Cache via Linked List (FALLBACK)

**Why:** Even without SPTM bypass, some trust cache implementations use a
linked list in kernel memory (XNU_DEFAULT frames). If TXM's trust cache
validation has gaps when processing linked entries, we might inject without
modifying TXM-protected pages.

**Steps:**
1. With kernel R/W, allocate kernel memory for a fake trust_cache_entry
2. Fill with target CDHashes
3. Find and modify the trust_cache_head pointer in XNU data segment
4. Link our entry at the head of the list

**Risk:** On SPTM devices, trust cache is reportedly in TXM frames. But
Dopamine's rootless approach may work if the linked list metadata (the
head pointer and next pointers) are in XNU_DEFAULT memory even though
the actual slab contents are TXM-typed. Need to verify.

---

## 4. Recommended Approach

**Primary: pt_desc refcount overflow (Strategy 1)**

This is the strongest candidate because:
1. It operates below SPTM's awareness (pmap layer)
2. It's reportedly not fully fixed through iOS 26.6b1
3. It affects A13-A19 (our A17 Pro is in range)
4. It doesn't require coprocessor access (pure kernel-side)
5. The uint16 width is verifiable from Apple's own XNU source

**Next steps:**
1. Extract kernelcache from iPad Mini iOS 27.0.1
2. Verify pt_desc refcount field width (uint16 vs uint32)
3. Check for overflow mitigations in pmap_enter_options
4. Build PoC: 65537 MAP_SHARED → trigger overflow → verify freed PTE
5. If overflow works → reclaim page → inject trust cache

**Fallback: SmartIO DMA (Strategy 2)**

If pt_desc is widened in iOS 27, pivot to SmartIO DMA write:
1. Reverse SmartIO IOKit driver on iPad Mini
2. Map RTKit mailbox interface
3. Craft DMA write to page-table physical address
4. Verify DART doesn't block the write

---

## 5. Sources

### Primary (academic/official)
- [arXiv: Modern iOS Security Features — SPTM, TXM, Exclaves](https://arxiv.org/pdf/2510.09272)
- [Apple Platform Security Guide](https://help.apple.com/pdf/security/en_US/apple-platform-security-guide.pdf)
- [Apple: Operating System Integrity](https://support.apple.com/guide/security/operating-system-integrity-sec8b776536b/web)
- [Apple: iOS 27.2 Security Content](https://support.apple.com/en-us/149230)

### Secondary (reverse engineering / community)
- [Rocket PPL/SPTM Bypass Writeup (iOS 17.0-17.3.1)](https://nvme0n1p.dev/en/posts/rocket-writeup)
- [Titan — SPTM Bypass iOS 16-17.3.1](https://github.com/alfiecg24/Titan)
- [silentIO — SmartIO DMA SPTM Bypass](https://github.com/ddddddedcds/silentIO)
- [Fentanyl — SPTM Bypass iOS 18.0-18.6.1](https://github.com/randomproxymaker/Fentanyl)
- [pmap-tte-remove — pt_desc Refcount Overflow](https://github.com/Indegosblade/pmap-tte-remove)
- [PPL/SPTM Bypasses — Apple Wiki](https://theapplewiki.com/wiki/PPL/SPTM_Bypasses)
- [SPTM & TXM — iOS Exploit Starterpack](https://tin-z.github.io/ios-exploit-starterpack/en/sptm-txm/)
- [sigreturn: SPTM, TXM and PPL](https://sigreturn.com/blog/sptm-txm-memory-tagging/)
- [sigreturn: iOS Code Signing Pipeline](https://sigreturn.com/blog/ios-code-signing-pipeline/)
- [Proteas: Quick Notes on SPTM](https://proteas.github.io/ios/2023/06/09/some-quick-and-discrete-notes-on-sptm.html)
- [iOS 27.0.1 SPTM Research](https://github.com/IvanIVGrozny/iOS27.0.1-SPTM-Research/)
- [iOS 27.0.1–27.2 Jailbreak Patch Analysis](https://github.com/Arch087146/iOS-27.0.1-27.2-jailbreak-patch-analysis)
- [iOS 27 A17 Pro Jailbreak Tracker](https://github.com/PalanBanana/ios27-a17pro-jailbreak-tracker)
- [asdfugil: SPTM Introduction Map](https://gist.github.com/asdfugil/101ae32734938cf2c186ff330fba1e97)
- [df-f: Tracing Back to the Source (SPTM)](https://df-f.com/blog/sptm3)
- [ironPeak: Bypassing Apple MIE](https://ironpeak.be/blog/bypassing-apple-mie/)
- [Cross-Cutting Attack Patterns Analysis](https://tin-z.github.io/ios-exploit-starterpack/en/attack-patterns/)
