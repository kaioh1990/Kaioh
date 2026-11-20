# A20 Pro MIE Bypass Research — iPhone Duo iOS 27.1

## Date: November 20, 2026
## Target: iPhone Duo (iPhone19,4 / V68AP), A20 Pro (T8160), iOS 27.1 (24A94425)

---

## 1. MIE (Memory Integrity Enforcement) Overview

MIE is Apple's always-on memory tagging on A19+/M5+ chips. Key properties:

- Uses ARM Enhanced Memory Tagging Extension (EMTE) in **synchronous mode**
- Tags 70+ system processes including kernel heap (kalloc)
- 4-bit tags → 16 possible values per allocation
- Tag mismatch on load/store → **immediate fault** (synchronous mode)
- SPTM protects the tag storage itself
- **CONFIRMED on A20**: Our Brotli OOB test proved MIE catches cross-boundary writes

### What MIE Does NOT Protect

1. **DMA writes through DART** — coprocessor DMA bypasses CPU load/store unit entirely
2. **Trusted writer argument validation** — the _zalloc_ro_mut code path itself
3. **Data-only attacks within properly tagged allocations** — modifying values, not crossing boundaries
4. **Speculative execution side channels** — tags can be leaked via TIKTAG technique
5. **Probabilistic tag reuse** — 1/16 chance freed memory gets same tag on reallocation

---

## 2. MIE Bypass Techniques

### A. DMA-Based Bypass (HIGHEST CONFIDENCE)

