# Sileo Bootstrap & Rootless Jailbreak Chain Research

## Status: Research Complete (Dopamine 3.x Architecture Study)

## Overview

This document maps the full path from "kernel exploit achieved" to "Sileo running with package management," based on a source-code study of [opa334/Dopamine](https://github.com/opa334/Dopamine) (7382 stars), the state-of-the-art rootless semi-untethered jailbreak for iOS 15–26.0.1 (arm64e) and iOS 15–18.7.1 (arm64).

The goal is to understand what additional primitives, bypasses, and infrastructure are needed beyond a single memory corruption vulnerability (like CVE-2026-86950's OOB write) to achieve a fully functional jailbreak with Sileo.

---

## 1. The Gap: OOB Write → Sileo

CVE-2026-86950 gives us an **out-of-bounds write** in CoreGraphics' anti-aliased rendering path (`aa_cache_render`). This is step zero. Between that and Sileo running, we need:

| Stage | What's Needed | Why |
|-------|--------------|-----|
| 1. Controlled OOB write | Heap shaping + target selection | Raw OOB is uncontrolled; must land on useful kernel/userspace objects |
| 2. Kernel R/W primitives | `kread64`/`kwrite64` | Read/write arbitrary kernel virtual memory |
| 3. PAC bypass (arm64e) | `kcall` — call arbitrary kernel functions | A15+ devices sign pointers; need to forge or reuse signed pointers |
| 4. PPL/SPTM bypass | `physread`/`physwrite` unrestricted | Page Protection Layer (A12–A14) or Secure Page Table Monitor (A15+/M1+) guard page tables |
| 5. Privilege escalation | uid=0, unsandbox, CS_PLATFORM_BINARY | Modify proc struct, mac_label, csflags in kernel memory |
| 6. Trustcache injection | Upload cdhashes for jailbreak binaries | Kernel code signature verification must accept our binaries |
| 7. Bootstrap deployment | Extract procursus filesystem to /var/jb/ | dpkg, apt, Sileo, shell utilities, libraries |
| 8. launchd injection | Inject systemhook.dylib into launchd | Persistent tweak injection for all processes |
| 9. Userspace reboot | ldrestart completes the jailbreak | Reloads all daemons with jailbreak hooks active |

---

## 2. Kernel Primitives Architecture

From `BaseBin/libjailbreak/src/primitives_external.h`, the global primitive struct that exploit plugins must populate:

```c
struct kernel_primitives {
    int (*kreadbuf)(uint64_t kaddr, void* output, size_t size);
    int (*kwritebuf)(uint64_t kaddr, const void* input, size_t size);
    int (*physreadbuf)(uint64_t physaddr, void* output, size_t size);
    int (*physwritebuf)(uint64_t physaddr, const void* input, size_t size);
    unsigned krwMinSafeReadSize;
    uint64_t (*kcall)(uint64_t func, int argc, const uint64_t *argv);
    void (*kexec)(kRegisterState *state);
    int (*kalloc_global)(uint64_t *addr, uint64_t size);
    int (*kalloc_local)(uint64_t *addr, uint64_t size);
    int (*kfree_global)(uint64_t addr, uint64_t size);
    int (*kmap)(uint64_t pa, uint64_t size, void **uaddr);
    int (*physaccess_mapped)(uint64_t pa, uint64_t size, kernel_map_accessor);
    uint64_t (*vtophys)(uint64_t ttep, uint64_t va);
    uint64_t (*phystokv)(uint64_t pa);
};
```

The exploit plugin fills `gPrimitives` and the rest of the jailbreak chain consumes them. This is the **contract** between exploit and post-exploitation.

### Primitive Hierarchy

```
OOB Write (CVE-2026-86950)
  └─► Controlled kernel R/W (kreadbuf/kwritebuf)
        ├─► PAC bypass → kcall (call arbitrary kernel functions)
        ├─► PPL/SPTM bypass → physreadbuf/physwritebuf (unrestricted physical memory)
        └─► PTE manipulation → kmap (map physical pages to userspace)
```

---

## 3. The Dopamine Jailbreak Sequence

From `DOJailbreaker.m` `runWithError:didRemoveJailbreak:showLogs:` (878 lines), the exact sequence:

### Phase 1: System Reconnaissance
```
1. Gather system info
   - Find kernelcache path on disk
   - Find SPTM/TXM paths (if A15+/M1+)
   - Run XPF patchfinder to locate kernel symbols/offsets
```

### Phase 2: Kernel Exploitation
```
2. Run kernel exploit (via selected exploit plugin)
   - Plugin provides: kread/kwrite primitives
   - Dopamine currently uses "weightBufs" (Apple Neural Engine exploit)
   
3. PAC bypass (arm64e only)
   - Provides: kcall (call arbitrary kernel functions)
   - Required for A12+ devices where pointer authentication is enforced
   
4. PPL/SPTM bypass
   - Provides: unrestricted physread/physwrite
   - Required for A15+/M1+ (SPTM) or A12-A14 (PPL)
   - Without this, page table modifications are blocked
   
5. Build physical R/W primitive
   - Via PTE manipulation or direct physrw depending on device
   
6. Clean up exploit artifacts
   - Remove IOSurface mappings, restore corrupted state
```

### Phase 3: Privilege Escalation
```
7. elevatePrivileges (from DOJailbreaker.m)
   a. proc_find(getpid()) — locate our proc struct in kernel
   b. proc_ucred(proc) — find our credential structure
   c. kwrite to ucred for uid=0, gid=0 (become root)
   d. mac_label_set(label, 1, -1) — disable sandbox
   e. proc_csflags_set(CS_PLATFORM_BINARY | CS_INSTALLER | 
                        CS_GET_TASK_ALLOW | CS_DEBUGGED)
   f. Result: our process is now root, unsandboxed, with platform binary trust
```

### Phase 4: Bootstrap Deployment
```
8. ensureJailbreakRootExists
   - Create jailbreak root at /private/preboot/<UUID>/dopamine-<UUID>/procursus/
   
9. updateVarJbSymlink
   - Remove old /var/jb symlink
   - Create /var/jb → JBROOT_PATH("/")
   
10. prepareBootstrap (DOBootstrapper.m)
    a. Remount /private/preboot as writable (mount("apfs", ..., MNT_UPDATE))
    b. Fix path permissions (root:wheel, 0755 from / to jbroot)
    c. Extract basebin.tar to JBROOT/ (bundled in .app)
    d. If first boot: extract bootstrap_<ver>.tar.zst to JBROOT/
       - Bootstrap source: https://apt.procurs.us/bootstraps/<CFver>/bootstrap-ssh-iphoneos-arm64.tar.zst
       - Decompressed with zstd, extracted with libarchive
    e. Create /.installed_dopamine marker
    f. Write default APT sources to /etc/apt/sources.list.d/default.sources:
       - repo.chariz.com
       - havoc.app
       - apt.thebigboss.org/repofiles/cydia/ (stable main)
       - ellekit.space
    g. Create /var/mobile/Library/Preferences (uid 501, gid 501)
    h. Patch basebin daemon plists (replace @JBROOT@ with actual path)
```

### Phase 5: Trustcache & Code Signing
```
11. loadBasebinTrustcache
    - Read basebin.tc from app bundle
    - Upload to kernel trustcache via trustcache_file_upload_with_uuid()
    - UUID: "BASEBIN\0..." (fixed identifier)
    - Effect: all BaseBin binaries are now trusted by the kernel's
      code signature verification, even without Apple signatures
```

### Phase 6: launchd Injection (The Boomerang Mechanism)
```
12. injectLaunchdHook
    a. Start boomerang XPC server in Dopamine app
       - Boomerang: mechanism to transfer kernel primitives from
         the Dopamine app process to launchd
    b. opainject launchd with launchdhook.dylib
       - opainject: Dopamine's process injection tool
       - launchdhook: hooks launchd to manage jailbreak state
    c. Boomerang server transfers primitives to launchd
       - Now launchd owns kread/kwrite/kcall/physrw
    d. launchd can now manage jailbreak persistence across process spawns
```

### Phase 7: System Integration
```
13. Apply protections
    - Bind mount over /private/preboot to prevent discovery
    - Protect sensitive system files
    
14. createFakeLib
    - Build modified dyld binary in JBROOT/basebin/gen/
    - Set DYLD_INSERT_LIBRARIES=/usr/lib/systemhook.dylib
    - systemhook.dylib: injected into every process at launch
      (this is how tweaks work — global dylib injection via dyld)
    
15. finalizeBootstrap
    a. Run prep_bootstrap.sh (first boot only)
    b. Install package managers (Sileo .deb from app bundle)
       - dpkg -i <path-to-sileo.deb>
    c. Install bundled packages:
       - libkrw0-dopamine (v2.0.3) — userspace kernel R/W API
       - libroot-dopamine (v1.0.1) — rootless path resolution
       - dopamine-basebin-link (v1.0.0) — symlinks for basebin tools
       - launchctl (v1:1.2.0) — iOS 19+ launchctl replacement
    
16. rebuildIconCache (uicache)
    - Refresh SpringBoard's app icon cache so Sileo appears
    
17. Launch jailbreak daemons
    - Start BaseBin LaunchDaemons
    
18. Userspace reboot (ldrestart)
    - Restarts all userspace processes
    - All processes now load with systemhook.dylib injected
    - Jailbreak is fully active
```

---

## 4. BaseBin Component Map

All live under `JBROOT/basebin/` (typically `/var/jb/basebin/`):

| Component | Role |
|-----------|------|
| **libjailbreak** | Core library: primitives API, kernel helpers, trustcache, XPC, process info |
| **launchdhook** | Dylib injected into launchd; manages jailbreak state, spawns hooked processes |
| **systemhook** | Dylib injected into every process via DYLD_INSERT_LIBRARIES; enables tweak loading |
| **opainject** | Process injection tool — injects dylibs into running processes |
| **boomerang** | Primitive transfer mechanism — passes kread/kwrite/kcall from app → launchd |
| **jbctl** | CLI jailbreak control tool — install packages, manage state, trigger rebuilds |
| **forkfix** | Fixes fork() behavior in jailbroken processes (kernel patches for posix_spawn) |
| **dyldhook** | Hooks dyld to redirect library loading for rootless paths |
| **rootlesshooks** | System-wide hooks for rootless compatibility (path translation /usr → /var/jb/usr) |
| **hookd** | Hook daemon — manages runtime function hooking |
| **watchdoghook** | Prevents watchdog from killing jailbreak processes during long operations |
| **idownloadd** | Optional: on-device shell daemon for debugging |
| **ChOma** | Mach-O parsing/manipulation library (code signing, entitlements) |
| **XPF** | Kernel patchfinder — locates symbols/offsets in kernelcache without symbols |
| **MachOMerger** | Merges Mach-O binaries (used for fakelib/dyld construction) |

---

## 5. Rootless Architecture (/var/jb/)

Traditional jailbreaks wrote to `/usr`, `/bin`, `/Library` etc. (rootful). Rootless puts everything under `/var/jb/` which symlinks to a UUID-based path in `/private/preboot/`:

```
/var/jb → /private/preboot/<BOOT-UUID>/dopamine-<JB-UUID>/procursus/

/var/jb/
├── basebin/           ← Dopamine's core binaries + LaunchDaemons
├── usr/
│   ├── bin/           ← dpkg, apt, bash, etc.
│   ├── lib/           ← dpkg libs, libroot, libkrw, tweaks
│   └── include/
├── etc/
│   └── apt/
│       └── sources.list.d/  ← APT repository configuration
├── var/
│   ├── lib/dpkg/      ← dpkg database (installed packages)
│   └── mobile/        ← user preferences
├── Library/
│   └── MobileSubstrate/DynamicLibraries/  ← tweaks go here
├── Applications/      ← jailbreak apps (Sileo lives here)
└── .installed_dopamine  ← marker file
```

### Why Rootless?

1. **Survives OTA updates** — `/var` partition persists across iOS updates; system partition is wiped
2. **No SSV issues** — Signed System Volume (iOS 15+) makes system partition immutable; rootless avoids it entirely
3. **Easier to remove** — delete one directory and the symlink; no system files modified

---

## 6. Sileo Installation Flow

Sileo is installed as a `.deb` package during `finalizeBootstrap`:

1. `prep_bootstrap.sh` runs initial setup (creates directories, sets permissions)
2. `installPackageManagers` calls `dpkg -i <sileo.deb>` via `jbctl internal install_pkg`
3. The `.deb` extracts to `/var/jb/Applications/Sileo.app/` and supporting files
4. `uicache` rebuilds SpringBoard's icon database → Sileo icon appears on home screen
5. Default repos are pre-configured (Chariz, Havoc, BigBoss, ElleKit)

### What Sileo Needs at Runtime

- **Root access**: Sileo runs privileged operations via `jbctl` or direct `dpkg` calls
- **Network access**: Downloads packages from configured APT repositories
- **libroot**: Resolves `/var/jb/` paths correctly (the `JBROOT_PATH()` macro)
- **libkrw**: Optional but used by some packages that need kernel R/W post-install
- **Working dpkg/apt**: The bootstrap's package management infrastructure

---

## 7. What We Need for iOS 27

### From CVE-2026-86950 to Full Chain

The OOB write in `aa_cache_render` corrupts a heap buffer during font rendering. To build a full jailbreak:

#### Step 1: Heap Shaping (Research Needed)
- Control what kernel objects surround the corrupted coverage buffer
- IOSurface spray is the classic technique (Dopamine's weightBufs uses IOSurface)
- Need to understand iOS 27's heap allocator (kalloc zones, sequoia changes)

#### Step 2: OOB → kread/kwrite
- Corrupt an IOSurface or pipe buffer structure to get arbitrary R/W
- This is the most exploit-specific step — varies by target object
- WeightBufs (Dopamine's current exploit) uses Apple Neural Engine + IOSurface

#### Step 3: PAC Bypass
- iPad Mini has A17 Pro → arm64e with SPTM
- Need either:
  - A PAC signing gadget (find kernel code that signs attacker-controlled values)
  - A PAC-less call primitive (find an indirect call site that doesn't check PAC)
  - Thread state manipulation (forge kcall via exception handling)

#### Step 4: SPTM Bypass
- A17 Pro uses Secure Page Table Monitor
- SPTM runs at a higher privilege level than XNU
- Need to either:
  - Find an SPTM vulnerability
  - Use legitimate SPTM APIs in an unintended way
  - Build physical R/W through PTE manipulation within SPTM's constraints

#### Step 5: Post-Exploitation
- Once we have the full primitive set (kread/kwrite/kcall/physrw), the Dopamine chain from Phase 3 onward is **largely reusable**
- Kernel struct offsets need updating for iOS 27 (XPF patchfinder handles this)
- Bootstrap and Sileo installation is iOS-version-independent

### Reusable vs. Version-Specific

| Component | Reusable? | Notes |
|-----------|-----------|-------|
| Bootstrap (procursus) | Yes | Same tar for similar CF versions |
| Sileo | Yes | Version-independent package manager |
| BaseBin infrastructure | Mostly | Needs offset updates, possible SPTM changes |
| Privilege escalation | Mostly | proc/ucred struct offsets may change |
| Trustcache injection | Mostly | API may change between iOS versions |
| Kernel exploit | **No** | Must be developed per-vulnerability |
| PAC bypass | **No** | Gadgets are version/device specific |
| SPTM bypass | **No** | Highest difficulty; SPTM is well-audited |

---

## 8. Exploit Plugin Interface

Dopamine uses a modular exploit system. The plugin must:

1. Populate `gPrimitives` struct with working function pointers
2. Call back into libjailbreak to signal readiness
3. Handle cleanup when asked

Known plugins:
- **weightBufs**: Apple Neural Engine kernel exploit (current, in-repo)
- **ClearSword**, **DarkSword**, **Titan**: Referenced in older Dopamine versions (not in current repo)

For CVE-2026-86950, we'd write a new plugin that:
1. Triggers the font rendering OOB write with controlled heap layout
2. Corrupts a target kernel object to establish kread/kwrite
3. Chains to PAC bypass for kcall
4. Chains to SPTM bypass for physrw
5. Populates `gPrimitives` and hands off to Dopamine's post-exploitation

---

## 9. Key Takeaways

1. **The exploit is the smallest part** — the OOB write itself is maybe 10% of the work. The other 90% is heap shaping, primitive building, PAC/SPTM bypass, and reliability engineering.

2. **Post-exploitation is modular and reusable** — Dopamine's BaseBin, bootstrap, and Sileo installation are largely version-independent. The exploit plugin is the only part that must be written from scratch.

3. **SPTM is the hardest barrier** — On A15+ devices (including iPad Mini's A17 Pro), SPTM protects page tables at a higher privilege level than the kernel. This is the single hardest bypass to develop.

4. **Trustcache is the gatekeeper** — Without trustcache injection, jailbreak binaries can't run even with root access. The kernel kills any process whose cdhash isn't in a trustcache.

5. **Semi-untethered means re-exploit on reboot** — The jailbreak doesn't survive a full reboot. The Dopamine app must be run again after each restart to re-exploit and re-inject.

---

## References

- Dopamine source: https://github.com/opa334/Dopamine
- Procursus bootstrap: https://apt.procurs.us/
- Sileo: https://github.com/Sileo/Sileo (1478 stars)
- ElleKit (tweak injection): https://ellekit.space/
- Key files studied:
  - `Application/Dopamine/Jailbreak/DOJailbreaker.m` (878 lines)
  - `Application/Dopamine/Jailbreak/DOBootstrapper.m` (634 lines)
  - `Application/Dopamine/Jailbreak/DOEnvironmentManager.h`
  - `BaseBin/libjailbreak/src/primitives.h` / `primitives_external.h`
  - `BaseBin/libjailbreak/src/kernel.h`
  - `BaseBin/libjailbreak/src/trustcache.h`
