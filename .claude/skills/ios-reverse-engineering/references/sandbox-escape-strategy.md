# Sandbox Escape Strategy for CVE-2026-86950 Jailbreak Chain

## TL;DR

**No separate sandbox escape CVE is needed.** IOSurface is reachable from both the app sandbox and the WebContent (Safari) sandbox. The kernel vulnerability (CVE-2026-112477, IOSurface race condition) serves as both the kernel exploit *and* the sandbox escape in a single step.

The simplified chain is:

```
PDF bug → arbitrary R/W (userspace) → IOSurface kernel bug → kernel R/W → jailbreak
```

---

## 1. PDF Rendering Context on iOS 27

The CoreGraphics vulnerability (CVE-2026-86950) fires during `CGImageSourceCreateThumbnailAtIndex` or any PDF rendering path. The sandbox it runs under depends on which process triggers the render:

| Delivery Vector | Rendering Process | Sandbox Profile | IOKit Access |
|----------------|-------------------|-----------------|--------------|
| **Jailbreak app** | App process | App sandbox | **YES** — full IOKit |
| **Safari** | WebContent | com.apple.WebProcess | **YES** — IOSurface, AGX |
| **Files.app** | QuickLook agent | com.apple.quicklook | **YES** — limited IOKit |
| **iMessage** | IMTranscoderAgent | com.apple.IMTranscoderAgent | **YES** — IOKit |

**Key fact:** `CGImageSourceCreateThumbnailAtIndex` is an in-process call. It runs in whatever process invokes it — there is no separate rendering sandbox for this API.

## 2. Why No Sandbox Escape Is Needed

### The traditional "sandbox escape" question

