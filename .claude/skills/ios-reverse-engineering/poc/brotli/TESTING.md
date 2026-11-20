# Brotli PoC Testing Instructions

## Quick Start

When you wake up, here's what to do:

### Option A: HTTP Test (No cert needed, but may not trigger Brotli decompression)

1. On iPad Safari, navigate to: `http://192.168.1.155:8889/`
2. Tap each payload button
3. Watch for crashes or error responses

**Note:** Safari may not decompress Brotli over HTTP. If all results show "NETWORK ERROR",
try Option B.

### Option B: HTTPS Test (Proper Brotli trigger, needs cert trust)

#### Step 1: Install self-signed certificate on iPad

1. On iPad Safari, navigate to: `https://192.168.1.155:4444/`
2. Safari will show "This Connection Is Not Private" — tap **Show Details**
3. Tap **visit this website**
4. If prompted, tap **Visit Website**
5. A profile installation prompt may appear — if so, go to **Settings > General > VPN & Device Management** and install the profile
6. Go to **Settings > General > About > Certificate Trust Settings**
7. Toggle ON trust for the "192.168.1.155" certificate

#### Step 2: Test Brotli payloads

1. Navigate to: `https://192.168.1.155:4444/`
2. You should see the trigger page with payload buttons
3. Tap **oob-base** first
4. If no crash, tap **Fire ALL**
5. Also try the **DecompressionStream** button (uses JS API, different code path)

### Option C: DecompressionStream Test (No cert, no HTTPS, JS API path)

1. Navigate to: `http://192.168.1.155:8889/` (or wherever the DS test page is served)
2. This page uses JavaScript's `DecompressionStream('br')` API
3. The raw bytes are embedded in the page — no network decompression needed
4. This tests whether the same Brotli decoder is used for the JS API

## What to Watch For

| Outcome | Meaning |
|---------|---------|
| **Safari crashes** | OOB write hit unmapped memory — VULNERABILITY CONFIRMED |
| **Tab dies / "webpage reloaded"** | WebKit.Networking crashed — VULNERABILITY CONFIRMED |
| **Empty response** | Decoder process crashed — likely VULNERABLE |
| **Red "OOB HIT" text** | Decoder continued past invalid block switch — VULNERABLE |
| **Green "PATCHED" text** | Decoder caught error -17 — PATCHED (unlikely on 27.0.1) |
| **Yellow "NETWORK ERROR"** | Safari blocked decompression — try different method |

## After Testing

If a crash is confirmed:
1. Connect iPad via USB
2. Run: `pymobiledevice3 crash ls` to list crash logs
3. The crash log will show the faulting address in WebKit.Networking
4. This confirms the entry point for our exploit chain

## Server Status

Servers should be running on the Mac. If they're not:

```bash
# Check
lsof -iTCP -sTCP:LISTEN -P -n | grep -E ':(4443|4444|8888|8889)'

# Restart HTTP Brotli trigger (port 8889)
cd /Users/ttameirao/jailbreak-research/CVE-2026-111497
python3 brotli_trigger_server.py 8889 &

# Restart HTTPS Brotli trigger (port 4444)
python3 brotli_https_server.py 4444 &

# Restart PDF server (port 8888)
cd /Users/ttameirao/jailbreak-research/CVE-2026-86950/trigger
python3 -m http.server 8888 &
```
