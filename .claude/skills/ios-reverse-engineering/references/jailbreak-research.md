# iOS 27 Jailbreak Research — Lockdown Mode Attack Surface

## Status: Active Research

## Background

A jailbreak for iOS 27 on iPhone Duo has reportedly been achieved via Lockdown Mode but not publicly released. This document tracks our research into how Lockdown Mode changes the iOS attack surface and potential exploitation paths.

## What Lockdown Mode Changes (Attack Surface Analysis)

Lockdown Mode disables/restricts many subsystems. Each change is a **code path difference** — the system must branch on whether Lockdown Mode is enabled, and those branches are new code that may contain vulnerabilities.

### High-Interest Attack Surface Changes

| Subsystem | What Changes | Why It's Interesting |
|-----------|-------------|---------------------|
| **WebKit/Safari** | "Certain complex web technologies are blocked" — JIT compilation disabled, web fonts blocked, some image codecs disabled | Disabling JIT means WebKit uses a **different code generation path** (interpreter-only). This is a fundamentally different execution model that may have less testing/hardening |
| **USB/Accessory Connections** | Device must be unlocked + explicit approval for accessories | Changes the USB stack's authentication flow — new state machine for connection approval |
| **Configuration Profiles** | MDM enrollment and profile installation blocked | The profile validation code now has a "reject all" path that didn't exist before |
| **Wi-Fi** | Auto-join disabled, non-secure networks disconnected, 2G/3G cellular disabled | Different network stack behavior, modem configuration changes |
| **Messages/iMessage** | Most attachments blocked, link previews disabled | The attachment parsing/rendering pipeline now has a filter stage |
| **FaceTime** | Incoming calls blocked unless previous contact | New call screening logic in FaceTime daemon |
| **Photos** | Location stripping, shared albums removed | EXIF handling changes, photo sharing daemon changes |

### Key Insight: Code Path Differentials

When Lockdown Mode is ON, the system must check a flag and branch. These checks happen in:
- **Kernel**: The kernel must expose the Lockdown Mode state to userspace
- **Daemons**: System daemons (mediaserverd, WebKit, configd, etc.) check the flag
- **Frameworks**: UIKit, WebKit, NetworkExtension, etc. all behave differently

Each check is a potential vulnerability:
1. **Race conditions**: What if Lockdown Mode state changes during a sensitive operation?
2. **Incomplete checks**: What if a code path is restricted in one place but not another?
3. **State confusion**: What if enabling/disabling Lockdown Mode leaves the system in an inconsistent state?

## iOS Security Architecture (What Needs to Be Bypassed)

### Modern iOS Exploit Chain (typical)

```
1. Initial Entry Point (userspace code execution)
   ↓
2. Sandbox Escape (break out of app sandbox)
   ↓
3. Kernel Read/Write (gain kernel memory access)
   ↓
4. PAC Bypass (defeat pointer authentication)
   ↓
5. PPL Bypass (defeat Page Protection Layer)
   ↓
6. KTRR Bypass (defeat Kernel Text Read-only Region) — hardware enforced
   ↓
7. Persistence (optional — survive reboot)
```

### Mitigation Details

| Mitigation | What It Does | Bypass Difficulty |
|-----------|-------------|-------------------|
| **ASLR** | Randomizes memory layout | Medium — info leaks common |
| **PAC** (Pointer Authentication Codes) | Cryptographic signatures on pointers | Hard — requires signing gadgets or key leaks |
| **PPL** (Page Protection Layer) | Separates kernel page table management | Very Hard — limited attack surface |
| **KTRR** (Kernel Text Read-only Region) | Hardware-enforced read-only kernel text | Extremely Hard — hardware level |
| **AMFI** (Apple Mobile File Integrity) | Code signing enforcement | Bypassed via kernel patching |
| **Sandbox** (Seatbelt) | Per-process capability restrictions | Medium — escape via IPC/mach vulnerabilities |
| **SSV** (Signed System Volume) | Cryptographic verification of system volume | Bypassed via rootless approaches |

## Lockdown Mode as Entry Vector — Hypotheses

### Hypothesis 1: WebKit Interpreter Path
When JIT is disabled, WebKit falls back to the LLInt (Low-Level Interpreter) or a simplified tier. This path:
- Gets less fuzzing attention than the JIT path
- May have different bounds checking
- Has different memory management patterns
- Could have integer overflow bugs in bytecode handling

