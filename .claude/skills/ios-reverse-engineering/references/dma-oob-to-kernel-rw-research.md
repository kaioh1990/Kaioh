# Converting DMA OOB Write to Kernel R/W — Research Summary

## Context
We have a confirmed kernel panic from CVE-2026-112705 on iPhone Duo (iOS 27.1, A20 Pro).
The AVE coprocessor DMA overflows an output buffer, corrupting adjacent kernel heap memory.
DMA writes bypass MIE (ARM MTE) since they go through DART, not the CPU tag-check unit.

---

## 1. DMA Buffer Physical Memory Layout

### How DART Works
- DART (Device Address Resolution Table) is Apple's IOMMU for coprocessors
- It translates coprocessor I/O virtual addresses (IOVA) to physical addresses
- The kernel creates IODMACommand objects backed by IODARTMapper to map buffers
- DART maps scattered physical pages into contiguous IOVA ranges for each device
- When the AVE coprocessor writes beyond its mapped IOVA range, the DMA hits physical memory ADJACENT to the last mapped physical page

### Key Insight: Physical Adjacency
- DMA OOB writes corrupt physically adjacent memory, not virtually adjacent
- The kernel's zone allocator manages virtual memory, but the physical page backing those zones is what matters
- If page N is mapped as the last page of the AVE output buffer, the OOB writes to physical page N+1
- **Whatever kernel object occupies physical page N+1 gets corrupted**

### Historical Precedent: Project Zero Wi-Fi DMA (2017)
- Shaped IO-Space so DART translation tables were allocated after a DMA flow ring
- Overwrote DART descriptors via DMA to map arbitrary physical memory
- Once DART tables were corrupted, could DMA-read/write any physical address
- **This technique could apply to AVE's DART** — if we can position AVE's DART page tables adjacent to its DMA buffers

---

## 2. Heap Shaping for DMA Buffer Adjacency

### The Core Problem
DMA operates on physical addresses. We need to control what physical page is adjacent to the AVE output buffer's last page.

### Technique: Zone Garbage Collection + Page Reuse
1. **Exhaust a target zone** — allocate many objects of a specific size class until the zone allocator requests fresh physical pages from the page allocator
2. **Free strategic objects** — create holes in the zone's page usage
3. **Trigger zone GC** — the zone allocator returns underutilized pages to the general page pool
4. **Hope the page allocator gives that physical page to the DMA allocation** — this is probabilistic

### Technique: IOSurface Property Spray
- IOSurface properties (via `IOSurfaceSetValue`) allocate `OSData`/`OSString` in kalloc zones
- The size of the property data determines which kalloc zone (e.g., kalloc.64, kalloc.256)
- By spraying many IOSurface properties of a target size, you can fill a zone so that the allocator requests new physical pages
- These physical pages come from the same general pool that serves DMA allocations

### Technique: Page-Level Spray (Most Relevant for DMA)
- Instead of trying to control which kalloc objects land on a specific page, control which PAGES are allocated next to the DMA buffer
- Allocate many large contiguous physical regions (e.g., via IOSurface or mach_vm_allocate) to consume pages
- Free some, then trigger the DMA allocation — the freed pages get reused
- The DMA OOB then hits a page you previously controlled

### Critical Limitation: kalloc_type Zone Isolation (iOS 15+)
- Apple's `kalloc_type` randomized bucketed type isolation makes cross-type heap corruption harder
- Same-type objects share a zone bucket, but different types are separated
- **However**: DMA OOB operates at the PHYSICAL PAGE level, which is below zone isolation
- Zone isolation is a virtual-memory concept; physical pages can be recycled between any zones
- This means DMA OOB potentially bypasses kalloc_type isolation entirely

---

## 3. Target Kernel Objects for Data-Only Exploitation

### Under SPTM/TXM (iOS 17+, A15+)
SPTM monitors page table changes — you can't map new executable pages or modify page permissions.
TXM runs at EL2 — you can't patch it from EL1.
**But**: SPTM has no opinion on the VALUE of kernel data structures like ucred.

### ucred Structure (Primary Target)
- Contains `cr_uid`, `cr_gid`, process capabilities
- In XNU's RO zone (zone map RO region: 0xfffffe19a2000000 - 0xfffffe1c3c000000)
- **Complication**: On iOS 17+, ucred sits in read-only zones backed by SPTM
- One research source says the viable path is "PAC-preserving data modification"
- DarkSword (CVE-2025-43510/43520) demonstrated ucred patching on iOS 18 under PPL/SPTM
- Key: "data-only" exploitation — only modify data fields, not code pointers

### proc_t Structure
- BSD process structure containing task ports, file descriptors, credentials pointer
- Also in protected zones on modern iOS
- If you can overwrite the `p_ucred` pointer in proc_t to point to a crafted ucred, you get root

### Pipe Buffers (Read/Write Primitive)
- `pipe()` syscall allocates kernel buffer for read/write
- Historically used as kernel R/W primitive (SockPuppet, iOS 12.4)
- **Broken on iOS 15+**: dedicated pipe zone not in KHEAP_DATA_BUFFERS
- `kalloc_data_require` panics on cross-zone access
- **Not viable as a direct primitive on iOS 27**

