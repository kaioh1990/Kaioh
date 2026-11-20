# Post-Exploitation: Rootless Jailbreak Bootstrap & Sileo Installation

## Overview

This document covers everything needed AFTER achieving kernel read/write on iOS 27.0.1 —
the post-exploitation steps to go from kernel primitives to a working Sileo package manager.

**Target:** iPad Mini (iPad16,1), iOS 27.0.1, Build 24A446, A17 Pro (arm64e)

## Revised Exploit Chain (Nov 2026)

```
CVE-2026-111497 (Brotli OOB write) → WebKit.Networking code exec
  → Sandbox escape (WebKit → system)
    → Kernel exploit (CVE-2026-65350 UAF or CVE-2026-112477 IOSurface race)
      → Kernel R/W
        → PAC bypass
          → PPL/SPTM bypass
            → Trust cache injection
              → Procursus bootstrap deployment
                → Sileo installation ← GOAL
```

**Entry point:** CVE-2026-111497 — Brotli decoder OOB write (confirmed VULNERABLE on iOS 27.0.1)
**Previous entry:** CVE-2026-86950 — CoreGraphics font overflow (confirmed PATCHED — dead end)

---

## Step 1: Kernel Primitives Required

Before post-exploitation can begin, the exploit chain must provide these primitives:

| Primitive | What It Does | How It's Used |
|-----------|-------------|---------------|
| **Kernel read** | Read arbitrary kernel virtual addresses | Find kernel objects, defeat KASLR |
| **Kernel write** | Write arbitrary kernel virtual addresses | Modify kernel state, disable protections |
| **PPL bypass** | Modify page tables protected by Page Protection Layer | Mark pages executable, inject code |
| **AMFI bypass** | Defeat Apple Mobile File Integrity checks | Allow unsigned/ad-hoc code to run |
| **Task port** | Mach task port for kernel_task | Direct kernel memory manipulation |

On A17 Pro (arm64e), **SPTM** (Secure Page Table Monitor) replaced PPL starting with
A15/A16 on iOS 17+. SPTM runs at a higher privilege level than the kernel itself.

### Known Bypasses (as of Nov 2026)

| Bypass | Author | iOS Range | Chips |
|--------|--------|-----------|-------|
| **Titan** (PPL/SPTM) | alfiecg24 | 16.5.1–17.3.1 | A15–A17, M1–M2 |
| **momentarius** (PPL) | staturnzz, TheRealClarity | 16.6–18.7.1, 26.0–26.0.1 | A12, A13 only |

**Gap:** No public PPL/SPTM bypass exists for iOS 27 on A17 Pro. Calif.io demonstrated
one at Hexacon (Oct 2026) but hasn't released it.

---

## Step 2: Trust Cache Injection

The **trust cache** is a kernel-side list of CDHashes (code directory hashes) that AMFI
checks before allowing a binary to execute. On stock iOS, only Apple-signed binaries are
in the trust cache.

### How Modern Jailbreaks Handle Trust Caches

**Dopamine's approach (reference implementation):**
1. Jailbreak app loads `basebin.tc` — a precomputed trust cache file containing CDHashes
   of all basebin binaries (launchdhook, systemhook, jbctl, etc.)
2. Using kernel write primitive, injects a new trust cache entry into the kernel's
   linked list of trust caches (`struct pmap_image4_trust_cache`)
3. Each entry contains:
   - Header with version, number of entries
   - Array of CDHash entries (20 bytes each for SHA-1, or 32 bytes for SHA-256)
   - Flags indicating hash type and trust level
4. Once injected, AMFI allows execution of any binary whose CDHash is in the trust cache

### Trust Cache Format (iOS 16+)

```c
struct trust_cache_entry_v2 {
    uint8_t  cdhash[20];     // SHA-1 of CodeDirectory
    uint8_t  hash_type;      // 1=SHA-1, 2=SHA-256 (truncated to 20)
    uint8_t  flags;          // constraint category
};

struct trust_cache_v2 {
    uint32_t version;        // 2
    uint8_t  uuid[16];       // unique identifier
    uint32_t entry_count;
    struct trust_cache_entry_v2 entries[];
};
```

### For iOS 27: What's Needed

1. **Compute CDHashes** of all bootstrap binaries (dpkg, apt, bash, ssh, Sileo, etc.)
2. **Build a trust cache blob** in the v2 format
3. **Inject into kernel** via the kernel write primitive
4. **Persist across userspace** — Dopamine's launchd hook keeps the trust cache alive

### AMFI Bypass Details

AMFI (`AppleMobileFileIntegrity.kext`) enforces code signing. Two approaches:

**Option A: Trust cache injection (preferred)**
- Add CDHashes to kernel trust cache
- AMFI checks trust cache → finds hash → allows execution
- No kernel patching needed; works with KPP/KTRR/AMCC intact

