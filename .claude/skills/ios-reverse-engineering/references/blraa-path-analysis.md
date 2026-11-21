# BLRAA Dispatch Path Analysis — Session 17

## +0x5F94 Dispatch Site (0xfffffe000909b364)

Entry 641 of the first VPS table lands at buffer+0x5F94. The 4-byte layer_id field
at that offset controls a 3-way dispatch:

| layer_id value | Path | Target |
|----------------|------|--------|
| 0 or ≥3 | Skip (default) | B 0x909b4b0 |
| 1 | BLRAA vtable[0x188] | Calls method(self, 4) |
| 2 | 5-level chain walk | Complex dispatch (decoded below) |

**We FULLY CONTROL which path fires by setting layer_id on the MV-HEVC encode.**

## Value==2 Path — FULLY DECODED (0x909b50c)

### Phase 1: Config check
```
0x909b50c: MOVZ W23, #0x5f98        // Load offset (+0x5F98 = entry 641's cumsum field)
0x909b510: ADRP X8, ...             // Load config page
0x909b514: ADD X8, X8, #0x218
0x909b518: LDR W8, [X8, #108]       // Load global config flag
0x909b51c: CBZ W8, 0x909b538        // If zero → proceed to chain walk (NORMAL PATH)
```

### Phase 2: 5-level pointer chain walk (at 0x909b538)
```
LDR X8, [X20, #0x28]    // Level 1: encoder sub-object (UNCORRUPTED — separate allocation)
LDR X8, [X8, #0x70]     // Level 2: nested pointer
LDR X8, [X8, #0x288]    // Level 3: nested pointer
LDR W8, [X8, #0x58]     // Level 4: 32-bit value
CBZ W8, 0x909b5a4        // Zero → Sub-branch B; Nonzero → Sub-branch A
```

These are REAL kernel pointers from the encoder object (not in our corrupted buffer).
The chain walk succeeds or fails based on encoder state, NOT our corruption.

### Sub-branch A: Nonzero chain walk (BLRAA vtable[0x1F0])
```
0x909b560: LDR X16, [X20, #0]       // Load vtable from encoder
0x909b564: MOV X17, X20
0x909b568: MOVK X17, #0xcda1, LSL #48
0x909b56c: AUTDA X16, X17           // ★ Authenticate vtable
0x909b570: MOVZ X17, #0x1f0         // vtable offset 0x1F0
0x909b574: ADD X16, X16, X17
0x909b578: LDR X8, [X16, #0]        // X8 = vtable[0x1F0]
0x909b580: ADD X2, X22, X23         // ★ X2 = buffer + 0x5F98 (OUR DATA!)
0x909b584: MOV X0, X20              // arg0 = self
0x909b588: MOV X1, X19              // arg1
0x909b58c: MOVZ W4, #0x1
0x909b590: MOVZ W5, #0x1
0x909b594: MOVZ W6, #0xf
0x909b598: MOVK X16, #0xfe64, LSL #48
0x909b59c: BLRAA X8, X16            // ★ vtable[0x1F0](self, X19, buf+0x5F98, ?, 1, 1, 15)
```

**ARG2 = buffer+0x5F98 → pointer to our OOB-written data, KERNEL-VALID ADDRESS.**

### Sub-branch B: Zero chain walk (direct function calls)
```
0x909b5a8: ADD X0, X22, X23         // X0 = buffer + 0x5F98 (OUR DATA!)
0x909b5ac: MOVZ W1, #0x846          // 2118 (HEVC param size)
0x909b5b0: MOVZ W2, #0x1
0x909b5b4: MOVZ W3, #0xf
0x909b5b8: BL 0x90b4f40             // ★ Call 1: process(buf+0x5F98, 0x846, 1, 15)

0x909b5bc: LDR X0, [X20, #0x30]     // arg0 = encoder sub-object
0x909b5c0: ADD X6, X22, X23         // X6 = buffer + 0x5F98 (OUR DATA!)
0x909b5c4: MOV X1, X19
0x909b5c8: MOVZ W2, #0x500          // 1280
0x909b5cc: MOVZ W3, #0x1
0x909b5d0: MOVZ W4, #0x3800         // 14336
0x909b5d4: MOVZ W5, #0x8834         // 34868
0x909b5d8: MOVZ W7, #0x846          // 2118
0x909b5dc: BL 0x9087cbc             // ★ Call 2: dispatch(enc_sub, X19, 0x500, 1, 0x3800, 0x8834, buf+0x5F98, 0x846)
```

