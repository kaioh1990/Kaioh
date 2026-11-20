# WebKit Sandbox Escape — iOS 27 Research

## Overview

After achieving code execution in WebKit.Networking (via CVE-2026-111497 Brotli OOB write),
the next step is escaping the sandbox. WebKit on iOS runs in a sandboxed process model with
multiple process types, each with different sandbox profiles and capabilities.

## WebKit Process Architecture on iOS

```
Safari (UI Process) — full app sandbox
  ├── WebContent (renderer) — tight sandbox, per-tab
  ├── WebKit.Networking — network operations, Brotli/gzip decompression
  ├── WebKit.GPU — GPU/Metal operations
  └── WebKit.Extension — web extensions
```

### WebKit.Networking Process

This is where CVE-2026-111497 gives us code execution. Key characteristics:
- Handles all HTTP/HTTPS requests for all tabs
- Performs content decoding (Brotli, gzip, deflate)
- Has network access (by design — it IS the network process)
- Has access to cookies, credentials, TLS session state
- Sandboxed but with broader IPC capabilities than WebContent

### Sandbox Profile Differences

| Process | Network | Filesystem | IPC | Mach Services |
|---------|---------|-----------|-----|---------------|
| WebContent | None (proxied) | Very limited | Limited mach ports | Few |
| **Networking** | **Full outbound** | Limited | **More mach ports** | **More services** |
| GPU | None | GPU device access | Limited | Metal/IOKit |
| UI Process | Full app | App container | Full | Full |

**Key advantage of Networking over WebContent**: The Networking process can make arbitrary
network connections. This means after code execution, we can:
1. Exfiltrate data
2. Download additional payloads
3. Communicate with C2 infrastructure
4. The sandbox escape doesn't need to also provide network access

## CVE-2026-113897: WebKit Process Model Sandbox Escape

**Fixed in**: iOS 27.2, Safari 27.2, macOS 27.2 (October 26, 2026)
**Impact**: A remote attacker may be able to break out of Web Content sandbox
**Component**: WebKit Process Model
**Fix**: Removed the vulnerable code
**Credits**: Luke Francis and Jackson Kuja
**WebKit Bug**: 328129

### Status on iOS 27.0.1

iOS 27.0.1 (Build 24A446) predates iOS 27.2 and has NO published CVE fixes.
**CVE-2026-113897 is PRESENT on iOS 27.0.1 and 27.1.**

This is significant — it provides a potential sandbox escape from WebContent.
However, we need to confirm whether it also works from WebKit.Networking
(the advisory says "Web Content sandbox" specifically).

### Research Notes

- Apple's fix was to REMOVE the vulnerable code entirely, suggesting the
  attack surface was an unnecessary IPC endpoint
- No public exploit or technical details available
- Not on CISA KEV — no known in-the-wild exploitation
- CVSS potentially 10.0 (one source) but unverified

## Historical WebKit Sandbox Escape Techniques

### 1. Mach Port/Service Attacks (Most Common)

WebKit processes communicate with system services via Mach IPC. Historical escapes:
- **CVE-2019-8605**: Sock port — exploited a Mach port UaF to get task port of unsandboxed process
- **CVE-2021-30983**: FORCEDENTRY — NSO Group exploited IMTranscoderAgent via BlastDoor
- **mach_portal** (Ian Beer, 2016): Used Mach port name reuse to get host_priv port

Pattern: Find a Mach service reachable from the sandbox that has a vulnerability,
exploit it to get the task port of a more privileged process.

### 2. IOKit Driver Attacks

IOKit drivers run in kernel space but are reachable from sandboxed processes:
- **IOSurface**: Historically rich attack surface (CVE-2026-112477 is a race condition)
- **AppleAVE2**: Video encoder/decoder driver — multiple historical CVEs
- **AGXAccelerator**: GPU driver — complex, error-prone

Pattern: IOKit clients can be created from sandbox → driver bugs → kernel code execution

### 3. XPC Service Attacks

Some XPC services are reachable from WebKit processes:
- **launchd**: Process lifecycle management
- **configd**: Network configuration
- **mDNSResponder**: DNS resolution

Pattern: Craft malicious XPC messages to trigger bugs in XPC service handlers

### 4. Shared Memory Attacks

WebKit processes share memory regions with other processes:
- GPU shared memory between WebContent and GPU process
- Shared cache (dyld shared cache) is mapped into all processes

