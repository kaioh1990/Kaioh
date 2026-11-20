# Brotli PoC Testing Instructions

## VULNERABILITY CONFIRMED (Nov 20, 2026)

**CVE-2026-111497 is CONFIRMED EXPLOITABLE on iPad Mini (iOS 27.0.1).**

The dimas trigger (23MB, NBLTYPESI=2) was served over HTTPS and Safari's Brotli
decoder produced corrupted output ("رد is Ты is") — the OOB write landed in
mapped memory within the ring buffer. Three triggers are now available:

| Trigger | Size | Mechanism | Status |
|---------|------|-----------|--------|
| **small** | ~11KB | dimas-style, 8192 commands | NEW — fastest test |
| **syrex** | ~512KB | NBLTYPESL=2 double exhaustion | FIXED (MNIBBLES was 0) |
| **dimas** | ~23MB | NBLTYPESI=2, 16M commands | CONFIRMED on device |

## Quick Start

On iPad Safari, navigate to: `http://192.168.1.155:8889/`

### Real Triggers (use these first)

1. Tap **small (11KB)** — fastest test, dimas-style with 8192 commands
2. Tap **syrex (512KB)** — NBLTYPESL=2 double exhaustion (MNIBBLES fixed)
3. Tap **dimas (23MB)** — 16M commands, CONFIRMED on iPad iOS 27.0.1

**IMPORTANT:** Safari requires HTTPS for `Content-Encoding: br` delivery.
Use the HTTPS server on port 4446: `https://192.168.1.155:4446/`
The DS (DecompressionStream) API does NOT support 'br' on iOS 27.0.1 Safari.

### Option B: HTTPS Test (needs cert trust)

#### Step 1: Install self-signed certificate on iPad

1. On iPad Safari, navigate to: `https://192.168.1.155:4444/`
2. Safari will show "This Connection Is Not Private" — tap **Show Details**
3. Tap **visit this website**
4. If prompted, tap **Visit Website**
5. A profile installation prompt may appear — go to **Settings > General > VPN & Device Management** and install the profile
6. Go to **Settings > General > About > Certificate Trust Settings**
7. Toggle ON trust for the "192.168.1.155" certificate

#### Step 2: Test Brotli payloads

1. Navigate to: `https://192.168.1.155:4444/`
2. The HTTPS server has the original detection probes (not the real triggers yet)
3. For real triggers, use the HTTP v4 server with DS buttons

## What to Watch For

| Outcome | Meaning |
|---------|---------|
| **Corrupted/garbled text** | OOB write landed in mapped memory — **EXPLOITABLE** (confirmed!) |
| **Safari crashes** | OOB write hit unmapped memory — VULNERABILITY CONFIRMED |
| **Tab dies / "webpage reloaded"** | WebKit.Networking crashed — VULNERABILITY CONFIRMED |
| **Empty response** | Decoder process crashed — likely VULNERABLE |
| **"cannot decode raw data"** | MNIBBLES=0 bug (syrex old version) or decoder error |
| **"Load failed"** | HTTPS cert not trusted, or plain HTTP (Safari needs HTTPS for br) |

## After Testing

If a crash is confirmed:
1. Connect iPad via USB
2. Run: `pymobiledevice3 crash ls` to list crash logs
3. The crash log will show the faulting address in WebKit.Networking
4. This confirms the entry point for our exploit chain

## Server Status

Servers running on the Mac (192.168.1.155):

```bash
# Check all servers
lsof -iTCP -sTCP:LISTEN -P -n | grep -E ':(4443|4444|8888|8889|8890)'

# Restart v4 HTTP Brotli trigger (port 8889) — REAL TRIGGERS
cd /Users/ttameirao/jailbreak-research/CVE-2026-111497
python3 brotli_v4_server.py &
# Note: dimas payload generation takes ~30-60s at startup

# Restart HTTPS Brotli trigger (port 4444)
python3 brotli_https_server.py 4444 &

# Restart PDF server (port 8888)
cd /Users/ttameirao/jailbreak-research/CVE-2026-86950/trigger
python3 -m http.server 8888 &
```

## Payload Details

### Real Triggers (v4 server)

| Name | Size | Mechanism |
|------|------|-----------|
| **syrex** | ~512KB | NBLTYPESL=2, block type 0 len=1 + type 1 len=0. Double exhaustion forces DecodeBlockTypeAndLength on stale Huffman state. |
| **dimas** | ~23MB | NBLTYPESI=2, initial command block length=0xFFFFFF (16M). After exhaustion, decoder reads attacker-controlled block switch data via memmove16(). |

### Legacy Probes (kept for comparison)

| Name | Size | Note |
|------|------|------|
| detect-base | 22B | MNIBBLES=0 — cannot trigger the bug |
| detect-ext | 23B | MNIBBLES=0 — cannot trigger the bug |
| oob-v2-0..2 | 238B | Bit-flipped valid streams — unlikely to trigger OOB |