**BOTH sub-branches pass kernel-valid pointer to our OOB data. NO PAN ISSUE.**

## Function 0x9087cbc — Vtable Dispatch Trampoline

```
Entry:
  W8 = W5 - W4 = 0x8834 - 0x3800 = 0x5034     // Size delta
  X9 = X0 + 0xC                                 // Object data offset
  X10 = W3 << 2 = 4                             // Index * 4
  X11 = X9 + sign_ext(W10) or X16 = X9 + X10   // Conditional address computation
  W9 = [X11]                                     // ★ Loads 32-bit value from computed addr
  X0 = [X0 + 0x20]                              // Follow pointer chain
  AUTDA vtable from X0                           // Authenticate
  W3 = W8 + W9 = 0x5034 + [loaded_value]        // Computed size/offset
  X4 = X6 = buffer+0x5F98                        // ★★ OUR DATA AS ARG4
  X5 = X7 = 0x846                               // Size
  W6 = 1
  BRAA to vtable method                          // PAC-authenticated indirect call
```

**The trampoline passes our data to yet another vtable method as arg4 (X4).**

## TYPE CONFUSION Analysis

The data at buffer+0x5F98 is NOT HEVC parameter data. It's OOB-written VPS counter entries:

```
+0x5F98: cumsum of entry 641 (uint32, ~180-200)
+0x5F9C: NAL_size>>3 of entry 641 (uint32, ~1)
+0x5FA0: [entry 642: NAL_type, layer_id, cumsum, size>>3]
+0x5FA4: [entry 642 layer_id — FULLY CONTROLLED 0-65535]
...continuing with more OOB entries...
```

When the encode pipeline processes this as HEVC parameter data:
- Cumsum values (~180) get interpreted as codec parameters
- layer_id values (fully controlled) get interpreted as parameter values
- If ANY processing function uses these as sizes/indices without bounds checking → **SECONDARY OVERFLOW**

## Why This Bypasses PAN

| Path | Pointer type | PAN status |
|------|-------------|------------|
| Cross-buffer stack overflow | Corrupted register → user-space addr | **BLOCKED by PAN** |
| Value==2 buffer data pass | buffer+0x5F98 → kernel heap addr | **NOT blocked** |

The buffer itself lives in kalloc.49152 (kernel heap). Pointing into it is always valid
under PAN because it's a kernel-space address. The DATA is corrupted but the POINTER is legitimate.

## Exploitation Implications

1. **No PAC bypass needed** — all function calls use real, PAC-signed vtable pointers
2. **No PAN concern** — all pointers are kernel-valid heap addresses
3. **Attacker controls data content** — layer_id (0-65535) and cumsum/size fields
4. **Multiple processing stages** — data passes through 2-3 vtable methods, each
   potentially using corrupted values as sizes/indices
5. **Size parameters in play** — 0x500, 0x3800, 0x8834, 0x846 suggest DMA buffer sizing

## REVISED Priority

```
TIER 0 (NEW — no PAC, no PAN dependency):
  ★ Value==2 type confusion via +0x5F94 dispatch
    - 214 encodes with layer_id=2 on the target entry
    - Corrupted data processed through PAC-authenticated vtable pipeline
    - Multiple stages for secondary corruption

TIER 1 (no format output dependency):
  - Value==1 BLRAA vtable[0x188](self, 4) — real call, corrupted state
  - SIMD 56-byte overwrite at +0x5ED4 (intra-buffer, ~205 encodes)

TIER 2 (PAN risk):
  - Cross-buffer stack overflow → register corruption → W-W-W

TIER 3 (DoS):
  - Counter overflow DoS (cumsum > 2049)
```

## Next Steps (On-Device Required)

1. **MV-HEVC verify** on iPhone Duo (prerequisite)
2. **214 encodes with layer_id=2** → trigger value==2 dispatch
3. **Crash log analysis** → reveals which sub-branch fired (A or B) and what happened
4. **If no crash**: the type confusion was processed silently → need readback probe
5. **If crash at vtable[0x1F0]**: chain walk succeeded, need to analyze target method
6. **If crash at 0x90b4f40 or 0x9087cbc**: chain walk zero path, analyze the crash
