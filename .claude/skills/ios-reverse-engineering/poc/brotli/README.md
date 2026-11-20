# CVE-2026-111497 — Brotli Decoder OOB Write

## Overview
Out-of-bounds write in Google Brotli C decoder (`c/dec/decode.c`).
Invalid block-type switch in `DecodeBlockTypeAndLength()` misreported as
"need more input" (BROTLI_NEEDS_MORE_INPUT) instead of format error.
Decoder continues with stale Huffman state → OOB write on ring buffer.

**CWE**: CWE-787 (Out-of-bounds Write), CWE-252 (Unchecked Return Value)
**CVSS**: 9.2 (Google CNA) / 8.1 (Red Hat) / High (Node.js)
**CISA KEV**: Yes (added October 27, 2026) — actively exploited in the wild

## Upstream Fix
- Brotli 1.0.10, 1.1.1, 1.2.1 (October 21, 2026)
- Fix commits: google/brotli `0ed4268`, `50a9b70`, `b418fa8`, `da98063`
- Fix adds proper error checking for `DecodeBlockTypeAndLength()` return value

## iOS Status
- iOS 27.0.1 (Build 24A446): **VULNERABLE** — Apple's libcompression uses Brotli < 1.2.1
- iOS 27.1 (Build 24A94232): **VULNERABLE**
- iOS 27.2 (Oct 26, 2026): **VULNERABLE** (per third-party brotli-lab testing)
- iOS 27.2.1 (Build 24B156, Nov 9, 2026): **PATCHED**
- iOS 27.3 beta 1: **VULNERABLE** (per third-party report)

## Attack Vector
HTTPS response with `Content-Encoding: br` containing crafted Brotli stream.
Safari decompresses automatically in WebKit.Networking process.
Zero-click — just load a URL. Works through Lockdown Mode.

Alternative: JavaScript `DecompressionStream` API works over HTTP (no cert needed).

## The Bug: Block-Type Switch Mishandling

In `c/dec/decode.c`, the `DecodeBlockTypeAndLength()` function returns
`BROTLI_FALSE` for format errors, which is the same value as "need more
input" on the fast path. The decoder doesn't distinguish these cases and
continues with stale Huffman tree pointers:

```
1. Decoder processes a single-block-type Brotli stream
2. Crafted bytes trigger an invalid block-type switch
3. DecodeBlockTypeAndLength() returns BROTLI_FALSE (format error)
4. Caller treats this as "need more input" on the fast path
5. Decoder continues with stale Huffman state from previous block
6. Stale state causes writes to ring buffer at attacker-influenced offsets
```

## Primitive
- Ring buffer OOB write
- Decoder state is ~230KB heap allocation
- Corrupted state controls both read and write positions (per Datadog analysis)
- Attacker-controlled offset from decoder state struct

## Detection
- **Patched builds**: Node.js reports `ERR__ERROR_FORMAT_BLOCK_SWITCH` (-17)
- **Patched builds**: Python reports "brotli: decoder is unhealthy"
- **Vulnerable builds**: Decoder processes the malformed stream without error

## Public Research & PoCs
- **dimasma0305/CVE-2026-111497-POC**: Public PoC with RCE harness (Oct 22, 2026)
- **TimmyTurner51/brotli-lab**: iOS-specific research harness, tested on iOS 27.0.1
  - Staged design: Brotli trigger → IOKit escalation stage
  - Reports code execution on macOS 27.0.1
- **Datadog Security Labs**: Detailed reproduction writeup (Node2Shell)
- **TheFool-TAROT/CVE-2026-111497-brotli-pwn**: Docker lab with PoC
- **Arkango/CVE-2026-111497**: Independent research with Express RCE analysis
- **ikow/brotli2crash**: Disputes RCE reachability, argues MLEN_MAX caps prevent it

## Exploit Chain for iOS 27 Jailbreak

```
Step 1: CVE-2026-111497 Brotli OOB write
  → Code execution in WebKit.Networking process
  │
Step 2: Sandbox escape
  → CVE-2026-113897 (WebKit Process Model, fixed 27.2) — PRESENT on 27.0.1
  → OR: IOSurface CVE-2026-112477 race condition — PRESENT on 27.0.1
  │
Step 3: Kernel exploit
  → CVE-2026-65350 (kernel UAF) — PRESENT on 27.0.1
  → OR: CVE-2026-112507 (kernel OOB write) — PRESENT on 27.0.1
  │
Step 4: PAC bypass → Trust cache injection → Sileo
```

## Files
- `brotli_trigger_server.py` — HTTP server with landing page + Brotli payloads
- `crash_server.py` (on Mac) — HTTPS server for Content-Encoding: br testing
- `../references/webkit-sandbox-escape.md` — Sandbox escape strategy document

## Requirements for Testing
- HTTPS for Content-Encoding: br (Safari only sends Accept-Encoding: br over HTTPS)
- Self-signed cert trusted on test device, or use HTTP + DecompressionStream JS API
- Alternative: Use HTTP server (port 8889) with JS-based trigger (no cert needed)

## References
- [Rapid7 Advisory](https://www.rapid7.com/blog/post/etr-cve-2026-111497-critical-out-of-bounds-write-in-the-node-js-brotli-decoder/)
- [Red Hat RHSB-2026-015](https://access.redhat.com/security/vulnerabilities/RHSB-2026-015)
- [Tenable FAQ](https://www.tenable.com/blog/brotli2shell-frequently-asked-questions-for-google-brotli-decoder-cve-2026-111497)
- [Datadog Reproduction](https://securitylabs.datadoghq.com/articles/node2shell-cve-2026-111497-nodejs-brotli-decoder/)
- [Broccoli2Shell](https://broccoli2shell.com/)
- [Stefan Esser: Brotli → WebKit.Networking](https://x.com/i0n1c/status/2120230223979672037)
- [brotli-lab iOS harness](https://github.com/TimmyTurner51/brotli-lab)
- [dimasma0305 PoC](https://github.com/dimasma0305/CVE-2026-111497-POC)
