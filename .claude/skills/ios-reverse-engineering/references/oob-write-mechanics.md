# OOB Write Mechanics — CVE-2026-111497 Heap Spray

## How the OOB Write Works After Block Exhaustion

### Setup
- Ring buffer: 128KB (131072 bytes) at address `RB`
- Block length symbol 25, cbe=0x003F0F → initial block_length = 32768 commands
- Each command: insert_len=0, copy_len=4 (I&C symbol 130)
- Distance: reads from prefix region in ring buffer

### Sequence
1. Meta-block 1 (uncompressed): writes 1024-byte prefix to `RB[0..1023]`
2. Meta-block 2 starts: block_type=0, block_length=32768
3. Commands 1-32768: each writes 4 bytes at current ring buffer position
   - Total: 32768 × 4 = 131072 bytes = one full ring buffer wrap
   - Ring buffer position after: `(1024 + 131072) % 131072 = 1024`
4. Command 32769: `DecodeBlockTypeAndLength()` is called
   - On **fast path** (non-BROTLI_SAFE): return value is **discarded**
   - Decoder continues with stale Huffman state (same tables)
   - This is the CVE: processing continues as if block type changed
5. Commands 32769+: same copy commands execute with stale state
   - Each still writes 4 bytes, still reads from prefix region
   - Ring buffer position continues advancing past the buffer end
   
### Where Does the OOB Write Land?

After 32768 commands (block exhaustion):
- `pos = (1024 + 32768*4) % 131072 = 1024` (wraps back)
- But on the fast path, after `DecodeBlockTypeAndLength` returns,
  the decoder may adjust block parameters. The stale state means:
  - The block type tree was single-symbol (symbol 0) → stays type 0
  - The block length tree was single-symbol (symbol 25) → reads 24 extra bits
  - Those 24 bits come from the **command data stream** (the zeros we packed)
  - New block_length = 16625 + (extra bits from stream)

The key insight: after block exhaustion, the decoder reads a NEW block length
from the bitstream. If the remaining bits are all zeros (which they are in our
packed command stream), new block_length = 16625 + 0 = 16625.

Then 16625 more commands execute: 16625 × 4 = 66500 more bytes written.
Total position: 1024 + 131072 + 66500 = 198596
Ring buffer position: 198596 % 131072 = 67524 = 0x107E4

**This is WITHIN the ring buffer** — not OOB yet.

But the key is that the decoder's ring buffer management may not check bounds
correctly with the stale state. The `BrotliDecoderState.ringbuffer_size` is
131072, and writes should wrap. But the OOB comes from the fact that the
decoder's internal bookkeeping of `meta_block_remaining_len` and position
become inconsistent after the stale block switch.

### Actual OOB Mechanism

Looking at the Brotli decoder source (c/dec/decode.c):

```c
// After block exhaustion on fast path:
// DecodeBlockTypeAndLength returns, its return value is ignored
// The decoder reads new block parameters from the bitstream
// BUT: the meta-block's remaining length counter is still ticking
// When meta_block_remaining_len reaches 0, the meta-block "ends"
// BUT the decoder has written more data than MLEN specified
```

With MLEN = 16777216 (24 bits all set) and each command producing 4 bytes:
- Max commands in meta-block: 16777216 / 4 = 4194304
- With block_length = 32768, block exhaustion at command 32768
- After exhaustion, 4194304 - 32768 = 4161536 more commands
- That's 4161536 × 4 = 16646144 more bytes

The ring buffer wraps many times (16646144 / 131072 = 127 wraps).
The final position: (1024 + 4194304*4) % 131072 = (1024 + 16777216) % 131072

16777216 % 131072 = 0 (exactly 128 wraps)
So final pos = 1024

The ring buffer itself is never written out-of-bounds by position wrapping.
**The OOB comes from the COPY SOURCE**, not the write destination.

### Copy Source OOB

Each copy command reads from: `ring_buffer[(pos - distance) % ringbuffer_size]`

