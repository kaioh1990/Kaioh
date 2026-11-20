# CVE-2026-111497 Brotli PoC Analysis — Complete Findings

## Critical Finding: Existing Payloads Are WRONG

The payloads currently deployed on the Mac servers (ports 8889, 4444) all start with
byte `0x20` which decodes as:
- Bit 0 = 0 → WBITS=16
- Bit 1 = 0 → ISLAST=0  
- Bits 2-3 = 00 → **MNIBBLES=0 (empty/skip meta-block)**

An empty meta-block never reaches the block-type decoding logic. These payloads
**cannot trigger CVE-2026-111497**.

---

## The Actual Vulnerability (from fix commit 0ed4268)

### Before the fix (VULNERABLE):
```c
// DecodeBlockTypeAndLength returned BROTLI_BOOL
static BROTLI_INLINE BROTLI_BOOL DecodeBlockTypeAndLength(
    int safe, BrotliDecoderState* s, int tree_type) {
  if (max_block_type <= 1) {
    return BROTLI_FALSE;  // Same value for "format error" and "need more input"!
  }
  // ... safe path also returned BROTLI_FALSE for both cases
}

// Callers on the FAST PATH (safe=0) used BROTLI_SAFE macro:
#define BROTLI_SAFE(METHOD)
  {
    if (safe) {
      if (!METHOD) return BROTLI_DECODER_NEEDS_MORE_INPUT;
    } else {
      METHOD;  // *** RETURN VALUE COMPLETELY DISCARDED ***
    }
  }

// Three call sites that discarded the error:
BROTLI_SAFE(DecodeCommandBlockSwitch(s));      // line ~2018
BROTLI_SAFE(DecodeDistanceBlockSwitch(s));     // line ~2163  
BROTLI_SAFE(DecodeLiteralBlockSwitch(s));      // line ~2357
```

### After the fix (PATCHED):
```c
// Returns BrotliDecoderErrorCode with DISTINCT values
static BROTLI_INLINE BrotliDecoderErrorCode DecodeBlockTypeAndLength(...) {
  if (max_block_type <= 1) {
    return BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH;  // -17, NEW error code
  }
  // safe path returns BROTLI_DECODER_NEEDS_MORE_INPUT (distinct from -17)
}

// New macro that checks BOTH paths:
#define BROTLI_SAFE_WITH_STATUS(METHOD)
  {
    BrotliDecoderErrorCode status;
    if (safe) { status = Safe##METHOD; }
    else { status = METHOD; }  // *** NOW CHECKS RETURN ***
    if (status != BROTLI_DECODER_SUCCESS) {
      result = status;
      goto saveStateAndReturn;
    }
  }
```

---

## Real PoC Repos Found

### 1. dimasma0305/CVE-2026-111497-POC (PRIMARY)
- **Status:** Verified RCE harness, tested on Node.js 20/22/24
- **Trigger SHA256:** `b7a6508eb39ef57ae5e3fdf9aa85540071a06eff5d5b5a2d359fc8e1768e4df4`
- **Mechanism:** Two-stage attack via `generate_stream()` + `trigger_stages()`
- **Key parameters in `generate_stream()`:**
  - WBITS=17 (1-bit flag + 3-bit code 0 + 1-bit selector)
  - Data meta-block: ISLAST=0, MNIBBLES=2, MLEN=0xFFFFFF (16MB output)
  - **NBLTYPESL=1** (1 literal block type)
  - **NBLTYPESI=2** (2 insert/copy block types) ← this is key
  - Command block type tree: 1-symbol tree (symbol 0)
  - Command block length tree: 1-symbol tree (symbol 25 = max length)
  - **Initial command block length = 0xFFFFFF** (16,777,215)
  - NBLTYPESD=1 (1 distance block type)
  - Literal tree: 1-symbol (byte 0x00)
  - Insert/copy tree: 1-symbol (symbol 130)
  - Distance tree: 1-symbol (symbol 184)
  - 2^24 commands of 11-bit distance_extra values to exhaust counter
- **Attack flow:**
  1. Stage 1: `generate_stream() + b"\x00" * 3` — exhausts block length counter
  2. Stage 2: 127 bytes with crafted offsets at bytes 7, 15, 19, 23 that corrupt decoder state
  3. For RCE: `generate_rce_stream()` adds a prefix with gadget addresses + callback shell

