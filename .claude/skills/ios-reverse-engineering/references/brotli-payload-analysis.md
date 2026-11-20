# CVE-2026-111497 Brotli Payload Byte-Level Analysis

## Payload Variants

```
oob-base:     20 00 10 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00
oob-extended: 20 00 10 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00 00
oob-huffman:  20 00 18 41 41 41 fc ff ff 07 00 22 28 f0 0b 60 f7 f4 1f 00 00 00
oob-padding:  20 00 10 41 41 41 fc ff ff 0f 00 22 28 f0 0b 60 f7 f4 1f 00 00 00
```

## Brotli Bitstream Format (RFC 7932, LSB-first)

Brotli is bit-packed, least-significant-bit first within each byte.

### Byte 0: `0x20` = `0010 0000`

**Stream header — WBITS (window size):**

Per RFC 7932 Section 9.1, if the first byte is not `0x11`:
- Bits 0-3 of byte 0 encode WBITS.
- `0x20` = binary `0010 0000`
- Bits [3:1] = `000`, bit[0] = `0`
- Since bit[0] = 0: WBITS = 16 (from lookup: when low nibble is `0`, WBITS = 16)
- Window size = 2^16 = 65,536 bytes (64 KB)

Actually, the Brotli WBITS encoding is:
- If byte & 0x01 == 0: WBITS = 16
- If byte & 0x0F == 0x01: WBITS = 17  
- If byte & 0x0F == 0x03: 1-bit prefix 1 + 3 bits for WBITS
- etc.

For `0x20`: low bit = 0, so **WBITS = 16, window = 64KB**. Only 1 bit consumed for the header.

Remaining bits from byte 0: `001 0000` (bits 1-7) flow into meta-block header.

### Meta-block Header (starting at bit 1)

**ISLAST** (1 bit): bit 1 of byte 0 = 0 → NOT the last meta-block

**MNIBBLES** (2 bits): bits 2-3 of byte 0 = `00` → MNIBBLES = 4 (meaning MLEN is encoded in 4 nibbles = 16 bits)

Wait — RFC 7932 Section 9.2:
- If ISLAST = 0:
  - MNIBBLES: 2 bits. If 00 → special "empty" meta-block. If non-zero → number of nibbles for MLEN-1.
  
Bits 2-3 = `00` from byte 0. This means MNIBBLES = 0 → **empty meta-block** (metadata block or padding).

Hmm, that doesn't match the expected behavior. Let me re-parse more carefully.

### Re-parsing with exact bit positions

Byte 0: `0x20` = bits `0 0 1 0 0 0 0 0` (bit 0 = LSB = 0, bit 7 = MSB = 0)

Wait, written out properly for a byte `0x20`:
- Binary: 00100000
- Bit 0 (LSB): 0
- Bit 1: 0
- Bit 2: 1
- Bit 3: 0
- Bit 4: 0
- Bit 5: 1  
- Bit 6: 0
- Bit 7: 0

**WBITS prefix bit (bit 0):** 0 → WBITS = 16, 1 bit consumed.

**Meta-block header starts at bit 1:**

**ISLAST (bit 1):** 0 → not last block

**MNIBBLES (bits 2-3):** bit2=1, bit3=0 → value = `10` binary = 2 → MLEN encoded in 2 nibbles = 8 bits

**MLEN-1 (bits 4-11, 8 bits):** 
- Bits 4-7 from byte 0: bit4=0, bit5=1, bit6=0, bit7=0 → nibble = `0100` = 4
- Bits 8-11 from byte 1 (`0x00` = `00000000`): bit0=0, bit1=0, bit2=0, bit3=0 → nibble = `0000` = 0
- MLEN-1 = 0x04 (LSB first: low nibble 4, high nibble 0) = 4
- **MLEN = 5** (5 uncompressed bytes in this meta-block)

### Byte 1: `0x00`

Bits 0-3 consumed for MLEN upper nibble (all 0).

**ISUNCOMPRESSED (bit 4 of byte 1):** 0 → compressed meta-block

Remaining bits (5-7) of byte 1 flow into block type/count encoding.

### Block Type and Tree Descriptions

After MLEN, compressed meta-blocks encode:
1. **NBLTYPESL** — number of literal block types
2. **NBLTYPESI** — number of insert-and-copy block types  
3. **NBLTYPESD** — number of distance block types

Each uses a variable-length encoding.

**Bit 5 of byte 1:** This is where the block type encoding begins.

Byte 1 remaining bits [5-7]: `000`
Byte 2: `0x10` = `00010000`

**NBLTYPESL** encoding: If first bit is 0, NBLTYPESL = 1 (single literal block type).
- Bit 5 of byte 1 = 0 → **NBLTYPESL = 1**

**NBLTYPESI**: 
- Bit 6 of byte 1 = 0 → **NBLTYPESI = 1**

**NBLTYPESD**:
- Bit 7 of byte 1 = 0 → **NBLTYPESD = 1**

With all block type counts = 1, no block-type switching trees are needed.

### Literal Context Modes and Huffman Trees

Bytes 2-5: `10 41 41 41`

The next sections encode:
- Number of literal Huffman trees (NTREESL)
- Context map for literals  
- Number of distance Huffman trees (NTREESD)
- Context map for distances
- Huffman code descriptions for literals, insert-and-copy, and distances

### The Trigger Point: Bytes 6-9: `fc ff ff 07/0f`

This is where the variants diverge and where the vulnerability triggers.

