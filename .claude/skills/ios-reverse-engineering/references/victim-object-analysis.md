# CVE-2026-86950 Victim Object Analysis

## OOB Write Primitive Summary

| Property | Value |
|----------|-------|
| Direction | FORWARD (+5120 bytes from coverage buffer start) |
| Target | Byte 0 of the next SMALL magazine allocation |
| Write type | Coverage increment (ADDITION, not store) |
| Bytes modified | 4 bytes (two 16-bit coverage halves) |
| Magnitude | ~38 × subpixel_alpha per half (depends on edge geometry) |
| Trigger | Single glyph render via `aa_cache_render` |
| Font params | N=10, DX=2, TX=-16367 |

### Key Constraints

- **SMALL magazine has no inline malloc headers.** Byte 0 of the allocation IS byte 0 of user data. No free-list pointers to corrupt.
- **ObjC/CF objects have PAC-protected ISA at byte 0.** On A12+ (all target devices), corrupting ISA causes PAC failure → crash, not exploitation.
- **Offset granularity is ~256 bytes per TX unit.** Cannot precisely target specific struct fields within an allocation (e.g., can't aim for +0x10 without jumping +256).
- **Coverage buffer is ephemeral.** Allocated at start of `aa_cache_render`, freed at end. The victim must be something that COEXISTS with the coverage buffer during rendering.

## Victim Object Candidates

### 1. Inline Image Decoded Pixel Buffers (RECOMMENDED for confirmation)

**Feasibility: VERY HIGH**

When CoreGraphics processes PDF inline images (BI/ID/EI operators), it:
1. Decodes the image data into a raw pixel buffer (malloc allocation)
2. Retains the buffer for the duration of the page render
3. Composites the pixels into the output image

For CMYK images (4 components): `W=1280, H=1, BPC=8` → decoded buffer = 1280 × 1 × 4 = **5120 bytes exactly**.

This matches the SMALL magazine quantum perfectly. 64 spray images create 64 persistent 5120-byte allocations. When the trigger glyph renders, its coverage buffer is allocated from the same SMALL magazine region, adjacent to the spray buffers. The OOB +5120 bytes corrupts one spray buffer's first 4 bytes.

**Detection:** The corrupted image has visibly wrong pixels at its leftmost position. All other spray images show the expected uniform color. Reading the rendered thumbnail reveals which spray image was hit and the exact OOB increment magnitude.

**Primitive:** Visual OOB confirmation + magnitude calibration. NOT directly useful for arbitrary R/W, but essential for validating the exploit fires on iOS.

### 2. ICC Color Profile Backing Buffers (RECOMMENDED for exploitation)

**Feasibility: HIGH**

PDF color spaces can reference ICC profiles:
```
/ColorSpace [/ICCBased <stream_ref>]
```

CoreGraphics loads the ICC profile stream into a **CFData backing buffer**. If we craft the ICC profile to be exactly 5120 bytes, its backing buffer lands in the SMALL magazine.

**ICC profile layout (first 4 bytes):**
```
Offset 0x00: Profile size (uint32 big-endian) = 0x00001400 (5120)
```

After OOB corruption, the profile_size field increases. When CoreGraphics re-reads the profile (during color space conversion), it trusts the corrupted size and reads past the buffer end → **heap over-read**.

**How this escalates:**
- The over-read data is interpreted as ICC tag table entries
- Each tag entry has: signature (4B) + offset (4B) + size (4B)
- If the over-read contains heap metadata or pointers, they're interpreted as tag offsets
- A tag with an artificially large offset causes CG to read from arbitrary heap locations
- The read data is used for color lookup tables → affects rendered pixel values
- Rendered pixel colors **encode the memory contents** → information disclosure

**Key advantage:** ICC profiles persist for the entire page render, ensuring the buffer is still allocated when the OOB fires.

### 3. CGImage Width/Height Fields

**Feasibility: MEDIUM**

CGImage struct (approximate arm64 layout):
```
+0x00: ISA (8 bytes, PAC-protected) ← NOT useful
+0x08: info flags + refcount
+0x10: width (size_t, 8 bytes)
+0x18: height (size_t, 8 bytes)
+0x20: bitsPerComponent
+0x28: bytesPerRow
+0x30: colorSpace
+0x38: dataProvider → data pointer
```

Corrupting `width` at +0x10 would cause size confusion: subsequent pixel extraction reads more columns than allocated → over-read. However, the OOB targets byte 0 (ISA), and shifting to +0x10 requires TX adjustment of less than 1 unit (16 bytes / 256 bytes-per-unit), which we can't achieve.

**Verdict:** Not reachable with current offset granularity.

### 4. ripc_GlyphImage (Glyph Cache Entry)

**Feasibility: LOW**

Internal struct with `bounds` (CGRect) at byte 0 and `bytesPerRow` at +0x10. Corrupting bytesPerRow would cause stride confusion in pixel reads. However, these are transient allocations within the glyph cache and hard to predict/control.

### 5. Coverage-to-Coverage Corruption

**Feasibility: NOT VIABLE**

Each glyph's coverage buffer is allocated and freed within its own `aa_cache_render` call. Two glyphs' coverage buffers never coexist in memory, so one can't corrupt the other.

## Recommended Exploitation Strategy

### Phase 1: OOB Confirmation (Step 2a)

**Goal:** Visually confirm the OOB fires on iOS and measure the increment.

**PDF structure:**
1. 64 CMYK inline images (1280×1×4 = 5120 bytes each)
   - All filled with `0x80` (50% gray)
   - Arranged as visible rectangles on the page
2. Trigger glyph renders LAST in the content stream

**Expected result on vulnerable iOS:**
- 63 images render as uniform 50% gray
- 1 image has corrupted leftmost pixel(s) — different color

**Expected result on patched macOS:**
- All 64 images render as uniform 50% gray

### Phase 2: ICC Profile Length Corruption (Step 2b)

**Goal:** Turn the OOB increment into a heap over-read.

**PDF structure:**
1. 32 ICCBased color space objects, each exactly 5120 bytes
   - Profile size field: `0x00001400` (5120 big-endian)
   - Minimal valid RGB profile with padding to reach 5120
2. 32 inline images using these ICC color spaces
3. Trigger glyph renders last

**After OOB:** One ICC profile's size field is incremented → CG reads past the buffer during color conversion → adjacent heap data leaks through rendered pixel colors.

### Phase 3: ASLR Defeat (Step 2c)

**Goal:** Disclose heap pointers to defeat ASLR.

Using the Phase 2 over-read:
- Adjacent heap data may contain CF/CG object pointers
- These pointers reveal the ASLR slide for dyld_shared_cache
- With the ASLR slide, we know the addresses of all system frameworks

**Technique:** WebKit + PDF hybrid
- Serve the PDF in a `<iframe>` within an HTML page
- Use Canvas `drawImage` + `getImageData` to read rendered pixels
- JavaScript decodes the pixel colors into disclosed pointer values
- Use this to craft a second-stage PDF with precise corruption targets

### Phase 4: Arbitrary R/W → Kernel (Steps 3-5)

With ASLR defeated and heap layout known:
1. Craft a PDF that places a victim object at a known address
2. OOB corrupts a function pointer or vtable entry (bypassing PAC requires gadget chain)
3. Achieve code execution within the PDF rendering sandbox
4. Use kernel vulnerability (CVE-2026-65350 UAF or CVE-2026-112477 IOSurface race) for privilege escalation
5. Patch kernel to disable code signing → install Sileo

## Implementation Priority

| Step | Description | Difficulty | Files |
|------|-------------|-----------|-------|
| 2a | CMYK spray + visual OOB confirmation | Easy | `gen_oob_confirm_pdf.py` |
| 2b | ICC profile length corruption | Medium | `gen_icc_exploit_pdf.py` |
| 2c | ASLR defeat via pixel readback | Medium | HTML + JS harness |
| 3 | Sandbox escape (code execution) | Hard | Native exploit code |
| 4 | Kernel exploit | Very Hard | Kernel exploit chain |
| 5 | Sileo installation | Easy | Package manager setup |

## Next Immediate Action

Build `gen_oob_confirm_pdf.py` — a PDF with CMYK inline image spray that visually confirms the OOB write on iOS. This is the prerequisite for everything else.
