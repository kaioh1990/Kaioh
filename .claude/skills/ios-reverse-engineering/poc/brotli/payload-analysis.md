# CVE-2026-111497 Brotli Payload Analysis

## Summary

The 4 trigger payloads are **vulnerability detection probes**, not OOB write exploits.
They exploit an invalid Brotli stream condition (ISLAST=1 + MNIBBLES=0) that the
**vulnerable** decoder mishandles — returning NEEDS_MORE_INPUT instead of FORMAT_ERROR.
They will NOT cause a Safari crash via memory corruption.

**To cause an actual OOB write, we need a different, more complex payload** (see "What's
Needed for OOB Write" at the end).

---

## Payload Structure

All 4 payloads share the same high-level structure:

```
Bytes:  20 00 10 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00
        ^     ^  ^                                                        ^
        |     |  |                                                        |
        WBITS |  Meta-block #2 starts here                           Trailing
              |
              Meta-block #1 metadata byte (skipped)
```

### Bit-Level Parse

**WBITS (bit 0):**
- bit[0] = 0 → WBITS = 16, window size = 65520 bytes

**Meta-block #1 (bits 1–23):**
- ISLAST = 0
- MNIBBLES = 0 → metadata meta-block (valid)
- RESERVED = 0
- MSKIPBYTES = 1
- MSKIPLEN = 0 → skip 1 byte (byte[2] = 0x10)
- Byte-align to bit 16, skip 1 byte to bit 24

**Meta-block #2 (bits 24+):**
- ISLAST = 1
- ISLASTEMPTY = 0
- MNIBBLES = 0

**THIS IS INVALID.** RFC 7932 Section 9.2: MNIBBLES = 0 is only valid when ISLAST = 0
(it signals a metadata meta-block, which is by definition not the last one).

### What Happens on Each Decoder

**FIXED decoder:**
```
ISLAST=1 + MNIBBLES=0 → BROTLI_DECODER_ERROR_FORMAT_RESERVED
  → returns error code -17 (ERR_FORMAT_BLOCK_SWITCH or similar)
  → clean error, no state corruption
```

**VULNERABLE decoder (iOS 27.0.1 libcompression):**
```
ISLAST=1 + MNIBBLES=0 → is_metadata = !is_last = FALSE
  → BUT size_nibbles=0, so enters metadata/skip handling anyway
  → Reads MSKIPBYTES=2, MSKIPLEN=33410
  → Tries to skip 33411 bytes
  → Only 16 bytes remain in stream
  → Returns BROTLI_DECODER_NEEDS_MORE_INPUT
  → Caller (WebKit.Networking) sees "need more input" for a complete response body
```

The differential: **NEEDS_MORE_INPUT vs FORMAT_ERROR** is the vulnerability signal.

---

## Variant Differences

| Variant | Diff from base | Purpose |
|---------|---------------|---------|
| **oob-base** (22B) | — | Baseline trigger |
| **oob-extended** (23B) | +1 trailing 0x00 | Tests behavior with slightly more data (17 vs 16 bytes for the skip) |
| **oob-huffman** (22B) | byte[2]: 0x10→0x18 (bit 19 flipped) | Different metadata byte — tests if decoder uses skipped data as Huffman state |
| **oob-padding** (22B) | byte[9]: 0x07→0x0f (bit 75 flipped) | Different padding flags in second meta-block — tests state divergence |

### Detailed Diffs

**oob-huffman:** Byte 2 is the 1-byte metadata payload from meta-block #1. It's
supposed to be skipped over, but if the vulnerable decoder improperly references it
as Huffman tree data (via stale state), the different bit pattern exercises a
different corruption path.

**oob-padding:** Byte 9 (0x07 → 0x0f) is part of the second meta-block's data area
where the decoder is trying to "skip" 33411 bytes. The extra set bit (bit 75) changes
what the decoder reads if it processes these bytes instead of properly skipping them.

**oob-extended:** One extra 0x00 byte gives the decoder 17 bytes instead of 16 for the
33411-byte skip. Tests whether the decoder behaves differently with marginally more data.

---

## Expected Test Results

When iPad Safari navigates to the trigger server:

| Scenario | What Happens | Meaning |
|----------|-------------|---------|
| **fetch() returns error quickly** | Decoder returned FORMAT_ERROR | **PATCHED** |
| **fetch() hangs / returns partial data** | Decoder returned NEEDS_MORE_INPUT | **VULNERABLE** (detection confirmed) |
| **Empty response** | WebKit.Networking confused by incomplete decode | **VULNERABLE** (detection confirmed) |
| **Safari tab crashes** | Unlikely with these payloads | Would indicate severe mishandling |
| **Network error** | CORS, mixed content, or cert issue | **Inconclusive** — fix server config |

The payloads are **probes**: they distinguish vulnerable from patched without causing crashes.
This is useful for confirming the vulnerability exists, but doesn't demonstrate the actual
memory corruption primitive.

---

## What's Needed for an Actual OOB Write

The real CVE-2026-111497 OOB write requires a much more complex Brotli stream:

1. **Set up valid Huffman trees** in the first block (literal/distance/insert-copy trees)
2. **Declare a single block type** (NBLTYPESL=1, NBLTYPESI=1, NBLTYPESD=1)
3. **Inject a block-type switch command** mid-data that the decoder processes
4. **DecodeBlockTypeAndLength()** encounters the switch, returns BROTLI_FALSE
5. On vulnerable builds, the caller **doesn't distinguish** "need input" from "error"
6. Decoder continues with **stale Huffman state** from a previous (or never-initialized) tree
7. Stale Huffman codes decode to **arbitrary byte values**
8. These values are written to the **ring buffer** (~230KB), potentially at an offset
   beyond the allocated bounds

Building this payload requires:
- Understanding the Brotli Huffman coding format at the bit level
- Crafting valid initial block trees
- Computing the exact point where the block-type switch triggers
- Controlling the stale state to produce a useful write pattern

This is significantly more complex than the detection probes and would need to be
adapted from the full public PoC (e.g., dimasma0305's exploit code).

---

## Payload Hex Dump

```
oob-base (22 bytes):
  20 00 10 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00

oob-extended (23 bytes):
  20 00 10 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00 00

oob-huffman (22 bytes):
  20 00 18 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00

oob-padding (22 bytes):
  20 00 10 41 41 41 fc ff ff 0f 00 22 28 f0 0b 60 f7 f4 1f 00 00 00
```