Pattern: Corrupt shared memory to influence behavior of less-sandboxed process

## iOS 27.0.1 Attack Surface for Sandbox Escape

### From WebKit.Networking specifically:

1. **CVE-2026-113897** (WebKit Process Model) — if reachable from Networking process
2. **IOSurface race** (CVE-2026-112477) — IOSurface is typically reachable from sandboxed processes
3. **Kernel UAF** (CVE-2026-65350) — if the triggering code path is reachable from sandbox
4. **Network daemon IPC** — Networking process communicates with network daemons
5. **Custom Mach services** — enumerate what services are reachable

### Enumeration Strategy

To determine reachable services from WebKit.Networking on iOS 27.0.1:
1. Look at the sandbox profile for the Networking process
2. Check which Mach ports are inherited or can be looked up
3. Test which IOKit user clients can be opened
4. Map XPC services that accept connections

## IOSurface as Combined Sandbox Escape + Kernel Exploit

### Why IOSurface is Attractive

IOSurface serves as a **bridge between userspace sandbox escape and kernel exploit**:
- IOSurface IOKit user client is accessible from most sandboxed processes
- It manages shared GPU memory surfaces between processes
- Complex internal state machine with races and size calculations
- Historical track record of exploitable bugs

### CVE-2026-112477: IOSurface Race Condition

- **Type**: Race condition in IOSurface
- **Fix**: Improved locking (iOS 27.2)
- **Impact**: System termination or kernel memory corruption
- **Present on**: iOS 27.0.1, iOS 27.1
- **Credits**: Anonymous researcher

Third-party research suggests this involves surface header region size validation —
a surface declaring 0x100000000 (4 GiB) header region is accepted while the real
allocation is only 64 KiB. The race is between surface creation and property access.

### Exploitation Path (Theoretical)

```
WebKit.Networking code execution (CVE-2026-111497)
  → Open IOSurface user client (from sandbox)
    → Trigger race condition (CVE-2026-112477)
      → Corrupt kernel memory
        → Achieve kernel R/W primitive
```

## Kernel Vulnerabilities Present in iOS 27.0.1

| CVE | Type | Component | Severity | Notes |
|-----|------|-----------|----------|-------|
| CVE-2026-65350 | UAF | Kernel | High | Memory management flaw |
| CVE-2026-112477 | Race | IOSurface | High | Locking issue |
| CVE-2026-112507 | OOB Write | Kernel | High | Bounds checking |
| CVE-2026-113910 | Info Disclosure | Kernel | Medium | Memory leak |

All fixed in iOS 27.2 (October 26, 2026).

## Post-Sandbox-Escape: Kernel Exploitation

After escaping the sandbox, the next step is kernel R/W. The chain:

1. **Kernel info leak** (CVE-2026-113910) — defeat KASLR
2. **Kernel memory corruption** (CVE-2026-65350 UAF or CVE-2026-112507 OOB write)
3. **Build kernel R/W primitive** — typically via corrupted IOKit object vtable
4. **PAC bypass** — sign forged pointers (very few public techniques)
5. **PPL/SPTM bypass** — manipulate page tables for trust cache injection

## References

- [CVE-2026-113897 Details](https://app.opencve.io/cve/CVE-2026-113897)
- [Safari 27.2 Security Content](https://support.apple.com/en-us/149527)
- [macOS 27.2 Security Content](https://support.apple.com/en-us/149521)
- [iOS 27.2 Vulnerabilities](https://cybersecuritynews.com/apple-ios-27-2-released/)
- [CVE-2026-65350 Details](https://app.opencve.io/cve/CVE-2026-65350)
- [mach_portal sandbox escape](https://github.com/maximehip/mach_portal/blob/master/sandbox_escape.c)
- [DEF CON 26: Escaping iOS 11 Sandbox](https://media.defcon.org/DEF%20CON%2026/DEF%20CON%2026%20presentations/)
- [Stefan Esser on Brotli → WebKit.Networking code execution](https://x.com/i0n1c/status/2120230223979672037)
- [TimmyTurner51/brotli-lab iOS harness](https://github.com/TimmyTurner51/brotli-lab)
- [SKVLENT/iOS27-Research-Exploits](https://github.com/SKVLENT/iOS27-Research-Exploits)
- [remontsuri/ios27-surface-audit](https://github.com/remontsuri/ios27-surface-audit)