### IOSurface Objects
- Used in physical UAF exploits for page reclamation
- Each IOSurface has user-controlled properties stored in kernel memory
- Can spray tagged values and scan for them after reclaiming a freed page
- **Most viable spray primitive** for heap shaping

### DART Page Tables (Nuclear Option)
- If we corrupt the AVE DART's own page tables via the DMA OOB...
- We could add new DART mappings that point to arbitrary physical addresses
- The AVE coprocessor could then DMA-read/write any physical memory
- This was Project Zero's Wi-Fi DMA technique (2017)
- **Unknown if AVE's DART tables are physically adjacent to its buffers**

---

## 4. Info Leak via AVE OOB Reads (CVE-2026-112466/112465)

### What We Know
- These are OOB READ vulnerabilities in the same AppleAVE2 driver
- Impact: "read kernel memory" through the AVE encoding pipeline
- Fixed in iOS 27.2 alongside the OOB writes

### Hypothesized Mechanism
- The AVE coprocessor reads input data via DMA from mapped physical pages
- If the read goes OOB (similar to the write OOB), it reads adjacent physical memory
- That data gets encoded by the AVE coprocessor
- The encoded output comes back through VideoToolbox as a compressed frame
- **The encoded frame may contain kernel data leaked from adjacent pages**
- Decoding the frame or analyzing the raw bitstream could reveal kernel addresses

### What This Gives Us
1. **KASLR bypass** — kernel text base address
2. **Heap layout** — physical addresses of kernel objects
3. **Zone map addresses** — locations of specific zones
4. **Information for heap shaping** — know where to position target objects

### How to Trigger
- Likely similar to the write OOB but with different parameters
- May use a different encoding mode or buffer configuration
- Need to reverse engineer which parameters trigger OOB reads vs writes
- The read might return encoded data containing kernel memory fragments

---

## 5. Prior DMA-Based Kernel Exploits on Apple

### Project Zero — Wi-Fi Chip DMA (2017)
- Exploited Broadcom Wi-Fi chip on iPhone 7
- Got code execution on Wi-Fi coprocessor
- Used DMA through DART to overwrite kernel BSS
- Shaped DART translation tables to be adjacent to DMA buffers
- Overwrote DART entries to gain arbitrary physical memory access
- **Most relevant prior art for our AVE DMA exploit**

### Project Zero — DCP Coprocessor (2022)
- Exploited Display Co-Processor (DCP) on Apple Silicon
- DCP makes RPC calls to AP that edit DART entries
- Fake C++ object stubs gave kernel read/write
- "Memory corruption privilege escalation that side-stepped kernel mitigations by corrupting memory on a co-processor"

### KeenLab — DART Permission Bug (pre-iOS 11)
- DART ignored read-only permission bits
- DMA mapping vulnerability gave R/W access regardless of permissions
- Fixed in iOS 11 — not applicable to iOS 27

### usbliter8 — USB DMA Underflow (2026, A12/A13)
- SecureROM bug, not kernel
- USB controller DMA pointer underflow
- A14+ configure DART correctly — not applicable to A20

### Operation Triangulation — Hardware MMIO (2023)
- Used undocumented hardware MMIO registers
- Bypassed hardware memory protection by writing to DMA registers directly
- Not a software bug — hardware backdoor

### Calif.io — AVE Chain (2026)
- Used CVE-2026-112705 and CVE-2026-114025 (both credited to them)
- "Data-only kernel local privilege escalation chain"
- "Ends with a root shell"
- Used on macOS 26.4.1 on M5 (similar MIE architecture)
- Full 55-page report NOT yet published (expected after Black Hat USA)
- **This is exactly our target chain — same bugs, same approach**
- Key difference: they had CVE-2026-114025 (arbitrary code execution) which may be a better primitive

### DarkSword — Data-Only Chain (2025/2026)
- Six-vulnerability chain for iOS 17-18
- CVE-2025-43510: COW mismap in vm_map → kernel R/W
- CVE-2025-43520: privilege escalation
- Data-only: patches ucred fields without introducing new code
- Works under PPL/SPTM because "SPTM has no opinion on cr_uid value"
- **Proves data-only ucred overwrite works under SPTM**

---

## 6. Proposed Exploit Strategy

### Phase 1: Info Leak (KASLR Bypass)
1. Trigger CVE-2026-112466/112465 (AVE OOB reads) to leak kernel addresses
2. Extract KASLR slide from leaked data
3. Map the kernel's zone layout from leaked pointers

### Phase 2: Heap Shaping
1. Spray IOSurface properties to fill target zones and consume physical pages
2. Free strategic allocations to create physical page holes
3. Trigger AVE DMA allocation — the output buffer should land adjacent to our controlled pages
4. Alternatively: try to position kernel credential/process structures on pages adjacent to AVE buffers