### 2. syrex1013/CVE-2026-111497-node2shell
- **BrotliBitWriter class** for constructing streams from scratch
- `craft_brotli_malformado_block_switch()` function:
  - WBITS=16
  - NBLTYPESL=2 (two literal block types)
  - Block type 0 length = 1 (exhausts after 1 literal)
  - Block type 1 length = 0 (immediately exhausts → forces DecodeBlockTypeAndLength)
  - After type 0 exhausts → switch to type 1 → type 1 immediately exhausts
  - Second switch attempt on NBLTYPESL=2 category reads invalid data → OOB
- Pads to ~512KB for heap pressure

---

## How the Trigger Actually Works (dimasma0305 approach)

1. **Stream declares NBLTYPESI=2** with initial block length 0xFFFFFF
2. **16,777,215 commands processed** — each is a copy with distance
3. **block_length[1] reaches 0** at `ProcessCommandsInternal` line ~2018
4. **`DecodeCommandBlockSwitch(s)` called** via old `BROTLI_SAFE` macro
5. **Fast path (safe=0):** `DecodeBlockTypeAndLength(0, s, 1)` runs:
   - max_block_type = 2 > 1, so passes the check
   - `ReadSymbol(type_tree, br)` reads from streaming input
   - **But input is the stage-2 crafted bytes!**
   - Block type/length are set to attacker-controlled values
6. **Return value DISCARDED** by `BROTLI_SAFE` macro on fast path
7. Processing continues with attacker-controlled block type and length
8. This corrupts the ring buffer pointer → **memmove16() writes to controlled address**
9. For RCE: prefix with gadget chain → redirect execution

### Why NBLTYPESI=2 (not NBLTYPES=1):
The PoC uses NBLTYPESI=2 because:
- With NBLTYPES=1, `DecodeBlockTypeAndLength` returns FALSE immediately (max_block_type ≤ 1)
- The old `BROTLI_SAFE` macro ignores this return on the fast path  
- But the counter doesn't get reset, so it underflows to UINT32_MAX
- This gives a DoS but less control over corruption

With NBLTYPESI=2:
- The function PROCEEDS to read a new block type from the input
- Attacker controls what gets read (stage 2 bytes)
- This gives precise control over block type, block length → controlled OOB

---

## Adapting for Safari/WebKit

For our iOS exploit chain, the trigger needs to work over HTTP Content-Encoding: br.

**Key differences from the Node.js PoC:**
1. **Safari uses the same C Brotli decoder** (linked into WebKit.Networking)
2. **Content-Encoding: br** causes automatic decompression
3. **No two-stage framing** — entire Brotli stream delivered in one HTTP response body
4. **But Transfer-Encoding: chunked** can achieve staging (send trigger in chunk 1, corruption data in chunk 2)

**Server modifications needed:**
1. Replace the MNIBBLES=0 payloads with `generate_stream()` output
2. Use chunked transfer encoding: first chunk = trigger, delay, second chunk = stage 2
3. The trigger stream is large (~23MB for the dimasma0305 version) because it needs to process 2^24 commands

**For DoS-only testing:**
- Just sending the trigger stream (stage 1) should crash WebKit.Networking
- After 16M commands, the decoder reads garbage for the block switch → crash

---

## Action Items for Parent Session

1. **Replace server payloads** — the MNIBBLES=0 payloads CANNOT trigger the bug
2. **Clone dimasma0305/CVE-2026-111497-POC** on the Mac and extract `generate_stream()` output
3. **Build a chunked-transfer HTTP server** that sends the Brotli stream in two stages:
   - Chunk 1: `generate_stream() + b"\x00" * 3`
   - Delay 500ms
   - Chunk 2: stage 2 corruption bytes (127 bytes with offsets at 7, 15, 19, 23)
4. **For Safari testing**, the simpler syrex1013 approach may work for DoS:
   - `craft_brotli_malformado_block_switch()` from poc_ref.py
   - Smaller payload, should crash the decoder
5. **Monitor for patching** — iOS 27.2.1 patches this, our iPad on 27.0.1 should be vulnerable

## References

- Fix commit: google/brotli@0ed4268 (Feb 27, 2026)
- dimasma0305 PoC: https://github.com/dimasma0305/CVE-2026-111497-POC
- syrex1013 PoC: https://github.com/syrex1013/CVE-2026-111497-node2shell
- Datadog writeup: https://securitylabs.datadoghq.com/articles/node2shell-cve-2026-111497-nodejs-brotli-decoder/
- JFrog Brotli2Shell: https://research.jfrog.com/post/brotli2shell-beyond-node2shell-and-http2shell/
- Imperva Node2Shell: https://www.imperva.com/blog/node2shell-from-brotli-data-to-remote-code-execution/
