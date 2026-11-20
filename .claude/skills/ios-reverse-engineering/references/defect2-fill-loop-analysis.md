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

## Session Context Overflow Zone Map (from kernelcache analysis)

```
+0x4678: DPB array 1 start (fill loop target)
+0x4698: Overflow starts (if 8-entry array)
+0x46B8: Overflow starts (if 16-entry array)
   ...  NO CODE REFERENCES in +0x4698..+0x4AC0 (dead zone, ~1KB)
+0x4AC0: FIRST POINTER — ldr x8, [x19, #0x4ac0] (1 ref, NTL>=275)
+0x4AD0: Second pointer (1 ref, NTL>=279)
+0x4B00: Third pointer (1 ref, deref ->0x268, NTL>=291)
+0x4B18: Fourth pointer (1 ref, deref ->0x268, NTL>=297)
+0x5000: ADD base (NTL>=611)
+0x51C8: KEY POINTER — 3 refs, deref ->0x2D0, ->0x20AC, ->0x98 (NTL>=725)
+0x51D8: KEY POINTER — 3 refs, deref ->0x34, ->0x8 (NTL>=729)
+0x51E8..0x5368: 7 more pointer fields (NTL 733-831)
+0x5BA8: Last reachable pointer (NTL>=1357)
+0x6228: DPB array 2 start (fill loop 2 target)
```

Key finding: NTL 9-63 is "silent" because the overflow lands in the dead zone
(+0x4698 to +0x4AC0) which has NO code references. No fields are read from
that region, so corruption goes undetected.

To corrupt actual pointer fields that get dereferenced, NTL must be >= 275.

## Pointer Dereference Chains

**+0x51C8** (most interesting, 3 references):
1. Load ptr, then `ldr x8, [ptr+0x2D0]` → `ldr w8, [that+0x18]`
2. Load ptr, then `ldr w8, [ptr+0x20AC]` (deep offset read)
3. Load ptr, then `ldr x22, [ptr+0x98]` (loads another pointer)

If NTL=726 corrupts this pointer, the encoder will read from a fake address,
potentially giving us an info leak or controlled crash.

**+0x4AC0** (first reachable, NTL=275):
Loaded once, then accessed through computed offsets. Corrupting this is the
safest approach — only one code path uses it, limiting side effects.

## Exploitation Strategy

1. **NTL 275 (surgical)**: Corrupt just the first pointer at +0x4AC0
   - Encoder reads from corrupted address → crash or info leak
   - If w24 value happens to point to controlled data → arbitrary read

2. **NTL 726 (aggressive)**: Corrupt the triple-referenced pointer at +0x51C8
   - Three dereference chains → more chances for useful primitive
   - Risk: may crash before reaching the useful dereference

3. **Heap spray + NTL combo**: Spray IOSurface objects, then encode with
   targeted NTL to corrupt session context pointer to point at spray data

## Exploitation Implications

1. **Controllable overflow length** via NTL (pick exact bytes to corrupt)
2. **Predictable but not arbitrary write value** (w24 from DPB math)
3. **Target: session context fields** — these control what the encoder does next
4. **Secondary bug potential**: Corrupted pointer in session context → 
   secondary dereference → controlled read/write
5. **Two overflow sites**: Both DPB arrays overflow, doubling corruption surface
6. **Dead zone**: NTL 9-63 safe for testing (no crash), NTL 275+ for exploitation
