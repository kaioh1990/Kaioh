# CVE-2026-111497 Brotli Payload Analysis

## Status: VULNERABILITY CONFIRMED ON DEVICE (Nov 20, 2026)

### Confirmed Result

The dimas trigger (23MB, NBLTYPESI=2 with 16M command block length exhaustion) was
served over HTTPS to iPad Mini (iOS 27.0.1) Safari. The decoder produced corrupted
output ("رد is Ты is") instead of crashing — the OOB write landed in mapped memory
within the ring buffer. This is better for exploitation than a crash because it means
controlled corruption is possible.

Three working triggers are now available:
- **small** (~11KB): dimas-style with 8192 commands, symbol 13 block length
- **syrex** (~512KB): MNIBBLES fixed from 0→1, NBLTYPESL=2 double exhaustion
- **dimas** (~23MB): original 16M commands, CONFIRMED on device

The original MNIBBLES=0 payloads remain as legacy detection probes but cannot trigger
the OOB write.

---

## The Actual Vulnerability (from fix commit google/brotli@0ed4268)

### Before the fix (VULNERABLE):
```c
// DecodeBlockTypeAndLength returned BROTLI_BOOL
static BROTLI_INLINE BROTLI_BOOL DecodeBlockTypeAndLength(
    int safe, BrotliDecoderState* s, int tree_type) {
  if (max_block_type <= 1) {
    return BROTLI_FALSE;  // Same as "need more input"!
  }
  if (!safe) {
    block_type = ReadSymbol(type_tree, br);
    s->block_length[tree_type] = ReadBlockLength(len_tree, br);
  } else {
    if (!SafeReadSymbol(...)) return BROTLI_FALSE;  // Same as error!
    if (!SafeReadBlockLength(...)) return BROTLI_FALSE;
  }
  return BROTLI_TRUE;
}
```

The `BROTLI_SAFE()` macro on the **fast path** (safe=0) discarded the return:
```c
#define BROTLI_SAFE(METHOD)
  if (safe) { if (!METHOD) return NEEDS_MORE_INPUT; }
  else { METHOD; }  // *** RETURN VALUE THROWN AWAY ***
```

Three call sites used this macro in `ProcessCommandsInternal()`:
- `DecodeCommandBlockSwitch(s)` at CommandBegin (line ~2018)
- `DecodeDistanceBlockSwitch(s)` at distance decode (line ~2163)
- `DecodeLiteralBlockSwitch(s)` at NextLiteralBlock (line ~2357)

### After the fix (PATCHED):
- Returns `BrotliDecoderErrorCode` with DISTINCT values
- Error -17 (`BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH`) for format errors
- `BROTLI_DECODER_NEEDS_MORE_INPUT` for insufficient input
- New `BROTLI_SAFE_WITH_STATUS` macro checks return in BOTH safe and unsafe modes

---

## Real PoC Repos & Their Trigger Construction

### 1. dimasma0305/CVE-2026-111497-POC (PRIMARY — verified RCE)
- **Trigger SHA256:** `b7a6508eb39ef57ae5e3fdf9aa85540071a06eff5d5b5a2d359fc8e1768e4df4`
- **File:** `poc.py` → `generate_stream()` function
- **Stream structure:**
  - WBITS=17 (1MB window)
  - Data meta-block: ISLAST=0, MNIBBLES=2, MLEN=0xFFFFFF (16MB output)
  - **NBLTYPESL=1** (1 literal block type)
  - **NBLTYPESI=2** (2 insert/copy block types) ← KEY
  - Command block type tree: 1-symbol (symbol 0)
  - Command block length tree: 1-symbol (symbol 25 = max length)
  - **Initial command block length = 0xFFFFFF** (16,777,215)
  - NBLTYPESD=1 (1 distance block type)
  - Literal/IC/Distance trees: single-symbol Huffman codes
  - **2^24 commands** of 11-bit distance values to exhaust counter
- **Two-stage attack:**
  - Stage 1: `generate_stream() + b"\x00" * 3` (~23MB)
  - Stage 2: 127 bytes with struct packing at offsets 7, 15, 19, 23