**oob-base vs oob-padding:** Byte 9 is `07` vs `0f`
- `07` = `00000111` — 3 low bits set
- `0f` = `00001111` — 4 low bits set

**oob-base vs oob-huffman:** Byte 2 is `10` vs `18`
- `10` = `00010000`
- `18` = `00011000` — bit 3 differs

These bytes encode the Huffman tree descriptions for the insert-and-copy length codes. The `fc ff ff` sequence creates an **invalid or adversarial Huffman tree** that causes `DecodeBlockTypeAndLength()` to fail.

### The Vulnerability Mechanism

**`DecodeBlockTypeAndLength()` in `c/dec/decode.c`:**

```c
static BROTLI_BOOL DecodeBlockTypeAndLength(
    BrotliDecoderState* s, int tree_type) {
  // ... reads block type from bitstream using Huffman tree
  // Returns BROTLI_FALSE on BOTH conditions:
  //   1. Need more input (legitimate, should retry)
  //   2. Format error (should abort)
}
```

The caller on the **fast path** (`BrotliDecoderDecompressStream`, state `BROTLI_STATE_COMMAND_BEGIN`):

```c
if (!DecodeBlockTypeAndLength(s, 1)) {
  // Interpreted as "need more input" — saves state and returns
  // BROTLI_DECODER_NEEDS_MORE_INPUT
  s->substate_read_block_length = ...;
  result = BROTLI_DECODER_NEEDS_MORE_INPUT;
  break;
}
```

When the crafted bytes produce a format error (invalid block type value > NBLTYPES), the function returns `BROTLI_FALSE`. The caller **does not check if this was an error vs. needing input**. It treats it as "needs more input" and continues.

### Stale Huffman State

When `DecodeBlockTypeAndLength()` fails mid-parse, it has partially consumed bits but left the Huffman decoding state from a **previous block type context** in place. Specifically:

1. `s->block_type_trees[tree_type]` still points to the previous tree
2. `s->block_length[tree_type]` retains the old block length  
3. The bit reader position is advanced past the invalid data

When the decoder continues (thinking it just needs more input and then resumes), it uses these stale values to index into the ring buffer. The stale block length can specify a write that extends **beyond the allocated ~230KB ring buffer**, causing an OOB write.

### Ring Buffer OOB Write

The Brotli ring buffer is allocated based on WBITS:
- For WBITS=16: ring buffer = 2^16 = 64KB (but actual allocation may be larger due to implementation)
- The decoder writes decompressed output into this ring buffer in a circular fashion
- With a corrupted block length from stale state, the write offset can exceed the buffer bounds

The OOB write lands in heap memory adjacent to the ring buffer. Since the ring buffer is a large allocation (typically via `malloc`), the adjacent memory contains other heap metadata or objects — prime targets for corruption.

### Variant Differences

| Variant | Difference | Effect |
|---------|-----------|--------|
| oob-base | Baseline | Standard OOB trigger |
| oob-extended | +1 null byte at end | Tests decoder behavior with extra trailing data |
| oob-huffman | Byte 2: 0x18 vs 0x10 | Different Huffman tree encoding — may trigger via different code path |
| oob-padding | Byte 9: 0x0f vs 0x07 | Extra bit set in block type field — wider invalid value |

The different variants test different code paths in the decoder to find which produces the most exploitable corruption pattern:
- **oob-base**: Minimal trigger, standard block type confusion
- **oob-huffman**: Altered Huffman tree description may cause the stale state to contain different values
- **oob-padding**: Larger invalid block type value → potentially larger OOB offset
- **oob-extended**: Tests whether the decoder reads past the stream end

### Expected Behavior on Vulnerable iOS 27.0.1

1. Safari/WebKit receives the Brotli stream (via Content-Encoding: br or DecompressionStream API)
2. Apple's libcompression Brotli decoder (Build 24A446) begins decompressing
3. Decoder parses header, starts on the compressed meta-block
4. Reaches the invalid block type at bytes 6-9
5. `DecodeBlockTypeAndLength()` returns `BROTLI_FALSE` (format error)
6. Caller misinterprets as "need more input", continues with stale Huffman state
7. Stale block length causes write beyond ring buffer bounds
8. **Result: OOB heap write** in WebKit.Networking (via CE:br) or WebContent (via DecompressionStream)

### Exploitation Primitive

The OOB write gives us:
- **What**: Write controlled bytes to heap memory adjacent to the ring buffer
- **Where**: Offset determined by the stale block length value
- **Size**: Potentially large (up to the full stale block length)
- **Content**: The "decompressed" data, which is derived from the crafted bitstream

For RCE, the strategy is:
1. Heap spray objects adjacent to the ring buffer allocation
2. Use the OOB write to corrupt a function pointer, vtable, or length field
3. Trigger use of the corrupted object → code execution

### Apple's Fix (iOS 27.2.1, Build 24B156)

The fix in `libcompression` properly checks the return value of `DecodeBlockTypeAndLength()`:

```c
// Fixed version:
result = DecodeBlockTypeAndLength(s, tree_type);
if (result == BROTLI_DECODER_ERROR_FORMAT_...) {
  // Properly handle format error — abort decoding
  return BROTLI_DECODER_RESULT_ERROR;
}
if (result == BROTLI_DECODER_NEEDS_MORE_INPUT) {
  // Legitimate need-more-input handling
  break;
}
```

The fix distinguishes between the two failure cases rather than treating both as "need more input".