**Mechanism:** ARM MTE tag checks happen only in the CPU's load/store unit.
DMA transfers through DART (Apple's IOMMU) do NOT perform tag checks.
Any coprocessor that can perform DMA to kernel memory bypasses MIE entirely.

**Confirmed by:**
- [Project Zero CVE-2025-0072 writeup](https://github.blog/security/vulnerability-research/bypassing-mte-with-cve-2025-0072/) — GPU backing page bypass on Pixel 8
- [HackTricks MTE page](https://hacktricks.wiki/en/binary-exploitation/common-binary-protections-and-bypasses/memory-tagging-extension-mte.html)
- Our own Brotli test: Phase 1 (allocation) succeeded, Phase 3 (CPU OOB write) faulted

**Applicable bugs on iOS 27.1:**
- CVE-2026-112705 (AVEVideoEncoder): `dart-ave` PTE-invalid fault = DMA OOB write
- SmartIO coprocessor DMA (silentIO technique): 0 PAC instructions confirmed on A17 Pro

### B. Trusted Writer Validation Bypass (Calif's Approach)

**Mechanism:** `_zalloc_ro_mut()` is the sole gatekeeper for modifying read-only
kernel memory. It runs at EL1 with special SPTM authorization. An integer overflow
in its argument validation lets attacker bytes spill across slot boundaries in the
read-only zone. MIE can't protect the validation logic itself.

**ironPeak analysis:** "The bug was in the one place MIE can't protect: the argument
validation of the trusted writer itself."

**CVE-2026-28952:** Integer overflow in _zalloc_ro_mut stack-area filter.
- Fixed in macOS 26.5, iOS 18.7.9
- Status on iOS 27.1: **UNCLEAR** — not listed in iOS 27.x notes
- Need to verify by diffing kernelcache

### C. Probabilistic Tag Reuse (1/16 Per Attempt)

**Mechanism:** With 4-bit tags, a freed allocation has a 1/16 chance of getting
the same tag when reallocated at the same size. A stale pointer with the matching
tag bypasses MIE.

**Applicability:** Can be used to make IOSurface race (CVE-2026-112477) work under MIE.
Need ~16 attempts on average. Each failed attempt triggers a fault (potential crash),
but if the fault is recoverable (not a kernel panic), iterating is feasible.

**Risk:** Each tag mismatch is a synchronous fault. In kernel context, this may be
a kernel panic. Need to verify fault handling behavior.

### D. Speculative Tag Leak (TIKTAG)

**Mechanism:** TIKTAG (USENIX Security 2025) uses speculative execution to leak
memory tags via cache side channels. Reported 95-97% success rate in kernel UAF PoC.

**Applicability:** Could learn the tag of a target allocation, then craft the
replacement allocation to match. Makes UAF exploitation deterministic rather than
probabilistic.

**Risk:** Requires a cache side channel from userspace to kernel. Apple may have
mitigated this on A20 with speculation barriers.

**Sources:**
- [TIKTAG paper (arXiv)](https://arxiv.org/html/2406.08719v1)
- [TIKTAG PDF (Kim et al.)](https://taesoo.kim/pubs/2025/kim:tiktag-sp.pdf)

---

## 3. CVE-2026-112705 — AVEVideoEncoder OOB Write (Primary Entry)

### Bug Description

Out-of-bounds write in Apple's AVEVideoEncoder (AppleAVE2 driver), reachable
via public VideoToolbox API. Two known trigger paths:

**Trigger 1 — Resolution overflow (Lak453 PoC):**
- Create VTCompressionSession for H.264 at 32768 x 16384
- BGRA pixel format, no IOSurface attributes
- Submit one frame → kernel panic
- Panic signature: `dart-ave` PTE-invalid write fault
- The oversized resolution causes the AVE coprocessor to DMA-write past its DART mapping

**Trigger 2 — Temporal layer count (MchinaMd57764 PoC):**
- Set NumberOfTemporalLayers=16463 via VideoToolbox property
- Submit encode → kernel data abort
- The out-of-range value causes the driver to calculate wrong buffer offsets

### Why This Is MIE-Safe

The `dart-ave` PTE-invalid fault means the OOB write occurs through the AVE
coprocessor's DMA engine, not through CPU load/store. DMA writes bypass MIE
tag checks entirely. The CPU never performs the write — the AVE hardware does.

### Current Assessment

Public PoCs are crash-only. The write is described as "fixed-offset constant fill."
Key unknowns:
1. Can we control the write destination? (depends on DART mapping layout)
2. Can we control the write content? (the pixel data we set)
3. What adjacent structures exist past the DART mapping boundary?

### Historical Precedent

CVE-2019-8795 (AppleAVE2, iOS 13): Out-of-bounds processing led to controlled
virtual method call or arbitrary kfree(). That bug was successfully weaponized
into a kernel exploit. Source: [Project Zero survey](https://projectzero.google/2020/06/a-survey-of-recent-ios-kernel-exploits.html)

### Weaponization Strategy

1. **Analyze DART mapping layout** for AppleAVE2 on A20/iOS 27.1
   - Extract from kernelcache: AVE driver's IOKit class hierarchy
   - Map the DART aperture boundaries
   - Determine what kernel structures are adjacent

2. **Control the write** via pixel buffer content
   - The PoC fills the buffer with 0x80 — but we can fill with arbitrary data
   - If the DMA write destination is predictable, we control both content and location
   - 32768 x 16384 x 4 bytes = 2GB of pixel data to control

3. **Target useful structures** past the DART boundary
   - IOKit object vtables
   - kext metadata
   - Mach port objects
   - Page table entries (if adjacent to DART aperture)

---

## 4. IOSurface Race Under MIE (Secondary Path)

CVE-2026-112477 (IOSurface race) is available on iOS 27.1.

### MIE Impact on UAF Exploitation

Traditional UAF flow:
1. Object A allocated with tag T1
2. Object A freed → memory gets new tag T2
3. Spray to reclaim with Object B (tag T2)
4. Access via dangling pointer (tag T1) → **MIE FAULT** (T1 != T2)

### Mitigation Strategies

**Strategy 1: Probabilistic (1/16)**
- Attempt the race + UAF exploitation
- 1/16 chance the replacement gets the same tag
- Each failure may panic — need to verify fault handling
- Average ~16 attempts if each failure is recoverable

**Strategy 2: TIKTAG tag leak**
- Leak the target allocation's tag speculatively
- Allocate replacement with matching tag
- Makes exploitation deterministic
- Requires cache side channel (may be mitigated on A20)

**Strategy 3: Same-zone reuse**
- Allocations in the same kalloc zone may reuse tags more often
- If the allocator cycles through tags, knowing the freed tag helps
- Need to study Apple's tag assignment algorithm

---

## 5. SmartIO Firmware on A20 (Verification Needed)

The silentIO SPTM bypass requires SmartIO firmware with 0 PAC instructions.
**CONFIRMED on A17 Pro (iPad Mini)**. Need to verify on A20 Pro (iPhone Duo).

### Verification Steps

1. Download iPhone Duo IPSW from Apple
   - Device: iPhone19,4 (V68AP)
   - Build: 24A94425 (iOS 27.1)
2. Extract SmartIOFirmware_ASCv6.im4p from BuildManifest
3. Run PAC audit: `audit_rtkit_pac.py`
4. If 0 PAC → silentIO-style bypass is viable on A20

### Why SmartIO DMA Bypasses Both SPTM AND MIE

- SPTM: Monitors CPU page table writes. DMA goes through DART, not SPTM.
- MIE: Tags checked in CPU load/store unit. DMA bypass the CPU entirely.
- Double bypass: One coprocessor DMA write defeats both protections.

---

## 6. Recommended Chain for A20 / iOS 27.1

### Option A: Pure DMA Chain (Highest Confidence)

```
AVEVideoEncoder OOB (CVE-2026-112705)
  → DMA-based kernel corruption (MIE-safe)
  → Controlled kernel R/W via corrupted driver structures
  → SmartIO DMA write (silentIO) → self-referencing PTE
  → Map TXM memory → inject trust cache
  → Procursus + Sileo
```

Confidence: Medium-High. Both entry (AVE) and bypass (SmartIO) are DMA-based.
MIE never enters the picture.

### Option B: Probabilistic UAF Chain

```
IOSurface race (CVE-2026-112477)
  → UAF with probabilistic tag match (1/16, MIE-aware)
  → Kernel R/W via reclaimed object
  → SmartIO DMA write → self-referencing PTE
  → Trust cache injection → Sileo
```

Confidence: Medium. Requires ~16 attempts average. Each failure may panic.

### Option C: Trusted Writer Chain (Calif-style)

```
_zalloc_ro_mut overflow (CVE-2026-28952 or variant)
  → Write to __DATA_CONST (MIE-safe: trusted writer bypass)
  → Modify kernel control data
  → SmartIO DMA write → SPTM bypass
  → Trust cache injection → Sileo
```

Confidence: Low-Medium. Need to verify if bug exists on iOS 27.1.

---

## 7. Immediate Next Steps

1. **Extract iPhone Duo IPSW kernelcache** — verify SmartIO firmware on A20
2. **Compile AVEVideoEncoder PoC** for arm64e and test on iPhone Duo via USB
3. **Pull crash logs** from MIE fault (Brotli test) and AVE test
4. **Analyze AVE driver** in kernelcache — DART mapping layout, adjacent structures
5. **Research _zalloc_ro_mut** on iOS 27.1 — is CVE-2026-28952 patched?
6. **Study Apple's MTE tag assignment** — understand tag cycling/reuse patterns

---

## Sources

### MIE/MTE Bypass
- [Project Zero: Bypassing MTE with CVE-2025-0072](https://github.blog/security/vulnerability-research/bypassing-mte-with-cve-2025-0072/)
- [TIKTAG: Breaking ARM MTE with Speculative Execution](https://arxiv.org/html/2406.08719v1)
- [HackTricks: MTE](https://hacktricks.wiki/en/binary-exploitation/common-binary-protections-and-bypasses/memory-tagging-extension-mte.html)
- [MTE-Aware Exploits](https://androidmobiles.org/developing-android-mte-aware-exploits-strategies-for-effective-memory-attacks/)

### AVEVideoEncoder (CVE-2026-112705)
- [Lak453 PoC](https://github.com/Lak453/CVE-2026-112705-poc)
- [Apple iOS 27.2 Security Content](https://support.apple.com/en-us/149230)
- [Project Zero: iOS Kernel Exploit Survey](https://projectzero.google/2020/06/a-survey-of-recent-ios-kernel-exploits.html)

### Calif MIE Bypass
- [ironPeak: Pardon MIE?](https://ironpeak.be/blog/bypassing-apple-mie/)
- [MacRumors: iPhone Duo Jailbroken](https://www.macrumors.com/2026/10/23/iphone-duo-jailbreak/)
- [Michael Tsai: MIE Exploit](https://mjtsai.com/blog/2026/05/18/memory-integrity-enforcement-exploit/)
- [Tom's Hardware: M5 MIE Bypass](https://www.tomshardware.com/tech-industry/cyber-security/apple-m5-architecture-suffers-first-privilege-escalation-exploit-anthropics-claude-mythos-helps-researchers-bypass-memory-integrity-enforcement)
- [iTnews: Apple patches MIE jailbreak](https://www.itnews.com.au/news/apple-rains-on-mie-bypassing-iphone-jailbreak-parade-with-ios-update-629978)