With our distance calculation:
- distance = 6141 + distance_extra * 8
- For 1024B prefix, distance_extra = 132 → distance = 7197

The copy reads from `(pos - 7197) % 131072`. This is always WITHIN the ring buffer.
But if `ringbuffer_size` has been corrupted or the state is inconsistent...

Actually, re-reading the dimas PoC more carefully: the OOB comes from
**stage 2 manipulation of the BrotliDecoderState struct**. The generate_rce_stream
doesn't directly cause OOB writes to adjacent memory — it causes controlled data
to be written into the ring buffer that is then output to the response.

The dimas attack uses TWO connections:
1. **Groom** — fills specific heap positions with known data
2. **Poison** — the rce_stage_two() writes to `s->ringbuffer` pointer + offset,
   where the offset comes from `staged_pos`. This modifies the ring buffer pointer
   itself to point elsewhere!

### The Real Attack: BrotliDecoderState Struct Corruption

The generate_rce_stream output goes to the HTTP response body. That's what we
see as corrupted text in Safari. It doesn't directly write OOB to heap.

For actual heap corruption, we need stage 2 (`rce_stage_two(pos)`), which the
dimas PoC sends as a separate TCP frame that lands at a specific position in
the BrotliDecoderState struct. This modifies:
- `s->ringbuffer` pointer → redirects where the decoder writes
- `s->ringbuffer_size` → changes how wrapping works

### Revised Heap Spray Strategy

The heap spray should work like this:

1. **Groom**: 32 Brotli connections → 32 BrotliDecoderState structs (~7KB each)
   These are smaller allocations that cluster together.
   
2. **Free half**: Create holes in the BrotliDecoderState cluster.

3. **Replace with controlled objects**: Fill holes with attacker-controlled data
   (e.g., from other HTTP responses with specific content-length matching 7KB).

4. **Poison**: The remaining Brotli decoder's ring buffer copy commands read from
   the attacker-controlled data that replaced an adjacent freed state struct.

5. **Result**: The decoder outputs attacker-controlled data to the HTTP response,
   which JS can read. If the freed slot was refilled with memory containing
   heap pointers, those pointers leak through the HTTP response.

This is an **information leak** via the ring buffer read-from-heap technique.

## Revised Practical Approach: Stale Data Leak

A simpler information leak that doesn't require struct corruption:

1. Allocate N × 128KB Brotli ring buffers (groom connections)
2. Free all groom connections → ring buffer pages returned to allocator
3. The freed pages still contain the marker bytes from the groom streams
4. Trigger other allocations that partially fill those freed pages with 
   interesting data (heap metadata, object pointers, vtable pointers)
5. Start a NEW Brotli poison connection → its ring buffer lands on a freed page
6. The ring buffer now contains a MIX of stale marker bytes and new object data
7. The poison stream's copy commands read from these positions
8. The HTTP response body contains the leaked bytes

The advantage: we don't need stage 2 or struct corruption. The generate_rce_stream
approach works because the COPY commands read from the ring buffer, and the ring
buffer was allocated on a page that contains stale data from other objects.

The key variable is DISTANCE: `distance = 6141 + distance_extra * 8`
- Prefix length 0 → distance_extra = 4 → distance = 6173
- This means copies read from `(pos - 6173) % 131072`
- As pos advances, the copy source sweeps through the ring buffer
- If the ring buffer contains leaked pointers at certain offsets,
  they appear in the output

For maximum coverage, use a PREFIX of size 0 (or very small) so the copy
distance is small and the source reads from early in the ring buffer where
stale data is most likely to reside.

## Server Endpoints for Revised Strategy

```
/groom/<session>/<N>  — Allocate 128KB ring buffer (groom, with marker)
/stale-read           — Start Brotli stream that reads stale ring buffer data  
/probe/readback       — JS sends back the decoded bytes for analysis
```
</content>
</invoke>