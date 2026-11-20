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

## Exploitation Strategy

### OOB Write Characteristics
- **Source:** AVE coprocessor DMA engine
- **What's written:** Encoded video data (codec headers, encoded frames per layer)
- **Amount controllable:** Yes — by varying NumberOfTemporalLayers (1 to 2^31)
- **Target:** Kernel heap (zone allocator, GEN0-GEN3 zones)
- **MIE bypass:** Automatic — DMA path has no MTE tag checks

### For Controlled Write (Next Steps)
1. Understand exact overflow amount: `(NTL × per_layer_encoded_size) - allocated_buffer_size`
2. Use heap shaping (IOSurface spraying) to place target objects adjacent to AVE DMA buffers
3. Control encoded output data by crafting specific input frames
4. Target: Corrupt a known kernel object (pipe buffers, IOSurface properties, ucred)

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