**Option B: AMFI policy patching**
- Modify AMFI's MAC policy hooks in kernel memory
- Patch `amfi_check_dyld_policy_self` to always return success
- More fragile; breaks with kernel integrity checks

Modern jailbreaks (Dopamine 3.0+) exclusively use Option A.

---

## Step 3: Basebin Deployment

**Basebin** is the core jailbreak runtime — a minimal set of binaries and libraries that
provide jailbreak services to the rest of the system.

### Dopamine's Basebin Components

| Binary | Purpose |
|--------|---------|
| `launchdhook.dylib` | Injected into launchd (PID 1); hosts jbserver Mach service |
| `systemhook.dylib` | Injected into spawned processes; loads tweaks, extends sandbox |
| `jbctl` | Jailbreak control utility (CLI) |
| `jbinit` | Early-boot initialization |
| `basebin.tc` | Trust cache for basebin binaries |
| `jbloader` | Bootstrap loader; deploys Procursus on first run |

### Launchd Hook (PID 1 Injection)

The launchd hook is the backbone of the jailbreak:

1. **Injection method:** After kernel R/W is obtained, the jailbreak:
   - Finds launchd's task port
   - Maps `launchdhook.dylib` into launchd's address space
   - Creates a new thread that calls `dlopen()` on the mapped dylib
   
2. **What the hook does:**
   - Registers a Mach service called `jbserver`
   - Intercepts `posix_spawn` to inject `systemhook.dylib` into new processes
   - Manages daemon spawning from `/var/jb/Library/LaunchDaemons/`
   - Provides privileged primitives to other jailbreak components
   
3. **Why PID 1:** launchd never dies while userspace is alive, so the jailbreak
   state outlives the app. This is what makes semi-untethered jailbreaks work —
   the jailbreak app exits, but launchd keeps the hook running.

### System Hook (Process Injection)

`systemhook.dylib` is loaded into every spawned process via the launchd hook's
`posix_spawn` interception:

1. Checks if the process should receive tweak injection
2. Extends the process's sandbox profile to allow `/var/jb/` access
3. Loads the tweak injection framework (ElleKit)
4. ElleKit then loads any tweaks configured for that process

---

## Step 4: Procursus Bootstrap Installation

The **Procursus** bootstrap provides the Unix userland for the jailbreak environment.

### What Procursus Contains

| Package | Version (approx) | Purpose |
|---------|-----------------|---------|
| `dpkg` | 1.21+ | Debian package manager |
| `apt` | 2.6+ | Advanced package tool (dependency resolution) |
| `bash` | 5.2+ | Shell |
| `coreutils` | 9.0+ | ls, cp, mv, cat, etc. |
| `openssh` | 9.0+ | SSH server and client |
| `curl` | 8.0+ | HTTP client |
| `zstd` | 1.5+ | Compression (apt uses zstd) |
| `libzstd1` | 1.5+ | Zstandard shared library |
| `gpgv` | 2.4+ | GPG verification for apt |
| `base` | - | Base filesystem layout |
| `profile.d` | - | Shell initialization scripts |

### Filesystem Layout

```
/private/preboot/<boot-manifest-hash>/procursus/
├── usr/
│   ├── bin/        → dpkg, apt, apt-get, bash, ssh, scp, curl, etc.
│   ├── lib/        → libdpkg.dylib, libapt-pkg.dylib, libz.dylib, etc.
│   ├── libexec/    → dpkg helpers, apt methods
│   └── share/      → man pages, terminfo, dpkg database
├── etc/
│   ├── apt/        → sources.list, apt.conf.d/
│   ├── ssh/        → sshd_config
│   ├── dpkg/       → dpkg.cfg
│   └── profile.d/  → shell init scripts
├── var/
│   ├── lib/dpkg/   → package database (status, available, info/)
│   └── cache/apt/  → downloaded .deb files
├── Library/
│   ├── dpkg/
│   ├── Frameworks/
│   ├── LaunchDaemons/  → jailbreak daemon plists
│   └── PreferenceBundles/
└── Applications/   → Sileo.app, etc.

/var/jb → symlink to /private/preboot/<boot-manifest-hash>/procursus/
```

### Bootstrap Installation Process

1. **Create preboot directory:**
   ```
   /private/preboot/<boot-manifest-hash>/procursus/
   ```
   The boot-manifest-hash is device-specific; read from
   `/private/preboot/active` or the kernel's boot-args.

2. **Extract bootstrap tarball:**
   The jailbreak carries a compressed bootstrap (typically ~15-20 MB)
   containing all the base packages pre-installed.

3. **Create /var/jb symlink:**
   ```
   ln -s /private/preboot/<hash>/procursus /var/jb
   ```

4. **Fix permissions:**
   ```
   chown -R root:wheel /var/jb/
   chmod 755 /var/jb/usr/bin/*
   ```

