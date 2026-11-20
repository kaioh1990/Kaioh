# iOS 27 Kernel Vulnerabilities (Present in 27.0.1 / 27.1)

## Overview

iOS 27.2 patched multiple kernel-level CVEs that are present in iOS 27.0.1 and 27.1. These are candidates for step 4 (kernel exploit) of our jailbreak chain. All research is educational, targeting devices we own.

## Kernel CVEs Patched in iOS 27.2

### CVE-2026-112507 — Kernel OOB Write
- **Type**: Out-of-bounds write
- **Component**: XNU kernel (memory management)
- **Primitive**: Controlled write past allocation boundary
- **Relevance**: HIGH — OOB writes in the kernel can corrupt adjacent objects to gain arbitrary kernel R/W. Similar pattern to our userspace CVE-2026-86950 but at kernel privilege level.

### CVE-2026-65350 — Use-After-Free
- **Type**: Use-after-free
- **Component**: Kernel object lifecycle
- **Primitive**: Dangling pointer → type confusion → arbitrary R/W
- **Relevance**: HIGH — UAFs are the most common kernel exploit primitive in modern iOS jailbreaks. The pattern is: free object A, reallocate the memory as object B (different type), trigger access through dangling pointer to A → type confusion gives controlled field access.

### CVE-2026-112477 — IOSurface Race Condition
- **Type**: Race condition (TOCTOU)
- **Component**: IOSurface (GPU/display surface management)
- **Primitive**: Double-free or UAF from concurrent access
- **Relevance**: HIGH — IOSurface has been a repeated kernel attack surface. It's reachable from the app sandbox and handles complex shared memory operations. Previous IOSurface exploits (e.g., CVE-2023-32434 used in Operation Triangulation) provided reliable kernel R/W.

### CVE-2026-113910 — Type Confusion / Info Disclosure
- **Type**: Type confusion leading to information disclosure
- **Component**: Kernel IPC / Mach subsystem (likely)
- **Primitive**: Kernel ASLR defeat (kslide leak) + potential for escalation
- **Relevance**: MEDIUM-HIGH — Info disclosure alone doesn't give code execution but is essential for defeating KASLR. Combined with another bug, this enables precise targeting of kernel data structures.

### CVE-2026-84495
- **Component**: Kernel
- **Details**: Limited public information
- **Patched in**: iOS 27.2

### CVE-2026-112474
- **Component**: Kernel
- **Details**: Limited public information
- **Patched in**: iOS 27.2

### CVE-2026-112679
- **Component**: Kernel
- **Details**: Limited public information
- **Patched in**: iOS 27.2

### CVE-2026-112517
- **Component**: Kernel
- **Details**: Limited public information
- **Patched in**: iOS 27.2

### CVE-2026-112514
- **Component**: Kernel
- **Details**: Limited public information
- **Patched in**: iOS 27.2

## Calif / Hexacon 2026 Jailbreak

- **Who**: Calif (calif.io)
- **Event**: Hexacon 2026 (June 2026)
- **Target**: iOS 27.0.1
- **Achievement**: Untethered jailbreak demonstrated
- **Status**: Kernel exploit writeup delayed — the team discovered new 0-day findings during the drafting process
- **Public exploit**: NOT released
- **Implication**: An untethered jailbreak means they achieved: kernel R/W + PAC bypass + PPL bypass + persistence across reboot. The specific CVEs used are not disclosed.

## Related: DarkSword Chain

Google TAG analyzed the DarkSword full-chain targeting iOS 18+:
- Kernel: CVE-2025-43510 and CVE-2025-43520
- PAC/TPRO bypass: CVE-2026-20700
- Patched in iOS 26.1 (kernel) and 26.3 (full chain)
- Demonstrates that state actors maintain iOS kernel exploit capability through each major version

## Recommended Kernel Exploit Candidates

For our chain targeting iOS 27.0.1 on iPad Mini:

### Tier 1 (Most Promising)
1. **CVE-2026-65350 (UAF)** — UAFs are the workhorse of kernel exploitation. Standard technique: kalloc spray + zone confusion + dangling pointer reuse.
2. **CVE-2026-112477 (IOSurface race)** — IOSurface is sandbox-reachable and has well-understood exploitation patterns from prior art.

### Tier 2 (Useful Supporting Bugs)
3. **CVE-2026-113910 (info disclosure)** — KASLR bypass to locate kernel objects precisely.
4. **CVE-2026-112507 (OOB write)** — If the OOB is controllable, this is as good as a UAF.

### Tier 3 (Unknown)
5. The remaining CVEs with limited public info may be useful once details emerge.

## What the Kernel Exploit Must Achieve

```
Step 4a: Kernel info leak (defeat KASLR)
   → Know where kernel objects/code live in memory
   
Step 4b: Kernel arbitrary R/W
   → Read/write any kernel virtual address
   
Step 4c: Credential escalation
   → Modify our process's ucred to uid=0 (root)
   → Modify our process's sandbox profile
   
Step 4d: Trust cache injection
   → Add our binaries' CDHashes to the kernel trust cache
   → Allows our unsigned code to execute past AMFI
```

## Exploitation Technique Patterns

### UAF → Arbitrary R/W (Standard Pattern)
```
1. Trigger UAF — free target kernel object (e.g., via IOSurface race)
2. Heap spray — reallocate freed memory with controlled data (pipe buffers, IOKit user clients)
3. Dangling access — use stale pointer to read/write through the replacement object
4. Fake object — spray a fake object whose fields we control
5. Read primitive — use the fake object's vtable/function pointer to leak kernel data
6. Write primitive — use the fake object to overwrite kernel data structures
```

### Kernel R/W → Full Jailbreak
```
1. Leak kernel slide (KASLR defeat)
2. Find our task_struct → proc → ucred
3. Overwrite ucred: uid=0, gid=0, clear sandbox
4. Find the trust cache linked list in kernel memory
5. Inject CDHashes for our bootstrap binaries
6. For persistence: patch kernel text (requires KTRR bypass) or use rootless approach
```

## Post-Kernel: PAC and PPL Bypass

### PAC (Pointer Authentication Codes)
- ARM64e (A12+, all our test devices) signs code and data pointers
- Kernel PAC keys are per-process
- Bypass approaches:
  - **Signing gadgets**: Find kernel code that signs arbitrary pointers (e.g., `pacia` in a controllable context)
  - **Key extraction**: Leak the PAC key via kernel R/W → sign pointers in userspace
  - **PAC-less paths**: Some kernel paths don't check PAC (legacy compatibility)

### PPL (Page Protection Layer)
- Separates page table management from the kernel
- Even with kernel R/W, can't modify page tables directly
- Bypass approaches:
  - **PPL entry points**: Find PPL functions that can be called with controlled arguments
  - **APRR register manipulation**: Modify the APRR (Access Permission Restriction Register) state
  - **Existing PPL bypass CVEs**: PPL bypasses are rare but have occurred (CVE-2026-20700 in DarkSword)

## Next Steps

1. Develop userspace exploit chain (CVE-2026-86950 → arbitrary R/W in renderer process)
2. Research sandbox escape from PDF renderer context
3. Investigate CVE-2026-65350 and CVE-2026-112477 for kernel exploitation
4. Study Calif's approach once their writeup publishes
