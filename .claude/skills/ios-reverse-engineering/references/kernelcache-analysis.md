# AppleAVE2 Kernelcache Analysis — iOS 27.1 (Build 24A94232)

## Kernelcache Info
- File: `~/a20_research/kernelcache_raw` (78MB, Mach-O 64-bit arm64e)
- Kext: com.apple.driver.AppleAVE2 (913.43.1)
- UUID: C5A5A187-EC3D-3F4A-AAF8-F385E23B6218

## Address Mapping
```
KC slide:          0x3889c000
__TEXT_EXEC vmaddr: 0xfffffe0008504000, fileoff: 22020096 (0x1500000)
Prelink text:      vmaddr 0xfffffe000700c000, fileoff 0x8000
AVE2 unslid base:  0xfffffe000878dbb0
AVE2 code size:    0x1CE627 (~1.8MB)
AVE2 code end:     0xfffffe000895c1d7

Formula (code): file_offset = 22020096 + (unslid_addr - 0xfffffe0008504000)
Formula (string VA): 0xfffffe000700c000 + (file_offset - 0x8000)
```

## Key Function Locations (unslid addresses, AVE2 offsets)

### CalcSurfaceInfo
- String "CalcSurfaceInfo" at VA 0xfffffe0007323a39
- 14 xrefs in code, two clusters:
  - AVE2+0x16fd4c..0x1700ac (8 xrefs) — main CalcSurfaceInfo dispatcher
  - AVE2+0x17fb80..0x17fda0 (6 xrefs) — secondary CalcSurfaceInfo path
- HEVC CalcSurfaceInfo implementation at 0xfffffe000889a80c (AVE2+0x10cc5c)

### Surface Creation/Destruction
- `CreateExternalSurfaces` at AVE2+0x1580e0 (function start)
- `AVE_CreateExternalOutSurfaces` — 13 xrefs starting AVE2+0x1581fc
- `AVE_CreateExternalInSurfaces` at VA 0xfffffe0007320c96
- `AVE_DestroyExternalOutSurfaces` at VA 0xfffffe0007321102
- `AVE_DestroyExternalInSurfaces` at VA 0xfffffe0007320d53

### DART Mapping
- `AVE_DARTMapExternalSurfaces` — 14 xrefs starting AVE2+0x15ee78
- `DARTMapExternalSurfaces` — 10 xrefs starting AVE2+0x170e00
- `DARTMapSurface` at VA 0xfffffe0007321fc7
- `GetDARTBuf` — 4 xrefs: AVE2+0x14f06c, 0x14f0f0, 0x14f13c, 0x14f19c
- `GetDARTAddr` at VA 0xfffffe0007320326
- `GetDARTLinearBuf` at VA 0xfffffe0007320450
- `GetDARTCompressedBuf` at VA 0xfffffe0007320461

### Output Buffer Management
- `AVE_Client_UpdateOutputBuf` — 2 xrefs: AVE2+0x62c00, 0x62c60
- `OutputBufNum` logged at AVE2+0x9ae20 (from struct offset [x21+0x604])
- `OutputBufSize` logged at AVE2+0x9af08 (from struct offset [x21+0x608])