5. **Register trust cache entries:**
   Compute CDHashes for every binary and dylib in the bootstrap,
   add them to the kernel trust cache.

6. **Initialize dpkg database:**
   ```
   /var/jb/usr/bin/dpkg --configure -a
   ```

7. **Configure APT sources:**
   Write `/var/jb/etc/apt/sources.list.d/procursus.sources`:
   ```
   Types: deb
   URIs: https://apt.procurs.us/
   Suites: iphoneos-arm64/1800
   Components: main
   ```

### Rootless Package Architecture

- **Architecture identifier:** `iphoneos-arm64` (not `iphoneos-arm`)
- All packages must be compiled for the rootless layout
- Paths reference `/var/jb/` instead of `/` as prefix
- Sileo and Zebra filter packages by architecture automatically

---

## Step 5: Sileo Installation

### Sileo Dependencies

Sileo requires these packages (from its control file):

| Dependency | Min Version | In Bootstrap? |
|-----------|-------------|---------------|
| `firmware` | >= 11.0 | System |
| `org.swift.libswift` | >= 5.0 | Needs install (or firmware >= 12.2) |
| `coreutils` | >= 8.30-1 | Yes (Procursus) |
| `dpkg` | >= 1.19.4-1 | Yes (Procursus) |
| `apt` | >= 1.8.2 | Yes (Procursus) |
| `libzstd1` | - | Yes (Procursus) |

### Installation Methods

**Method A: Pre-bundled in jailbreak (recommended)**

Dopamine-style jailbreaks bundle Sileo.deb in the bootstrap tarball.
On first jailbreak:
1. Bootstrap extracts → Sileo.app lands in `/var/jb/Applications/`
2. `uicache` registers the app with SpringBoard
3. Trust cache includes Sileo's CDHash
4. User taps Sileo icon → it launches

**Method B: Manual installation via dpkg**

If Sileo isn't bundled:
```bash
# Download the rootless deb
curl -L -o /tmp/sileo.deb \
  https://repo.getsileo.app/debs/sileo_2.5.4_iphoneos-arm64.deb

# Install via dpkg
/var/jb/usr/bin/dpkg -i /tmp/sileo.deb

# Fix any missing dependencies
/var/jb/usr/bin/apt-get install -f

# Register with SpringBoard
/var/jb/usr/bin/uicache -p /var/jb/Applications/Sileo.app
```

**Method C: Via apt (requires working Sileo repo)**

```bash
/var/jb/usr/bin/apt-get update
/var/jb/usr/bin/apt-get install org.coolstar.sileo
```

### Post-Installation

1. **Add default repos** in Sileo:
   - Procursus: `https://apt.procurs.us/` (base packages)
   - Havoc: `https://havoc.app/` (tweaks)
   - BigBoss+: `https://apt.thebigboss.org/repofiles/cydia-hierarchical/`
   - Chariz: `https://repo.chariz.com/`

2. **Verify package management:**
   ```bash
   /var/jb/usr/bin/apt-get update    # refresh repos
   /var/jb/usr/bin/dpkg -l           # list installed packages
   ```

3. **Enable SSH (optional):**
   ```bash
   /var/jb/usr/bin/launchctl load /var/jb/Library/LaunchDaemons/com.openssh.sshd.plist
   ```

---

## Step 6: Persistence (Semi-Untethered)

### How Semi-Untethered Works

After a reboot, the jailbreak state is lost:
- Kernel patches are gone
- Trust caches are cleared
- launchd hook is unloaded
- `/var/jb` symlink may remain but binaries won't execute (no trust cache)

**To re-jailbreak:**
1. User opens the jailbreak app (must be sideloaded/signed)
2. App re-exploits the kernel
3. Re-injects trust caches
4. Re-hooks launchd
5. Full jailbreak state restored (no data loss)

### Untethered Jailbreak (Advanced)

An untethered jailbreak survives reboots by:
1. Modifying a boot-time daemon or kernel extension
2. The modification triggers the exploit chain automatically at boot
3. Requires a boot-time code execution primitive

Calif.io demonstrated an untethered iOS 27.0.1 jailbreak at Hexacon but hasn't
released the technique. Their approach likely uses a persistent boot-time daemon
or launchd configuration that triggers the exploit chain at boot.

---

## iOS 27-Specific Considerations

### A17 Pro Security Features

| Feature | Description | Impact |
|---------|-------------|--------|
| **SPTM** | Secure Page Table Monitor (replaces PPL) | Hardware-enforced page table protection |
| **PAC** | Pointer Authentication Codes (QARMA3) | All function pointers signed |
| **MTE** | Memory Tagging Extension (if enabled) | Heap metadata corruption harder |
| **KTRR** | Kernel Text Read-only Region | Kernel code immutable |
| **SSV** | Signed System Volume | System partition integrity |
| **DMA protections** | IOMMU/DART | Prevent DMA attacks from peripherals |