In targeted attacks (e.g., NSO's FORCEDENTRY), the attacker doesn't control which process renders the payload. The chain must work from wherever the exploit fires (e.g., IMTranscoderAgent). If that process can't reach the kernel driver, a separate sandbox escape is needed to pivot to a more privileged process.

### Our situation is different

For a **voluntary jailbreak** on a device the user owns:

1. **Custom app approach (recommended):** The user sideloads a jailbreak app (via AltStore, Xcode, or TrollStore). The app calls `CGImageSourceCreateThumbnailAtIndex` on the exploit PDF *from its own process*. The app sandbox allows full IOKit access, including IOSurface. No sandbox escape needed.

2. **Safari approach (stretch goal):** The user visits an exploit page in Safari. The PDF renders in WebContent. The WebContent sandbox *allows* IOSurface connections (required for GPU compositing). So WebContent → IOSurface → kernel works without an additional bug.

This matches how every modern jailbreak works:

| Jailbreak | Sandbox Escape? | Approach |
|-----------|----------------|----------|
| unc0ver | No | App → kernel vuln directly |
| Dopamine | No | App → kfd kernel exploit |
| palera1n | No | App → checkm8 USB → kernel |
| checkra1n | No | USB exploit → kernel directly |
| Taurine | No | App → kernel vuln directly |

**The kernel vulnerability IS the sandbox escape** — it elevates from user process (any sandbox level) to kernel privilege.

## 3. What IOSurface Access Gives Us

IOSurface is the target for CVE-2026-112477 (race condition). It's reachable because:

- **From app sandbox:** `IOServiceOpen` on `IOSurfaceRoot` user client. Apps need IOSurface for GPU rendering, so it's always permitted.
- **From WebContent:** WebKit's GPU Process uses IOSurface for compositing. The sandbox profile explicitly allows `IOSurfaceRootUserClient`.

### IOSurface Race Exploitation Pattern

```
1. Create IOSurface objects with controlled properties
2. Trigger the race condition (concurrent access from two threads)
3. Race yields double-free or UAF on a kernel allocation
4. Spray kernel heap to reclaim freed memory with controlled data
5. Access the IOSurface through the stale reference → type confusion
6. Build kernel R/W from the type-confused access
```

This is well-trodden ground — IOSurface has been exploited in:
- Operation Triangulation (CVE-2023-32434)
- Multiple Google Project Zero submissions
- DarkSword chain (CVE-2025-43510)

## 4. Recommended Exploit Chain Architecture

### Phase A: Jailbreak App (Primary Path)

```
┌─────────────────────────────────────────────────┐
│ Step 1: Jailbreak App (sideloaded)              │
│   - Embeds exploit PDF + trigger font           │
│   - Calls CGImageSourceCreateThumbnailAtIndex   │
│   - CVE-2026-86950 fires in app process         │
├─────────────────────────────────────────────────┤
│ Step 2: Heap Feng Shui (DONE)                   │
│   - Spray 5120-byte SMALL magazine allocations  │
│   - Forward OOB (+5120B) hits byte 0 of next    │
│   - Corrupts victim object header               │
├─────────────────────────────────────────────────┤
│ Step 3: Arbitrary R/W (userspace)               │
│   - Corrupted CFData length → oversized read    │
│   - Leak heap pointers → defeat ASLR            │
│   - Build fake objects → arbitrary R/W          │
├─────────────────────────────────────────────────┤
│ Step 4: Kernel Exploit                          │
│   - Open IOSurfaceRootUserClient (app allowed)  │
│   - Trigger CVE-2026-112477 (race condition)    │
│   - Kernel heap spray → reclaim UAF memory      │
│   - Build kernel R/W                            │
├─────────────────────────────────────────────────┤
│ Step 5: Post-Exploitation                       │
│   - KASLR defeat (kernel info leak)             │
│   - Find our proc → ucred → set uid=0           │
│   - Remove sandbox profile                      │
│   - Inject CDHashes into trust cache            │
│   - PAC bypass (signing gadgets or key leak)    │
├─────────────────────────────────────────────────┤
│ Step 6: Bootstrap                               │
│   - Mount rootfs read-write (or rootless)       │
│   - Install bootstrap (Procursus/Elucubratus)   │
│   - Install Sileo package manager               │
│   - Install substrate/Substitute for tweaks     │
└─────────────────────────────────────────────────┘
```

### Phase B: Safari URL (Stretch Goal)

Same chain, but:
- Step 1 becomes: user visits `http://local/exploit.html` → embeds PDF via `<embed>` or `<iframe>`
- Rendering happens in WebContent process
- WebContent CAN reach IOSurface, so Steps 4-6 are identical
- Slightly harder because WebContent has tighter memory layout (ASLR, JIT)

## 5. What About PAC and PPL?

### PAC (Pointer Authentication Codes)

All our test devices are A12+ (arm64e). PAC signs code and data pointers. After getting kernel R/W:

- **Approach 1 — Signing gadgets:** Find kernel code that executes `pacia`/`pacda` with attacker-controlled register values. Chain these to sign arbitrary pointers.
- **Approach 2 — Key leak:** Use kernel R/W to read the PAC keys from kernel memory, then sign pointers in userspace.
- **Approach 3 — Avoid PAC paths:** Some kernel code paths don't validate PAC (legacy compatibility). Target these.

### PPL (Page Protection Layer)

PPL protects page tables even from kernel R/W. Needed for:
- Remapping executable pages (for code injection)
- Trust cache modification (stored in PPL-protected memory on newer iOS)

Approaches:
- **PPL entry point abuse:** Call PPL functions with crafted arguments via kernel R/W
- **APRR manipulation:** Modify the hardware access permission state
- **Rootless jailbreak:** Avoid PPL entirely by not modifying protected pages (Dopamine approach)

**Recommendation:** Use rootless jailbreak approach (like Dopamine) to avoid PPL entirely. Trust cache injection via the XNU-level linked list rather than the PPL-protected static cache.

## 6. Comparison: Our Chain vs. Published Chains

| Component | FORCEDENTRY (NSO) | Our Chain | Dopamine |
|-----------|------------------|-----------|----------|
| Initial bug | CoreGraphics int overflow | CoreGraphics int overflow | Kernel vuln directly |
| Arbitrary R/W | JBIG2 Turing machine | Heap feng shui + CFData | N/A (kernel directly) |
| Sandbox escape | Not needed (IMTranscoderAgent) | Not needed (IOSurface reachable) | Not needed (app) |
| Kernel exploit | Undisclosed | CVE-2026-112477 (IOSurface) | kfd variants |
| PAC bypass | PAC signing gadgets | TBD | kfd + landa |
| PPL bypass | Full PPL bypass | Rootless (avoid PPL) | Rootless |

## 7. Next Concrete Steps

1. **Complete Step 3** (arbitrary R/W from CVE-2026-86950) — victim object corruption, CFData length expansion
2. **Prototype the jailbreak app** — Xcode project that loads exploit PDF and calls CGImageSourceCreateThumbnailAtIndex
3. **Research CVE-2026-112477** — IOSurface race condition PoC development
4. **KASLR defeat** — Use CVE-2026-113910 (info disclosure) or leak kernel pointers via IOSurface
5. **Build kernel R/W** — IOSurface UAF → kalloc spray → type confusion
6. **Post-exploit** — ucred + trust cache + bootstrap installation