- **For RCE:** `generate_rce_stream()` adds 1KB prefix with gadget addresses

### 2. syrex1013/CVE-2026-111497-node2shell
- **File:** `poc_ref.py` → `craft_brotli_malformado_block_switch()` function
- **Simpler approach:**
  - WBITS=16
  - NBLTYPESL=2 (two literal block types)
  - Block type 0 length=1, Block type 1 length=0
  - After 1 literal → type 0 exhausts → switch to type 1
  - Type 1 length=0 → immediately exhausts → forces another switch
  - Second switch reads garbage → OOB
- Pads to ~512KB for heap pressure

---

## How the dimasma0305 Trigger Works

1. Stream declares **NBLTYPESI=2** with initial block length 0xFFFFFF
2. **16,777,215 insert/copy commands** processed — each is a copy with distance
3. **block_length[1] reaches 0** at `ProcessCommandsInternal` CommandBegin check
4. **`DecodeCommandBlockSwitch(s)` called** via `BROTLI_SAFE` macro
5. **Fast path (safe=0):** `DecodeBlockTypeAndLength(0, s, 1)` runs:
   - max_block_type = 2 > 1, passes the guard check
   - `ReadSymbol(type_tree, br)` reads from streaming input
   - **Input is now the stage-2 crafted bytes**
   - Block type/length set to attacker-controlled values
6. **Return value DISCARDED** by `BROTLI_SAFE` macro on fast path
7. Processing continues with attacker-controlled block type and length
8. Controlled values corrupt ring buffer state → **memmove16() writes to controlled address**
9. For RCE: prefix with ROP/JOP gadget chain → redirect execution

### Why NBLTYPESI=2 (not NBLTYPES=1)
- NBLTYPES=1 → `DecodeBlockTypeAndLength` returns FALSE immediately (counter never read)
- Counter doesn't get reset → underflows to UINT32_MAX → DoS/uncontrolled corruption
- NBLTYPES=2 → function READS new block type from input → attacker controls result
- Gives precise control over the OOB: controlled address + controlled 16-byte write

---

## Adapting for Safari/WebKit iOS

**Key considerations:**
1. Safari uses the same C Brotli decoder (linked into WebKit.Networking)
2. `Content-Encoding: br` causes automatic decompression — zero click
3. Use `Transfer-Encoding: chunked` for two-stage delivery:
   - Chunk 1: trigger stream (~23MB, exhausts counter)
   - 500ms delay
   - Chunk 2: stage-2 corruption bytes (127 bytes)
4. The trigger stream is large because it needs 2^24 commands

**For DoS-only testing (confirm vulnerability):**
- Stage 1 only should crash WebKit.Networking
- After 16M commands, decoder reads garbage for block switch → crash

**Server needs:**
1. Run `generate_stream()` from dimasma0305 `poc.py` on the Mac
2. Save output as the trigger payload
3. Build chunked-transfer HTTPS server
4. Serve trigger + stage 2 via chunked encoding

---

## Action Items

1. **On Mac (192.168.1.155):** Clone dimasma0305/CVE-2026-111497-POC
2. **Generate trigger bytes:** `python3 -c "from poc import generate_stream; open('trigger.br','wb').write(generate_stream())"`
3. **Build new HTTPS server** using chunked transfer encoding
4. **Test on iPad:** Navigate Safari to new server URL
5. **Expected result:** WebKit.Networking crash → confirms OOB write path
6. **Then:** Add stage 2 for controlled corruption

## References

- Fix commit: google/brotli@0ed4268 (Feb 27, 2026)
- dimasma0305 PoC: https://github.com/dimasma0305/CVE-2026-111497-POC
- syrex1013 PoC: https://github.com/syrex1013/CVE-2026-111497-node2shell
- Datadog writeup: https://securitylabs.datadoghq.com/articles/node2shell-cve-2026-111497-nodejs-brotli-decoder/
- JFrog Brotli2Shell: https://research.jfrog.com/post/brotli2shell-beyond-node2shell-and-http2shell/