### Hypothesis 2: USB/Accessory Authentication
The new approval flow for accessories adds:
- A UI prompt mechanism that must communicate with the USB stack
- State tracking for "device locked" vs "device unlocked + approved"
- Potential for TOCTOU (time-of-check-time-of-use) between approval and connection

### Hypothesis 3: Lockdown Mode Toggle State Machine
Enabling/disabling Lockdown Mode requires a reboot. During the toggle:
- Multiple daemons must be notified
- Configuration state must be atomically updated
- What if the state is inconsistent between subsystems?

### Hypothesis 4: Network Stack Changes
Disabling 2G/3G and changing Wi-Fi behavior means:
- The baseband processor receives different configuration
- The Wi-Fi driver has different auto-join logic
- Network policy enforcement has new rules

## Research Plan

### Phase 1: Information Gathering
- [ ] Identify how Lockdown Mode state is stored and queried (likely a system preference or nvram flag)
- [ ] Map which daemons/processes check Lockdown Mode state
- [ ] Find the WebKit code paths that differ with JIT disabled
- [ ] Research recent iOS CVEs related to Lockdown Mode components
- [ ] Study recent jailbreak techniques (Dopamine, palera1n approach on newer hardware)

### Phase 2: Attack Surface Mapping
- [ ] Enumerate all Mach ports and IPC interfaces that change behavior
- [ ] Identify kernel extensions/kexts involved in Lockdown Mode
- [ ] Map the USB stack authentication flow changes
- [ ] Analyze the Wi-Fi/cellular stack configuration differences

### Phase 3: Vulnerability Research
- [ ] Fuzz the WebKit interpreter path
- [ ] Test Lockdown Mode enable/disable race conditions
- [ ] Analyze IPC message handling in affected daemons
- [ ] Look for type confusion in Lockdown Mode state checks

## Tools Needed
- Disassembler (Ghidra, IDA, Hopper) for analyzing iOS binaries
- Frida for runtime instrumentation (requires developer mode)
- iPhone Duo with developer mode enabled
- Xcode + iOS 27 SDK for building test harnesses
- Crash log analysis (symbolication)

## References
- Apple Lockdown Mode: https://support.apple.com/en-us/105120
- Project Zero iOS research: https://googleprojectzero.github.io/0days-in-the-wild/
- checkm8 (hardware exploit, A5-A11): https://github.com/axi0mX/checkm8
- Recent PAC bypass research: Various academic papers
- iOS kernel cache analysis tools

## CVE-2024-23222 Deep Analysis (Coruna Entry Point)

### Vulnerability: DFG Concurrent Compilation TOCTOU

**Affected code**: `Source/JavaScriptCore/dfg/DFGGraph.cpp` → `tryGetConstantProperty()`
**Vulnerable commit**: `77a6809901292311fc674150434d829d3dd2bf0c`
**Fix commit**: `64714692967ad278155fcae66c5cb0f853b3bf34`

### Root Cause

DFG (Data Flow Graph) compilation runs on a **background thread**. When the compiler encounters a property access on an object it believes is constant, it calls `tryGetConstantProperty()`:

1. Takes the object's `cellLock`, reads the current structure
2. Verifies the structure is in the expected structure set
3. Reads the property value via `getDirectConcurrently()`
4. Returns the "constant" value to fold into compiled code

**The flaw**: Between when the compiler reads the value (step 3) and when compiled code is installed/runs on the main thread, the object can be mutated:
- Structure transitions happen
- Property values change
- The referenced cell gets freed by GC

Result: compiled code uses a dangling pointer → **use-after-free**

### The Fix (3 key changes)

1. **New `DFGDesiredObjectProperties` class**: Records `{object, offset} → {value, structure}` pairs — what the compiler assumed during constant folding

2. **Main-thread validation in `Plan::isStillValidOnMainThread()`**: Before installing compiled code, verify that all assumed property values are still correct. If any changed, discard the compilation.

3. **Narrowed constant folding conditions**: If structure transitions aren't being watched, only allow folding when the structure set has exactly 1 entry (prevents transitivity attacks where an object transitions through intermediate structures).

### PoC Mechanics

**`poc_det3.js` (deterministic, needs `--useConcurrentJIT=0`)**:
```
1. Create: s = { val: siblings[0], pad: 13 }; Object.seal(s)
2. Hot loop: read s.val 60,000 times → DFG constant-folds s.val
3. Mid-loop (at iteration 16,000): set s.val = 0, free siblings, force GC
4. DFG-compiled code still reads freed cell → crash/corruption
```