### Phase 3: Controlled Write
**Option A — Direct Object Corruption:**
1. Position ucred or proc structures on the page after the AVE output buffer
2. Trigger the DMA OOB write with controlled temporal layer count
3. The OOB corrupts specific fields in the adjacent structure
4. Need to understand exactly what bytes the OOB writes (encoded video data pattern)

**Option B — DART Table Corruption (more powerful):**
1. Position AVE's DART page tables adjacent to the DMA output buffer
2. DMA OOB overwrites DART entries
3. Add new DART mappings pointing to arbitrary physical addresses
4. Use AVE coprocessor to DMA read/write any physical memory
5. This gives arbitrary kernel R/W

**Option C — CVE-2026-114025 (arbitrary code execution):**
1. This CVE is described as giving "arbitrary code execution" (not just system termination)
2. It has a different fix ("input validation" vs "bounds checking")
3. It may provide a more controllable write primitive than the DoS-grade OOB
4. Need to reverse engineer what input validation CVE-2026-114025 adds in iOS 27.2

### Phase 4: Privilege Escalation
1. With kernel R/W, locate current process's proc_t
2. Find the p_ucred pointer
3. Either: overwrite cr_uid/cr_gid to 0 (root)
4. Or: replace p_ucred pointer with a pointer to kernel's init_proc ucred
5. Data-only — no code execution needed, SPTM doesn't block this

### Phase 5: Code Signing Bypass
1. With root + kernel R/W, modify trust cache or set CS_DEBUGGED
2. Or use TXM trust cache injection if accessible
3. This allows loading unsigned code (Sileo/Procursus)

---

## 7. Key Open Questions

1. **What exactly does the DMA OOB write?** The overflowed data is encoded video output — is it controllable? Or is it essentially random compressed data? Understanding the bitstream pattern is critical for Option A.

2. **Can we control the OOB length?** Different NumberOfTemporalLayers values may give different overflow sizes. 16463 gives a panic; can we find a value that overflows by exactly 8 or 16 bytes for surgical corruption?

3. **Where are AVE's DART page tables in physical memory?** If they're adjacent to the DMA buffers, Option B (DART corruption) is viable.

4. **What does CVE-2026-114025 trigger?** Different from CVE-2026-112705's bounds checking fix — the "input validation" fix might protect a different code path that gives more control.

5. **Can the OOB reads (CVE-2026-112466) be triggered without crashing?** We need a survivable info leak, not another panic.

6. **Are ucred structures still in RO zones on iOS 27.1?** If so, we need an alternative target or must modify the zone's page permissions via SPTM bypass.

---

## Sources
- [Project Zero — Over The Air Wi-Fi Exploiting (2017)](https://googleprojectzero.blogspot.com/2017/10/)
- [Project Zero — The Core of Apple is PPL (2020)](https://projectzero.google/2020/07/the-core-of-apple-is-ppl-breaking-xnu.html)
- [Project Zero — DCP Exploitation (2022)](https://googleprojectzero.blogspot.com/2022/06/)
- [Project Zero — Survey of iOS Kernel Exploits (2020)](https://projectzero.google/2020/06/a-survey-of-recent-ios-kernel-exploits.html)
- [8kSec — DarkSword Kernel Escalation](https://www.8ksec.io/darksword-kernel-escalation-cve-2025-43510-43520/)
- [Corellium — UNIX Pipes for iOS Kernel Primitives](https://www.corellium.com/blog/exploring-unix-pipes-for-ios-kernel-exploit-primitives)
- [Azeria Labs — Grooming the iOS Kernel Heap](https://azeria-labs.com/grooming-the-ios-kernel-heap/)
- [Secfault — Writing an iOS Kernel Exploit from Scratch](https://secfault-security.com/blog/chain3.html)
- [Apple — kalloc_type Design](https://security.apple.com/blog/towards-the-next-generation-of-xnu-memory-safety/)
- [Alfie CG — Step-by-Step iOS Kernel Exploit](https://alfiecg.uk/2024/09/24/Kernel-exploit.html)
- [HackTricks — iOS Physical UAF IOSurface](https://hacktricks.wiki/en/binary-exploitation/ios-exploiting/ios-physical-uaf-iosurface.html)
- [KeenLab — iOS Jailbreak Internals (Black Hat 2018)](https://i.blackhat.com/us-18/Wed-August-8/us-18-Chen-KeenLab-iOS-Jailbreak-Internals.pdf)
- [Calif.io MIE Challenge (X/Twitter)](https://x.com/calif_io/status/2081851654631215596)
- [ironPeak — Pardon MIE?](https://ironpeak.be/blog/bypassing-apple-mie/)
- [Modern iOS Security Features (arXiv)](https://arxiv.org/pdf/2510.09272)
- [SPTM/TXM iOS 17+ Mitigations](https://tin-z.github.io/ios-exploit-starterpack/en/sptm-txm/)
- [MchinaMd57764 — AVETemporalPoC](https://github.com/MchinaMd57764/AVETemporalPoC-CVE-2026-112705)
- [Lak453 — CVE-2026-112705-poc](https://github.com/Lak453/CVE-2026-112705-poc)
