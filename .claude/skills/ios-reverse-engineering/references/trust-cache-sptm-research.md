# Trust Cache Injection Under SPTM — iOS 27 Research

## Date: 2026-11-21

## 1. The Problem

After achieving kernel R/W + root (ucred cr_uid=0), we still can't run unsigned
binaries. iOS code signing is enforced by AMFI and, on SPTM devices, by TXM
(Trust eXecution Monitor). We need to either:
- Inject entries into the trust cache (traditional approach), OR
- Bypass code signing enforcement entirely

## 2. Trust Cache Architecture on iOS 27

### Trust Cache Types
- **Static trust cache**: Baked into the firmware, contains CDHashes for all
  Apple-shipped binaries. Read-only, cannot be modified at runtime.
- **Loadable (dynamic) trust cache**: Loaded at boot from personalized Cryptex
  images. Can theoretically be extended at runtime.
- **Engineering trust cache**: Only on internal/development devices.

### Trust Cache Structure
Each entry contains a CDHash (20-byte SHA-1 or 32-byte SHA-256 truncated hash
of a binary's code directory). AMFI checks incoming code pages against all
loaded trust caches before allowing execution.

### Pre-SPTM Injection (iOS 16 and older)
1. Gain kernel R/W
2. Find `loadedTrustCaches` linked list head in kernel data segment
3. Allocate kernel memory for a new trust_cache_entry
4. Fill with CDHashes of binaries to run
5. Link at head of list
6. AMFI reads the list → allows execution

This worked because trust cache management ran in the kernel's address space.

## 3. SPTM/TXM Changes — THE BLOCKER

### What Changed (CONFIRMED from academic paper + Apple docs)
On A15+ (iOS 17+) and A13-A14/M1 (iOS 27+), trust cache management moved
from the kernel to TXM:

- **TXM runs in a separate trust domain** from the kernel, at a higher
  privilege level under SPTM
- **Kernel R/W cannot modify TXM state**: the kernel cannot read or write TXM
  memory, page tables, or trust cache data structures
- **Trust cache injection via kernel R/W is BLOCKED**: even with full kernel
  read/write, you cannot manipulate the trust cache because TXM owns it
- **SPTM enforces page table integrity**: the kernel cannot remap TXM pages
  or change their permissions

### Architecture
```
EL3: SPTM (Secure Page Table Monitor)
  └── Owns page tables, enforces memory isolation
EL1-S: TXM (Trust eXecution Monitor)  
  └── Manages trust caches, code signing, entitlements
  └── Separate address space from kernel
EL1: XNU kernel
  └── Has R/W over its own data, but NOT TXM data
  └── Can request TXM operations via SPTM dispatch
EL0: Userspace
```

### Implication
**A kernel exploit alone is NOT enough for code signing bypass on SPTM devices.**
You need EITHER:
1. A TXM vulnerability (to corrupt trust cache state)
2. An SPTM vulnerability (to bypass memory isolation)
3. A bypass that avoids the trust cache entirely

Sources: [arXiv 2510.09272 - Steffin & Classen](https://arxiv.org/pdf/2510.09272v1),
[Apple Platform Security Guide](https://support.apple.com/guide/security/sec8b776536b),
[ios-exploit-starterpack](https://34306.github.io/ios-exploit-starterpack/en/sptm-txm/)

## 4. Known Approaches for Code Signing Bypass Under SPTM

### A. SPTM/TXM Vulnerability (Hardest, Most Powerful)
- **Requirement**: Find a bug in SPTM or TXM firmware
- **Result**: Full trust cache control, potentially persistent
- **Status**: Calif.io's untethered jailbreak reportedly includes an SPTM bypass,
  but no details published. The Apple Wiki lists "Chollima" jailbreak components
  as including "an untether exploit, a sandbox escape, a kernel exploit and an
  SPTM bypass" — UNCONFIRMED.
- **Our feasibility**: Very low without dedicated SPTM/TXM reversing

### B. CryptSn0w's Cryptex + dyld Approach (PATCHED)
- **Technique**: Ship jailbreak as a device-personalized Cryptex (disk image with
  trust cache and Apple signing ticket). Loader modified dyld's signature checks
  and copied payload code into anonymous memory, so native code ran without its
  hash in the fixed trust cache.
- **Status**: Patched server-side by Apple. No longer works. Source released by
  0xjohnny for research only.
- **Our feasibility**: Zero — server-side fix means the signing ticket approach
  is dead.

Source: [PBX Science - CryptSn0w Source](https://pbxscience.com/?p=24550)

### C. CS_DEBUGGED Flag (Partial Bypass)
- **Technique**: Set CS_DEBUGGED (0x10000000) on a process's p_csflags. This
  allows the process to run with invalid code pages.
- **How it's set**: Via `cs_allow_invalid()` in XNU, which checks AMFI policy
  first (proc_lock → clear CS_KILL|CS_HARD → if CS_VALID, set CS_DEBUGGED)
- **SPTM complication**: On SPTM devices, the AMFI policy check likely gates
  through TXM. Setting p_csflags directly via kernel R/W may work IF the check
  is purely in-kernel, but TXM may validate the flags independently.
- **Our feasibility**: MEDIUM — worth testing. If we can set CS_DEBUGGED via
  kernel R/W and AMFI/TXM doesn't independently validate, our process can load
  unsigned code. The flag is in kernel memory (proc structure), not TXM memory.

### D. DarkSword-Style Data-Only Approach (Most Promising for Us)
- **Key insight from 8kSec**: "PPL and SPTM don't stop an exploit chain that
  never introduces native code and instead performs a series of writes against
  existing policy state."
- **Technique**: Instead of injecting new code, modify existing policy/data to
  achieve the goal. For code signing:
  1. Modify AMFI's in-kernel policy state to allow our binaries
  2. Or: modify the process's credential/entitlement data
  3. Or: use existing signed code (like a debugserver or developer tool) as a
     springboard
- **Our feasibility**: HIGH for limited scope. We may not need to run arbitrary
  unsigned binaries — if we can find a signed binary that can install packages
  (or modify an existing one's behavior through data-only changes), we bypass
  the trust cache problem entirely.

Source: [8kSec - DarkSword Analysis](https://8ksec.io/darksword-kernel-escalation-cve-2025-43510-43520/)

### E. amfi_allow_any_signature Boot Argument
- **Technique**: Set the `amfi_allow_any_signature` boot-arg
- **Status**: Only works on development/reduced-security devices. Production
  devices ignore this boot argument.
- **Our feasibility**: Zero on production iPhone Duo

### F. doublera1n / Custom Firmware (A13 Only)
- **Technique**: Boot custom iBoot + patched kernel from DFU mode, install
  rootless jailbreak on top
- **Requires**: BootROM exploit (usbliter8 for A12/A13) + RP2350 hardware
- **Our feasibility**: Zero for A20 Pro (no BootROM exploit)

Source: [CyberPress - doublera1n](https://cybersecuritynews.com/booted-jailbroken-ios-27-iphone-11-pro/)

## 5. Recommended Strategy for Our Chain

Given that trust cache injection is BLOCKED by TXM on SPTM devices, our best
options in order of feasibility:

### Option 1: CS_DEBUGGED + Process Injection (Test First)
1. Achieve kernel R/W (via DMA OOB → pipe corruption)
2. Find our process's proc_t in kernel memory
3. Set CS_DEBUGGED flag in p_csflags via kernel write
4. Attempt to load unsigned code (dlopen a custom dylib, or mmap+execute)
5. If AMFI/TXM blocks this independently → move to Option 2

**Test**: Write a probe that gets kernel R/W, sets CS_DEBUGGED on itself,
then tries to execute a code page with known-invalid signature.

### Option 2: Data-Only Bootstrap Installation
Instead of running unsigned binaries directly:
1. Achieve root (ucred cr_uid=0)
2. Use kernel R/W to modify filesystem mount flags (remove read-only)
3. Write Procursus/Sileo files to disk using kernel write primitive
4. Compute CDHashes of all written binaries
5. Find a way to get those hashes trusted:
   a. If CS_DEBUGGED works → set it on launchd or SpringBoard
   b. If not → look for TXM interaction surfaces exploitable from kernel

### Option 3: Cryptex-Based Approach (Research Needed)
CryptSn0w's approach (personalized Cryptex + dyld modification) was patched
server-side, BUT the concept of loading a trust cache via Cryptex is legitimate.
If we can:
1. Create a valid Cryptex with our binaries' CDHashes
2. Get it loaded at boot (requires persistence mechanism)
3. The trust cache in the Cryptex would be loaded by TXM legitimately

This requires understanding the Cryptex personalization and loading flow,
which is complex but potentially viable if we have root + persistence.

### Option 4: TXM/SPTM Vulnerability (Last Resort)
If all above fail, we'd need to find a vulnerability in TXM or SPTM itself.
This is the hardest path and requires dedicated firmware reversing.

## 6. What Calif.io Likely Did

Based on available evidence (SPECULATIVE):
- Their jailbreak reportedly includes an SPTM bypass
- This means they likely found a vulnerability in SPTM or TXM
- With an SPTM bypass, trust cache injection becomes possible again
- Their untether mechanism likely loads a trust cache at boot via the bypass
- They have NOT published the SPTM bypass details
- Apple fixed "most flaws" in iOS 27.2 but Calif.io hasn't released a tool

## 7. Honest Assessment

### CONFIRMED:
- Trust cache injection via kernel R/W is blocked by TXM on SPTM devices
- SPTM/TXM architecture separates trust cache management from kernel
- CryptSn0w's Cryptex approach was patched server-side
- No public, working trust cache injection technique exists for iOS 27 on A15+

### LIKELY:
- CS_DEBUGGED flag is in kernel memory (proc structure), potentially modifiable
  via kernel R/W even on SPTM devices
- DarkSword-style data-only exploitation can bypass SOME SPTM protections
- Calif.io found an SPTM/TXM vulnerability for their jailbreak

### UNKNOWN:
- Whether CS_DEBUGGED actually allows unsigned code execution on iOS 27 with SPTM
  (TXM may independently validate code signatures regardless of CS_DEBUGGED)
- Whether there's a TXM interaction surface exploitable from kernel context
- Whether Cryptex loading can be abused with root access

### BIGGEST RISK:
The trust cache / code signing step is the HARDEST part of the chain on SPTM
devices. Everything before it (DMA OOB → heap corruption → kernel R/W → root)
is achievable with known techniques. But getting unsigned code to actually
execute may require an SPTM/TXM vulnerability we don't have.

**Mitigation**: Test CS_DEBUGGED first. If it works, the chain is viable.
If not, we may need to pivot to a data-only approach that doesn't require
running new binaries (e.g., modifying existing system binaries' behavior
through configuration/data changes rather than code replacement).
