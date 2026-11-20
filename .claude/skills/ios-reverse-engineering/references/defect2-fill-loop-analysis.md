# Defect 2 Fill Loop Analysis — AVE2+0x11A7A4

## Fill Loop Disassembly

Located at AppleAVE2 __TEXT_EXEC base + 0x11A7A4 (iOS 27.1, build 24A94232).

```
; Setup before loop
ldr  x11, [x19, #0x3760]       ; x11 = NTL-derived bound from session context
add  x10, x19, #0x4678         ; x10 = DPB array base = session_ctx + 0x4678
mov  x9, #0                    ; x9 = index, starts at 0
mov  x12, #-1                  ; x12 = counter (pre-incremented)

; w24 setup
; w24 = ASR(w25, shift_amount)
; w25 = return value from DPB computation helper
; shift_amount = return value from another helper

; Fill loop
loop:
  str  w24, [x10, x9, lsl #2]  ; write w24 at array_base + (index * 4)
  add  x9, x9, #0x1            ; index++
  add  x12, x12, #0x1          ; counter++
  cmp  x12, x11                ; compare to NTL-derived bound
  b.cc loop                    ; loop while counter < bound
```

## Key Registers

| Register | Value | Source |
|----------|-------|--------|
| x10 | session_ctx + 0x4678 | DPB array base address |
| x11 | [session_ctx + 0x3760] | Loop bound, derived from NumberOfTemporalLayers |
| x9 | 0 (initial) | Array index |
| x12 | -1 (initial, pre-inc to 0) | Loop counter |
| w24 | ASR(w25, shift) | DPB-computed value, NOT arbitrary data |
| x19 | AVE session context pointer | The main session structure |

## Two Fill Loops

There are **two** fill loops that execute sequentially:
1. **Loop 1**: Writes to `session_ctx + 0x4678` region (DPB array 1)
2. **Loop 2**: Writes to `session_ctx + 0x6228` region (DPB array 2)

Both write the same w24 value with the same NTL-derived bound.

## Session Context Memory Layout (Partial)

```
+0x3760: NTL-derived bound (loop limit)
+0x4678: DPB array 1 (fill loop 1 target) ← OVERFLOW HERE
  ... gap of ~7.6KB ...
+0x6228: DPB array 2 (fill loop 2 target) ← OVERFLOW HERE TOO
```

## Overflow Calculations

Each write is 4 bytes (str w24 / w32 register, lsl #2 = index*4).
Standard HEVC DPB is 16 entries max at Level 5.x.
Allocated array: 16 entries * 4 bytes = 64 bytes.

| NTL | Writes | Total Bytes | Overflow | Status |
|-----|--------|-------------|----------|--------|
| 2 | 2 | 8 | 0 (safe) | Within bounds |
| 8 | 8 | 32 | 0 (safe) | Within bounds |
| 9 | 9 | 36 | 0 (maybe safe) | At boundary |
| 16 | 16 | 64 | 0 (exact fit) | At boundary |
| 32 | 32 | 128 | 64 bytes | Silent (within mapped) |
| 48 | 48 | 192 | 128 bytes | Silent (within mapped) |
| 63 | 63 | 252 | 188 bytes | Silent (within mapped) |
| 128 | 128 | 512 | 448 bytes | Likely silent |
| 256 | 256 | 1024 | 960 bytes | Unknown |
| 512 | 512 | 2048 | ~2KB | Unknown |
| 16463 | 16463 | 65852 | ~65KB | CRASH (unmapped page) |

NOTE: Actual array size needs device confirmation. Could be 8 entries (32 bytes)
instead of 16 (64 bytes), which would double effective overflow at each NTL.

## What Gets Corrupted

The overflow writes w24 (a DPB-computed value) into session context fields
AFTER the DPB array. These fields likely include:

- Reference picture list pointers
- DPB management state flags  
- Buffer indices for other encoding stages
- Possibly DART mapping metadata
- Rate control state

The fact that w24 is NOT arbitrary data (it's ASR of a DPB computation)
means the corruption value is somewhat predictable but not fully controllable
from userspace. However, the POSITION of corruption IS controllable via NTL.

## Exploitation Implications

1. **Controllable overflow length** via NTL (pick exact bytes to corrupt)
2. **Predictable but not arbitrary write value** (w24 from DPB math)
3. **Target: session context fields** — these control what the encoder does next
4. **Secondary bug potential**: Corrupted pointer in session context → 
   secondary dereference → controlled read/write
5. **Two overflow sites**: Both DPB arrays overflow, doubling corruption surface
