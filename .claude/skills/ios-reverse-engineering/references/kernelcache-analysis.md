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

## NEW: Complete iOS 27.2 AVE CVE Map (from Apple Security Notes)

### OOB Writes (4 CVEs)
| CVE | Impact | Fix | Credit |
|-----|--------|-----|--------|
| CVE-2026-112705 | system termination | bounds checking | Calif.io (Schmitt, Chernykh, Maine) |
| CVE-2026-114025 | **arbitrary code execution** | input validation | Josh Maine of Calif.io |
| CVE-2026-112689 | system termination or corrupt kernel memory | input validation | Franco Belman (Blackwing) |
| CVE-2026-112470/112469/112468 | system termination or corrupt kernel memory | bounds checking | Hcamael/aipy |

### OOB Reads (2 CVEs)
| CVE | Impact | Fix | Credit |
|-----|--------|-----|--------|
| CVE-2026-112466 | system termination or **read kernel memory** | input validation | Hcamael/aipy |
| CVE-2026-112465 | system termination or **read kernel memory** | bounds checking | Hcamael/aipy |

### Key Observations
- CVE-2026-114025 is the ONLY one with "arbitrary code execution" — this is the exploit primitive
- CVE-2026-112705 (our confirmed trigger) only gives "system termination"
- CVE-2026-112466/112465 give info leaks — "read kernel memory"
- Both OOB read CVEs added Nov 20, 2026 (same day as 114025)
- Different fix types suggest different code paths: "bounds checking" vs "input validation"

## NEW: OOB Read Path Analysis (Info Leak)

### CreateExternalInSurfaces — NO NTL usage
- 4 xrefs: AVE2+0x1576fc, 0x157780, 0x1577c4, 0x15782c
- Scanned ±0x800 around first xref: **zero loads from struct+0x510**
- Input surface creation does NOT use iNumOfTemporalLayer
- OOB read CVEs use a DIFFERENT mechanism than the write CVEs

### GetDARTBuf Region (AVE2+0x14f000-0x14fd00)
- GetDARTLinearBuf xrefs: AVE2+0x14f330, 0x14f3b4, 0x14f400, 0x14f460
- GetDARTCompressedBuf xrefs: AVE2+0x14f79c, 0x14f820, 0x14f86c, 0x14fcac
- Buffer size calculation with MUL at: +0x14f68c, +0x14f69c, +0x14f6ac, +0x14f6c4
- Multiple `cmp w1, #0x3` and `cmp w22, #0x1` checks — format-dependent branching
- Calls AVE2+0xff168 and AVE2+0xff1dc — likely buffer arithmetic helpers

### PostProcess Function (Encode Output Path)
- String "PostProcess" at VA:0xfffffe0007316de9 (in AVE2 range)
- 8 xrefs clustered at AVE2+0x10aa10..0x10afd8
- This is the post-encode processing — where encoded data is handled
- Potential info leak path: if OOB-read data gets encoded into bitstream

### Likely OOB Read Trigger (Hypothesis)
- The read CVE likely uses stride/offset parameters instead of NTL
- Large stride values could cause the DMA engine to read beyond the input buffer
- Or: incorrect buffer size calculation for specific codec configs (e.g., 10-bit color)
- Encoded pixel data from beyond the buffer → included in bitstream → readable

