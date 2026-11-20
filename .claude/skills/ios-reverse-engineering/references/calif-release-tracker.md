# Calif.io Release Tracker

## Chollima Jailbreak — Kernel Exploit Release Watch

### Timeline
- **Oct 16, 2026**: Hexacon demo — untethered JB on iPhone 18 Pro, iOS 27.0.1 (24A446)
- **Oct 23, 2026**: iPhone Duo (iOS 27.1) also demonstrated jailbroken
- **Oct 26, 2026**: iOS 27.2 released — fixes "most" Chollima vulnerabilities
- **Early Nov 2026**: Anna Groza (pwnlambda) says kernel R/W writeup "within about a week"
- **Mid-Nov 2026**: Release DELAYED — no new ETA provided
- **Nov 9, 2026**: iOS 27.2.1 released — fixes CVE-2026-111497 (Brotli)
- **Nov 2026**: Calif demos Brotli-based jailbreak on iOS 27.2 (iPhone, iPad, Mac)
- **Nov 2026**: Persistence attack survives iOS update + reboot on Lockdown Mode device

### What We're Waiting For
1. **Kernel R/W exploit code + writeup** — the most critical piece
   - Will target iOS 27.0.1 specifically
   - May use CVE-2026-65350 (kernel UAF) or CVE-2026-112477 (IOSurface race)
   - opa334 plans to integrate into Dopamine
2. **SPTM bypass** — needed for A14+ devices (including our A17 Pro iPad Mini)
   - Community tracker claims bypass exists for 27.0.1, patched in 27.2 beta
   - Not independently verified
3. **Persistence mechanism** — Apple hasn't patched yet per Esser

### Where to Watch
- Stefan Esser (@i0n1c on X/Twitter): https://x.com/i0n1c
- Anna Groza (@pwnlambda on X/Twitter): https://x.com/pwnlambda  
- Josh Maine (@0xjmaine on X/Twitter)
- Calif.io website/blog: https://calif.io
- Dion Blazakis (@halvarflake on X/Twitter)
- opa334 (Dopamine): https://github.com/opa334
- Apple Wiki Chollima page: https://theapplewiki.com/wiki/Chollima
- PBX Science coverage: https://pbxscience.com
- OneJailbreak: https://onejailbreak.com
- iMaxTomas tracker: https://github.com/iMaxTomas/ios27-jb-tracker

### Captive Portal Attack Vector
Calif's Brotli attack uses a captive portal as trigger:
- Victim connects to attacker Wi-Fi
- Captive portal page loads automatically
- Page responds with Content-Encoding: br containing malicious Brotli stream
- WebKit.Networking decompresses → OOB write → code execution
- Zero-click, zero-interaction, works through Lockdown Mode

This is the same attack vector we're developing with our trigger server.

### Our Device Compatibility
| Device | Build | Same as Chollima Demo? |
|--------|-------|----------------------|
| iPad Mini A17 Pro | iOS 27.0.1 (24A446) | **YES — exact match** |
| iPhone Duo | iOS 27.1 (24A94232) | YES — Calif also demoed on 27.1 |
| iPhone 15 Pro | iOS 27.2.1 (24B156) | NO — most Chollima vulns patched |

### Integration Plan
When Calif releases kernel exploit:
1. Download and study the writeup
2. Adapt for our iPad Mini (same build = should work directly)
3. Combine with our Brotli entry point
4. If SPTM bypass also released → full chain to Sileo
5. If no SPTM bypass → data-only exploitation or wait for Dopamine integration