### Temporal Layer Handling
- `iNumOfTemporalLayer` loaded from struct offset [x21+0x510]
- Max value = 7 (mov w9, #7 at AVE2+0x63560)
- Assertion: `0 <= iNumOfTemporalLayer && (iNumOfTemporalLayer - 1) <= 7`
  - At line 631 (0x277) and 551 (0x227) in source
  - Xrefs at AVE2+0x63570, 0x6360c
- BUT: assertion fires AFTER CalcSurfaceInfo already computed sizes

### Crash Site
- PC offset: AVE2+0xD998 (crash instruction: `stp x0, x0, [x0, #0]`)
- LR offset: AVE2+0x1220AC
- Caller does list initialization at object+0x58 and object+0x78
- x0=0x59 — corrupted pointer from DMA OOB

## Overflow Mechanism (Established)

1. **NumberOfTemporalLayers=16463** passes through VideoToolbox → videocodecd unchecked
2. CalcSurfaceInfo computes DPB (Decoded Picture Buffer) surface sizes using this value
3. The assertion `(iNumOfTemporalLayer - 1) <= 7` exists but fires too late — after size calc
4. CreateExternalOutSurfaces allocates DMA buffers based on the computed (small) sizes
5. The AVE coprocessor DMA-writes encoded data for all 16463 layers
6. DMA writes overflow the allocated buffer into adjacent kernel memory
7. **DMA bypasses MIE** — no MTE tag checks on coprocessor memory bus
8. Adjacent kernel heap pointer corrupted to 0x59
9. Later linked-list initialization dereferences corrupted pointer → kernel panic

## Structure Layout (Partial)
```
Session/UCInfo struct (pointed to by x21):
  +0x510: iNumOfTemporalLayer (w32)
  +0x5fc: (some buffer field, logged in OutputBuf area)
  +0x600: (some buffer field)
  +0x604: OutputBufNum (w32)
  +0x608: OutputBufSize (w32)
  +0x60c: (another buffer field)
  +0x610: (another buffer field)

SurfaceInfo struct (pointed to by x19 in CreateExternalSurfaces):
  +0x120: surface pointer (x64)
  +0x138: surface count/flag (w32)
  +0x13c: another count (w32)
  +0x150: surface info pointer (x64)
  +0x16c: related field (w32)
```

## saCOBuf / Output Buffer Layout
- Assertion string: `pHdr->sSliceInfo.saCOBuf[i].saOBuf[AVE_BufIdx_Data].iSize > 0`
- This reveals: Header → SliceInfo → array of Compressed Output Buffers → array of Output Buffers
- Each COBuf has multiple OBuf entries indexed by AVE_BufIdx_Data
- The DMA output goes into these saCOBuf/saOBuf structures
- With NTL=16463, the coprocessor writes 16463 layers worth of encoded data

## NEW: Surface Creation/Allocation Chain (Traced Nov 20)

### CreateIOSurface (AVE2+0x149da0)
- String: "CreateIOSurface"
- Assertion: `"pSurfaceRoot != nullptr && psTask != nullptr && size > 0 && ppSurface != nullptr"`
- Takes: (surfaceRoot, task, buffer_desc, count, size, something, count2, output_ptr)
- Validates: x0 != NULL, x1 != NULL, w6 >= 1, x7 != NULL
- Calls AVE2+0x1496c8 to build IOSurface dictionary
- Calls AVE2+0x1c1038 for actual allocation
- Error: "failed to create surface %p %p %p %lld 0x%x 0x%x 0x%llx 0x%x %d %d"

### IOSurface Dictionary Builder (AVE2+0x1496c8)
- Called from CreateIOSurface
- Creates OSDictionary with 5 entries via AVE2+0x1c1018
- Sets properties via AVE2+0x1c1178 (key/value pairs with sizes 0x20, 0x40)
- Builds IOSurface properties: width, height, pixel format, bytes per row, alloc size
- **SIZE IS FIXED** — does NOT scale with iNumOfTemporalLayer

### Surface Creation Wrapper (AVE2+0x14a964)
- Called from CreateExternalOutSurfaces (AVE2+0x1580e0 → 0x163f70)
- Assertion: "pSurfaceRoot != nullptr && psTask != nullptr && size > 0"
- Calls AVE2+0x1c1518 for setup, then AVE2+0x149da0 (CreateIOSurface)
- Loads from struct: [x19+0x88] = surface count/type

### HEVC CalcSurfaceInfo (AVE2+0x10cc5c)
- 4 multiplications found — all compute buffer sizes from width/height/format
- **NO loads from struct+0x510 (iNumOfTemporalLayer)**
- Stores results to struct offsets: 0x198, 0x19c, 0x5e8, 0x5ec
- Buffer size is determined by resolution and codec only

## NEW: Temporal Layer DMA Loop (Critical Finding)

### NTL Usage Map (7 loads of struct+0x510 across kext)
1. AVE2+0x0279e0..0x027ab0 (4 loads) — encode logging path, x21 register
2. AVE2+0x06347c, 0x06355c (2 loads) — assertion path (fires too late)
3. **AVE2+0x13e2e8** (1 load) — **CRITICAL: DMA surface mapping loop**

### DMA Surface Mapping Loop (AVE2+0x13e2e8)
```
+0x13e2e8: ldr w8, [x19, #0x510]    ← load iNumOfTemporalLayer
+0x13e2ec: cmp w8, #0x1              ← check >= 1
+0x13e2f0: b.lt +0x13e438            ← skip if < 1
+0x13e2f4: movz x20, #0x0            ← loop counter i = 0
+0x13e2f8: movz w25, #0x0            ← accumulator = 0
+0x13e2fc: add x23, x19, #0x514      ← x23 = &struct[0x514] (per-layer array)
...
+0x13e310: (loop body)
+0x13e314: ldr w8, [x23, x20, lsl #2] ← load array[i] from struct+0x514
+0x13e318: add x0, ..., w25, sxtw #3  ← compute DMA buffer offset
+0x13e31c: str x8, [sp, #0x0]         ← push to stack
+0x13e324: bl AVE2+0x149560           ← MAP SURFACE for layer i
+0x13e328: add w25, w0, w25           ← accumulate mapped size
+0x13e32c: add x20, x20, #0x1        ← i++
+0x13e330: ldr w8, [x19, #0x510]     ← reload NTL as loop limit
+0x13e338: b.lt +0x13e310            ← loop if i < NTL
```

**This loop runs iNumOfTemporalLayer times.**
- With NTL=8 (normal): maps 8 surfaces → fits in allocated buffer
- With NTL=16463: maps 16463 surfaces → massive DMA overflow
- Array at struct+0x514 contains per-layer surface descriptors
- AVE2+0x149560 maps each surface into DART (coprocessor DMA address space)
- Accumulator w25 tracks total mapped size (returned as function result)

### Session Struct Layout (expanded)
```
+0x510: iNumOfTemporalLayer (w32) — loop iteration count
+0x514: per-layer surface descriptor array (array of w32, NTL entries)
        Each entry is a surface ID or descriptor index
+0x5e8: CalcSurfaceInfo output 1 (computed buffer metric)
+0x5ec: CalcSurfaceInfo output 2 (computed buffer metric)
+0x604: OutputBufNum (w32, debug logging only)
+0x608: OutputBufSize (w32, debug logging only)
```

## Exploitation Strategy

### OOB Write Characteristics
- **Source:** AVE coprocessor DMA engine
- **What's written:** Encoded video data (codec headers, encoded frames per layer)
- **Amount controllable:** Yes — by varying NumberOfTemporalLayers (1 to 2^31)
- **Target:** Kernel heap (zone allocator, GEN0-GEN3 zones)
- **MIE bypass:** Automatic — DMA path has no MTE tag checks
- **Root cause confirmed:** DMA mapping loop at AVE2+0x13e2e8 iterates NTL times
  into buffers sized for ≤8 layers (CalcSurfaceInfo ignores NTL)

### Overflow Formula
```
allocated_size = CalcSurfaceInfo(width, height, codec) → fixed for ≤8 layers
actual_dma_size = NTL × per_layer_encoded_size
overflow_amount = actual_dma_size - allocated_size
                = (NTL - 8) × per_layer_encoded_size  (approximately)
```

### Controlled Overflow Testing Plan (when iPhone returns)
1. **NTL=8** — maximum normal value, should NOT overflow (baseline)
2. **NTL=9** — minimum overflow: 1 extra layer (~per_layer_size bytes OOB)
3. **NTL=10** — 2 extra layers
4. **NTL=16** — 8 extra layers (small controlled overflow)
5. **NTL=32** — 24 extra layers (moderate overflow)
6. **NTL=16463** — confirmed panic (massive overflow)

Each test reveals:
- Whether that NTL crashes (overflow detected by hardware/software)
- Crash offset in panic log → per-layer DMA size
- Whether heap corruption is silent (useful for exploitation)

### For Controlled Write
1. Determine per-layer DMA size empirically from crash data
2. Use NTL=9 or NTL=10 for minimum overflow (easiest to control)
3. Heap spray IOSurface objects to position targets adjacent to DMA buffers
4. Control encoded pixel data to influence what gets written OOB
5. Target: Corrupt a known kernel object (pipe buffers, IOSurface properties, ucred)

### For Info Leak (CVE-2026-112466/112465)
- AVE OOB reads can leak kernel memory through the encode output path
- Need separate PoC to trigger read vs write OOB
- Would give KASLR bypass and heap layout information

## Calif.io Intel
- Used two vulnerabilities (CVE-2026-112705 + CVE-2026-114025) for data-only LPE
- Chain: unprivileged local user → root shell, public syscalls only
- Full 55-page report still embargoed (as of Nov 20, 2026)
- CVE-2026-114025 gives "arbitrary code execution" vs just "system termination"
- Fix was "improved input validation" (different from 112705's "bounds checking")