### Available Kernel CVEs for iOS 27.0.1

These are patched in iOS 27.2 but present in 27.0.1:

| CVE | Type | Component | Usefulness |
|-----|------|-----------|------------|
| CVE-2026-65350 | UAF | Kernel (XNU) | Kernel R/W primitive |
| CVE-2026-112477 | Race condition | IOSurface | Kernel R/W primitive |
| CVE-2026-112507 | OOB write | Kernel | Kernel R/W primitive |
| CVE-2026-113910 | Info disclosure | Kernel | KASLR defeat |

### Liter8 — Existing iOS 27 Tethered Jailbreak (Reference)

[Liter8](https://github.com/Xplo8E/Liter8) is a tethered iOS 27 jailbreak
for A12/A13 (iPhone 11 series only) using the usbliter8 SecureROM exploit:

- Requires USB/DFU mode boot
- Uses Procursus rootless bootstrap
- SSH ramdisk for initial setup
- iOS 27.0.1 Build 24A446 listed as "experimental"
- Not applicable to our A17 Pro iPad Mini (wrong chip generation)

---

## Tools & Infrastructure Needed

### For Bootstrap Building

| Tool | Purpose |
|------|---------|
| `ldid` | Ad-hoc code signing (compute CDHash for trust cache) |
| `dpkg-deb` | Build .deb packages |
| `apt-ftparchive` | Generate APT repo metadata |
| Cross-compiler | Build arm64e binaries for iOS |

### For On-Device Work (post-exploit)

| Tool | Purpose |
|------|---------|
| `jbctl` | Jailbreak state management |
| `uicache` | Register apps with SpringBoard |
| `launchctl` | Load/unload daemons |
| `ldid` | Sign binaries for trust cache |

### Pre-Built Resources

- **Procursus bootstrap tarballs:** Built by the Procursus team, available from
  their CI or the [GeoSn0w/Procursus](https://github.com/GeoSn0w/Procursus) mirror
- **Sileo .deb:** Available from [Sileo/Sileo](https://github.com/Sileo/Sileo) releases
  or the official repo at `https://repo.getsileo.app/`
- **ElleKit:** Tweak injection framework, source at `github.com/evelyneee/ellekit`

---

## Summary: What's Blocking Sileo on iOS 27.0.1

| Requirement | Status | Notes |
|-------------|--------|-------|
| Initial code execution | **IN PROGRESS** | CVE-2026-111497 Brotli OOB write; crash PoC being built |
| Sandbox escape | **NOT STARTED** | Need WebKit.Networking → system escape |
| Kernel R/W | **NOT STARTED** | CVE-2026-65350 or CVE-2026-112477 available |
| PAC bypass | **NOT STARTED** | Very few public bypasses for A17 Pro |
| SPTM bypass | **NOT STARTED** | No public bypass for iOS 27 on A17+ |
| Trust cache injection | **DOCUMENTED** | Technique well-understood (this doc) |
| Procursus bootstrap | **DOCUMENTED** | Available from Procursus project |
| Sileo installation | **DOCUMENTED** | Standard dpkg/apt install once bootstrap exists |

**Bottom line:** The post-exploitation path (Steps 6-8: trust cache → bootstrap → Sileo)
is well-documented and uses established techniques. The blockers are all in the
exploitation chain (Steps 1-5), specifically the SPTM bypass for A17 Pro.

---

## Sources

- [Dopamine (opa334)](https://github.com/opa334/Dopamine) — Reference rootless jailbreak
- [Dopamine Architecture (DEV Community)](https://dev.to/arshtechpro/how-dopamine-works-the-architecture-of-a-modern-ios-jailbreak-2hj3)
- [Dopamine Process Flow (DeepWiki)](https://deepwiki.com/opa334/Dopamine/3-jailbreak-process-flow)
- [Procursus (The Apple Wiki)](https://theapplewiki.com/wiki/Procursus) — Bootstrap project
- [Rootless (The Apple Wiki)](https://theapplewiki.com/wiki/Rootless) — Rootless jailbreak layout
- [Rootless (Theos Docs)](https://theos.dev/docs/rootless) — Developer reference for rootless
- [Sileo (GitHub)](https://github.com/Sileo/Sileo) — Package manager source
- [Liter8 (GitHub)](https://github.com/Xplo8E/Liter8) — iOS 27 tethered jailbreak reference
- [Dopamine (The Apple Wiki)](https://theapplewiki.com/wiki/Dopamine) — Version history
- [Dopamine 3.0 (iClarified)](https://www.iclarified.com/101697/dopamine-30-jailbreak-released-with-ios-1731-support-ios-26-on-a12a13) — Titan/momentarius bypasses