## Calif.io Intel (Updated Nov 21, 2026)
- Used two vulnerabilities (CVE-2026-112705 + CVE-2026-114025) for data-only LPE
- Chain: unprivileged local user → root shell, public syscalls only
- Full 55-page report still embargoed (as of Nov 21, 2026)
- CVE-2026-114025 gives "arbitrary code execution" vs just "system termination"
- Fix was "improved input validation" (different from 112705's "bounds checking")
- Blog: "data-only kernel local privilege escalation chain targeting macOS 26.4.1 (25E253)"
- Team: Bruce Dang, Dion Blazakis, Josh Maine (with Claude Mythos Preview)
- One commenter noted "Deterministic Entropy Reduction" of MTE's 6.25% (1/16) tag collision
  → But DMA bypasses MTE entirely, so tag collision brute-force is unnecessary for DMA path

### Calif.io MIE Bypass Strategy (Critical for Our Approach)
- Chain works AROUND MIE, not through it — **data-only** approach
- No code pointers corrupted → MIE tags never checked on exploit path
- DMA bypass confirmed: coprocessor DMA writes are not subject to MTE tag checks
- MIE is active on A19+ (M5+) in EMTE synchronous mode
- Even Lockdown Mode's MTE protection is bypassed by data-only DMA approach
- Apple credited fix to Calif.io in macOS Tahoe 26.5 (CVE-2026-28952)

### Calif.io iOS 27.0.1 Jailbreak (Separate from macOS chain)
- Untethered jailbreak for iOS 27.0.1 published by Calif.io
- Bypasses MIE even with Lockdown Mode active
- Unclear if same CVEs as macOS chain or different bugs
- Our target (iOS 27.1) is one version newer — the AVE CVEs are unpatched until 27.2

### Implications for Our Exploit
1. Data-only approach is PROVEN to work against MIE on A-series chips
2. DMA bypass of MIE is the correct strategy (no need to defeat tag checks)
3. Target data structures: proc→ucred→cr_uid (for root), proc→p_csflags (for code signing)
4. These are DATA fields, not code pointers — MIE does NOT protect them
5. Our AVE DMA OOB write can corrupt these without triggering MIE faults

## NEW: Kernel R/W Primitive Strategy

### Pipe Buffer Corruption (Primary)
1. Spray 512 pipe pairs → allocates pipe_buffer structs in kernel heap
2. Close every 8th pipe → creates holes in zone pages
3. AVE DMA buffer allocated in one hole (adjacent to surviving pipes)
4. Trigger NTL=9 or NTL=10 → small DMA OOB write into adjacent pipe_buffer
5. Corrupted pipe_buffer.buffer pointer → arbitrary kernel R/W via read()/write()

### CRITICAL INSIGHT: OOB Is Array Read → Wrong DART Mapping

The DMA loop at AVE2+0x13e2e8 does NOT just write linearly past a buffer.
The actual overflow mechanism is:

```
for (i = 0; i < NTL; i++) {
    surface_desc = struct[0x514 + i*4];  // OOB READ for i >= 8!
    remaining = 32 - accumulator;
    dest = buffer_base + accumulator;
    mapped_size = per_layer_process(dest, remaining, surface_desc);
    accumulator += mapped_size;
}
```

#### Full Disassembly of DMA Loop (AVE2+0x13e2e8..+0x13e338)
```
+0x13e2e8: ldr w8, [x19, #0x510]       // Load NTL from struct
+0x13e2ec: cmp w8, #1                   // If NTL < 1, skip loop
+0x13e2f0: b.lt +0x13e438

+0x13e2f4: movz x20, #0                 // i = 0 (loop counter)
+0x13e2f8: movz w25, #0                 // accumulator = 0
+0x13e2fc: add x23, x19, #0x514         // array_base = &struct[0x514]
+0x13e300: sub x24, x29, #0x80          // buffer_base = frame - 0x80 (local)
+0x13e304: movz w26, #0x20              // max_size = 32 (buffer limit!)
+0x13e308: adrp x22, ...                // format string page
+0x13e30c: add x22, x22, #0xe25         // format string addr

// LOOP BODY:
+0x13e310: sub w1, w26, w25             // remaining = 32 - accumulator
+0x13e314: ldr w8, [x23, x20, lsl #2]  // value = array[i] ← OOB for i>=8!
+0x13e318: add x0, x24, w25, sxtw       // dest = buffer_base + accumulator
+0x13e31c: str x8, [sp]                 // push value as stack arg
+0x13e320: mov x2, x22                  // format string
+0x13e324: bl +0x149560                 // per_layer_process(dest, remaining, fmt, value)
+0x13e328: add w25, w0, w25             // accumulator += return_value
+0x13e32c: add x20, x20, #1             // i++
+0x13e330: ldr w8, [x19, #0x510]        // reload NTL (loop limit)
+0x13e334: cmp x20, x8                  // compare i with NTL
+0x13e338: b.lt +0x13e310               // loop if i < NTL
```

#### Key Observations from Disassembly:
1. **buffer_base is a STACK LOCAL** (frame - 0x80), not a heap buffer
   - The 32-byte limit (w26 = 0x20) is for this stack buffer
   - The actual DMA surface mapping happens inside the called function
2. **The called function at +0x149560** receives the OOB value as a stack arg
   - For valid layers (0-7): passes real surface descriptors
   - For OOB layers (8+): passes garbage struct values as surface descriptors
3. **The accumulator tracks bytes written to the stack buffer** (max 32)
   - But the DMA writes to kernel memory happen inside the called function
4. **No bounds check on i** — loop runs NTL times unconditionally

#### Per-Layer Array Store Analysis
Only 2 explicit stores to the per-layer array found in entire kext:
```
+0xce1a0:  str w0, [x19, #0x528]    // layer[5]
+0xce1ec:  str w0, [x19, #0x52c]    // layer[6]
+0x18eb34: str w0, [x19, #0x528]    // layer[5] (alt path)
+0x18eb7c: str w0, [x19, #0x52c]    // layer[6] (alt path)
```
Layers 0-4 and 7 are set via indexed stores (register offset).
**struct+0x534 (first OOB slot) has ZERO stores** — its value is whatever
residual data exists from struct initialization or prior operations.

#### 4 locations compute the array base address:
```
+0x27d48:  add x20, x21, #0x514     // encode setup path
+0x41228:  add x3, x3, #0x514       // config path
+0x99724:  add x3, x3, #0x514       // another config path
+0x13e2fc: add x23, x19, #0x514     // THE DMA LOOP
```

For i >= 8, `struct[0x514 + i*4]` reads BEYOND the 8-entry per-layer array:
- Layer 0-7: +0x514..+0x530 — valid surface descriptors
- Layer 8: +0x534 — whatever is at this struct offset (OOB)
- Layer 9: +0x538, etc.
- ... through +0x5e7 (struct fields before CalcSurfaceInfo outputs)
- Layer N: reads arbitrary struct data interpreted as surface descriptor

These garbage/adjacent values are passed to the per-layer processing function.
If interpreted as a surface ID or memory descriptor, they cause the coprocessor
to write encoded data into WRONG kernel memory — not just adjacent to the
buffer, but potentially at ARBITRARY locations determined by struct contents.

This makes the exploit MORE powerful but LESS predictable than a linear OOB.
The write target depends on what garbage data is at struct+0x534..0x5E7.

Per-layer array bounds:
  +0x514: layer[0] (4 bytes) — valid
  +0x518: layer[1] — valid
  ...
  +0x530: layer[7] — valid (last)
  +0x534: OOB START — adjacent struct field, read as surface desc for layer 8
  +0x5e8: CalcSurfaceInfo output 1
  +0x5ec: CalcSurfaceInfo output 2

The 180 bytes at +0x534..+0x5E7 are unknown struct fields whose values
become "surface descriptors" for the DMA mapping of temporal layers 9+.

### OOB Region Struct Field Map (+0x534..+0x5E8)
Heavily-accessed fields (access count in parentheses):
```
+0x534: (5)  loads in DMA loop region — FIRST OOB FIELD
+0x538: (3)  
+0x540: (8)  pointer field (ldr x) — 64-bit value read OOB
+0x548: (8)  
+0x550: (5)  
+0x558: (14) heavily used
+0x560: (10) 
+0x568: (12) 
+0x570: (10) 
+0x578: (32) MOST ACCESSED — likely surface ptr or config
+0x580: (27) heavily used pointer field
+0x588: (13) 
+0x590: (16) 
+0x598: (29) heavily used
+0x5a0: (12) 
+0x5a8: (11) 
+0x5b0: (16) 
+0x5b8: (16) includes nulling stores (str x31)
+0x5c0: (11) 
+0x5c8: (10) 
+0x5d0: (23) includes both loads and nulling stores
+0x5d8: (8)  
+0x5e0: (3)  
+0x5e8: (57) CalcSurfaceInfo output — MOST TOTAL ACCESSES
```

### XNU Pipe Buffer Layout (from bsd/sys/pipe.h)
```
struct pipebuf {
    u_int    cnt;      // +0x00: bytes available
    u_int    in;       // +0x04: write index
    u_int    out;      // +0x08: read index
    u_int    size;     // +0x0c: allocated size
    caddr_t  buffer;   // +0x10: kernel data pointer (target for corruption)
};
```

NOTE: On modern XNU with kalloc_type, pipe structs are in type-specific zones.
May not be co-located with AVE DMA buffers. IOSurface or mach_port objects
in general kalloc zones may be better spray targets.

### Alternative: IOSurface Property Corruption
1. Spray IOSurface objects with known property values
2. Trigger DMA OOB to corrupt an IOSurface's backing allocation
3. Read back property values through IOSurfaceCopyAllValues()
4. If a property is corrupted, the return value leaks kernel heap data

### AVE Hardware Register Write Function (+0x11c574)
Tiny function (5 instructions), called ~400+ times throughout kext:
```
+0x11c574: HINT (BTI/PAC)
+0x11c578: REV w8, w1          // byte-reverse value for HW endianness
+0x11c57c: CMP w0, #5          // validate register index
+0x11c580: B.CS +0x11c58c      // if index >= 5, extended path
+0x11c584: MOVZ w0, #1         // return success
+0x11c588: RET
```
This is `ave_write_reg(uint32_t reg_index, uint32_t value)`.

### Encode Pipeline (4-Register-Write Sequence)
Found at 3 call sites: +0xD994, +0x1220A4, +0x13e348
```
bl +0x11c574   // ave_write_reg — write HW register
bl +0x11c5b8   // another HW reg operation (likely read-back/check)
bl +0x192340   // mailbox/status operation
bl +0x11c5f0   // another HW operation
```

Crash site (+0xD994): `ave_write_reg(3, 4)` — reg 3, value 4
Panic LR site (+0x1220A4): `ave_write_reg(0x66, 4)` — reg 0x66, value 4
Register 0x66 likely triggers the encode DMA engine.

The crash sequence:
1. Register writes configure AVE coprocessor DMA descriptors
2. Per-layer array values (+0x514[i]) set DMA source/destination addresses
3. Final register write triggers the DMA engine
4. DMA engine reads per-layer descriptors including OOB values from +0x534+
5. OOB value at +0x534 ≈ 0x59 (near-NULL) → DMA tries to access 0x59
6. DART/IOMMU translation fault → kernel panic
   FAR=0x59, ESR=0x96000046 (translation fault level 2)

### Loop at +0x13e2e8 Is Likely Logging
The called function at +0x149560 is a thin wrapper around +0x1487d4.
Signature: (dest=buffer_base+accum, remaining=32-accum, fmt=string, value=OOB_read)
This matches vsnprintf(char *buf, size_t n, const char *fmt, ...).
The loop formats layer descriptors into a 32-byte stack buffer as debug output.
The actual DMA configuration happens through direct HW register writes
using the per-layer array values, elsewhere in the encode setup path.

### ACTUAL DMA Configuration Path: +0x13d308..+0x13d530 (CRITICAL DISCOVERY)

The REAL path where OOB values reach the DMA engine is at AVE2+0x13d308,
NOT the logging loop at +0x13e2e8. This code reads directly from struct+0x534
(the first OOB slot) and uses it as a **loop count** for DMA register writes.

#### First OOB Gate (+0x13d308):
```
+0x13d308: LDR w8, [x19, #0x534]      // Load OOB value from first OOB slot
+0x13d30c: CMP w8, #1                  // Check: is it >= 1?
+0x13d310: B.lt +0x13d530             // Skip if < 1 (safe path)
+0x13d314: MOV x0, x24                // Setup args for HW register write
+0x13d318: MOV x1, x21
+0x13d31c: BL +0x11c574              // ave_write_reg() — triggers DMA config
+0x13d320: CBZ w0, +0x13d404         // Check return, skip to indexed loop
+0x13d324-13d344: BL +0x11c5b8, BL +0x192340, BL +0x11c5f0  // 3 more HW writes
```

#### Indexed DMA Loop (+0x13d404):
```
+0x13d404: LDR w8, [x19, #0x534]      // Re-read OOB value = LOOP COUNT
+0x13d408: CMP w8, #1
+0x13d40c: B.lt +0x13d530             // Skip if < 1
+0x13d410: MOV x20, #0                // counter = 0
+0x13d414: ADD x22, x19, #0x538       // x22 = &struct+0x538 (array base for loop)

// LOOP BODY (+0x13d428..+0x13d52c):
+0x13d430: BL +0x11c574              // ave_write_reg() — HW register write
+0x13d43c: BL +0x11c5b8              // HW write #2
+0x13d44c: BL +0x192340              // HW write #3
+0x13d458: BL +0x11c5f0              // HW write #4
+0x13d460: LDR w9, [x22, x20, LSL #2] // *** struct+0x538[counter] — INDEXED OOB READ ***
+0x13d520: ADD x20, x20, #1          // counter++
+0x13d528: CMP w20, w8               // counter < OOB_count?
+0x13d52c: B.lt +0x13d428            // Loop back
```

#### Cascading OOB at +0x13d530:
```
+0x13d530: LDR w8, [x19, #0x558]      // Load ANOTHER struct field as count!
+0x13d534: CMP w8, #1                  // Another gate condition
+0x13d538: B.lt +0x13d758             // Skip if < 1
```

#### Exploitation Implications:
1. **struct+0x534 is a LOOP COUNT**: The OOB value (0x59=89) means 89 iterations
2. **Each iteration reads from struct+0x538+i*4**: Deep read into unrelated fields
3. **Each iteration triggers 4 HW register writes**: DMA configuration per "layer"
4. **Cascading**: After the first loop, struct+0x558 is read as ANOTHER count
5. **To control DMA target**: We need to control struct+0x534 (count) AND struct+0x538+ (descriptors)
6. **For exploitation**: Set struct+0x534 = 1, struct+0x538 = target kernel address
   → DMA writes to our chosen address, bypassing MIE/MTE entirely

#### Why FAR=0x59 in the panic:
The residual value at struct+0x534 is 0x59 (89). This is used as BOTH:
- A loop count (89 iterations)
- AND the first value read by the indexed load reads from +0x538, +0x53c, etc.
The DMA engine receives these garbage values as addresses → translation fault at 0x59.

### Complete OOB Surface Type Map (DMA Config Function +0x13d280..+0x13d9b0)

The OOB region starting at struct+0x534 contains THREE cascading surface type
descriptors, each with a count field followed by an array of surface addresses:

```
Offset  | Role                        | Notes
--------+-----------------------------+----------------------------------
+0x514  | Per-layer array (8 entries) | NORMAL — within bounds
+0x534  | Surface type A COUNT        | FIRST OOB — gate for DMA loop
+0x538  | Type A array[0]             | Indexed: [x19+0x538, x20, LSL#2]
...     | Type A array[count-1]       | Each entry = 4-byte surface desc
+0x558  | Surface type B COUNT        | Second cascade gate
+0x55c  | Type B array[0]             | Indexed: [x19+0x55c, x20, LSL#2]
...     | Type B array[count-1]       |
+0x57c  | Surface type C COUNT        | Third cascade gate
+0x580  | Type C config (36 bytes)    | Gap before array
+0x5a0  | Type C array[0]             | Indexed: [x19+0x5a0, x20, LSL#2]
+0x9a0  | Type C aux array 1[0]       | Additional per-entry data
+0x9c0  | Type C aux array 2[0]       | Additional per-entry data
```

Each surface type block:
1. Loads count, checks >= 1
2. Triggers 4 HW register writes (DMA config)
3. Loops count times, reading from indexed array
4. Each array entry is treated as a surface descriptor/IOVA
5. Falls through to next surface type

**For exploitation: Set type A count (+0x534) = 1, type A array[0] (+0x538) = target address.
This gives a single controlled DMA write to any kernel address.**

### PoC Files
- `ave_kwrite_primitive.c` — Pipe spray + AVE OOB + corruption check
- `ave_controlled_oob.c` — Multi-mode NTL testing with heap spray
- `ave_infoleak_probe.c` — Encoded bitstream analysis for kernel data