**`race_v7.js` (natural race, no flags needed)**:
```
Same concept but tries multiple timing windows (K=1500..48000)
and multiple GC storm rounds to hit the race condition naturally.
```

### Why This Matters for iOS 27 Research

CVE-2024-23222 itself is patched on iOS 27. But it teaches us:
- **JIT compiler TOCTOU bugs are a class of vulnerability**, not a one-off
- The DFG/FTL compilation pipeline has inherent concurrency risks
- Lockdown Mode disables JIT → different code path → may have similar logic bugs in the interpreter tier
- The exploit primitive (type confusion → arbitrary R/W) is the same pattern needed for any jailbreak

### Hardware Test Plan
- **iPad Mini (iOS 27.0.1)**: Primary test device — lower risk
- **iPhone Duo (iOS 27.1)**: Target device — only after iPad Mini validation

## iOS 27.2 Endpoint Security Analysis (Calif.io Research)

**Source**: Josh Maine, Calif.io, September 18, 2026 — "Apple Internals: Endpoint Security ships in iOS 27.2 beta"

### What Shipped

iOS 27.2 beta introduces the Endpoint Security (ES) framework to iOS via:
- **KEXT**: `com.apple.iokit.EndpointSecuritySE` — kernel extension with MAC policy hooks
- **Dylib**: `libEndpointSecurity.dylib` — userspace client library

### Key Findings: Completely Inert on Retail Devices

1. **Research Mode gate**: Both `esk_kext_start()` and `EndpointSecurityDriver::init()` check `research_mode_state()` AND `extended_research_mode_state()` before activating. Only Security Research Devices (SRDs) pass this check.

2. **Notify-only**: The subscription filter allows only NOTIFY events (37 event types). No AUTH events — ES cannot block or deny operations, only observe them.

3. **Single client**: Only `watchdogd` holds the `.embeddedclient` entitlement required for Type-1 connections. No other binary can connect.

4. **watchdogd doesn't subscribe**: Despite importing `es_new_client` and `es_delete_client`, watchdogd has no calls to `es_subscribe` — it creates a client but never subscribes to any events.

### MAC Policy: 42 Hooks

The KEXT registers a MAC policy with 42 populated hooks covering:
- **Process lifecycle**: `proc_check_fork`, `proc_check_signal`, `proc_check_wait`, `proc_notify_exit`
- **File operations**: `vnode_check_open`, `vnode_check_rename`, `vnode_check_unlink`, `vnode_check_write`, etc.
- **Task/thread control**: `proc_check_get_task`, `proc_check_get_movable_control_port`, `proc_check_set_thread_state`, `proc_check_set_cs_info`
- **New 27.2 events**: exception ports, special ports, `thread_set_state`, `set_cs_info`, `map_anon`, `sigaction`
- **Credential changes**: `cred_check_label_update`

### Subscription Filter Implementation

The filter uses a 172-bit set checked against three 64-bit forbid masks:
- Events 0–63: mask `0x9000040000006c08` (forbids AUTH events like ES_EVENT_TYPE_AUTH_EXEC)
- Events 64–127: mask `0x0000700000000020`
- Events 128–171: mask `0x0000000000000018`

Any event whose bit is set in the forbid mask is rejected at subscription time.

### Connection Requirements (Type-1)

1. Must be **root** (UID 0)
2. Must hold **`com.apple.developer.endpoint-security.embeddedclient`** entitlement
3. Must pass **TCC check** (Transparency, Consent, and Control)
4. Only then can call `es_new_client` to create a client handle

### Implications for Jailbreak Research

- **Not an attack surface on retail**: The research-mode gate means ES is dead code on consumer devices. No value as an exploitation target.
- **Signals future direction**: Apple is bringing macOS-style endpoint telemetry to iOS. The spring .3/.4 releases historically ship bigger security changes — ES may become more functional there.
- **SRD value**: For researchers with SRDs, the notify-only ES provides visibility into kernel-level operations that was previously unavailable on iOS.
- **Related research**: Calif.io also discovered "Java4Shell" and "Swift-in-the-Kernel" — both pointing to increasing kernel-level instrumentation on iOS.

## Notes
- The reported jailbreak used Lockdown Mode — this narrows the attack surface significantly
- iPhone Duo likely uses A-series or M-series chip with full PAC/PPL/KTRR
- iOS 27 builds on years of hardening — any exploit is likely a logic bug, not a memory corruption
