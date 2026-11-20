# Heap Feng Shui Strategy for CVE-2026-86950

## Educational Research — Turning OOB Write into Arbitrary R/W

---

## 1. The Primitive (Recap)

From `gen_font.py` and the Calif.io writeup:

```
Coverage buffer allocation:
  width_px = (bbox_max_x - bbox_min_x) / 4096
  buffer_size = 4 * (width_px + 16) bytes
  if width_px > 1015: malloc(buffer_size)  ← HEAP OOB
  else:               alloca(buffer_size)  ← STACK OOB

Corrected PoC settings (v2):
  N = 10, DX = 17, TX = 16409
  width_px = int(17 * SCALE^10 * 1024/16384) = 1087
  buffer_size = 4 * (1087 + 16) = 4412 bytes
  malloc quantum = 5120 bytes (SMALL bin), padding = 708 bytes
  OOB = +768 bytes past buffer → +60 bytes into NEXT allocation

OOB write type: 16-bit increment on two adjacent 16-bit values
OOB offset: TX=16409 → trig_px=1279, oob_px=192, oob_bytes=768
  768 - 708 (padding) = 60 bytes into adjacent victim object

Confirmed on macOS 27.0.1 (PATCHED): no OOB, clean exit
Ready for testing on iPad Mini (iOS 27.0.1, VULNERABLE)
```

## 2. Size Class Targeting

### libmalloc Magazine Allocator Size Classes

libmalloc bins allocations into size classes. The coverage buffer size is attacker-controlled (via `DX`), so we can target any class:

| DX | width_px (approx) | buffer_size | Likely malloc bin |
|----|-------------------|-------------|-------------------|
| 4  | 2,048             | 8,256       | LARGE_TINY or SMALL |
| 8  | 4,096             | 16,448      | SMALL |
| 16 | 8,192             | 32,832      | SMALL |
| 33 | 16,896            | 67,648      | LARGE (current PoC) |
| 2  | 1,024             | 4,160       | TINY |

### Optimal Size Class Selection

For heap feng shui, we want a size class that:
1. Has **many same-sized objects** we can spray to fill the region
2. Contains **useful victim objects** (CFData, NSData, ObjC objects)
3. Has **predictable layout** (sequential allocations are adjacent)

**Strategy: Target the SMALL magazine (~1KB–15KB range)**

By adjusting DX, we can make the coverage buffer land in a size class where CoreFoundation objects like CFData are commonly allocated. CFData stores its length at a known offset from its header.

## 3. Heap Feng Shui Approach

### Phase 1: Exhaust Free Lists

Before triggering the vulnerability, spray allocations of the target size class to fill existing free slots in the magazine region. This forces the allocator to serve from fresh, sequential memory.

**In a PDF context**, we can spray via:
- Multiple embedded fonts (each font triggers allocations)
- Multiple pages with different resources
- Inline images of controlled sizes
- Content streams that trigger CoreGraphics allocations

### Phase 2: Victim Object Placement

Allocate the "victim" object — the one we want to corrupt. For CFData length corruption:

```
Heap layout goal:
  [spray][spray][spray][coverage_buffer][CFData_victim][spray]...
                        ^^^^^^^^^^^^^^^^ ^^^^^^^^^^^^
                        OOB write here   corrupts length field
```

### Phase 3: Trigger the OOB Write

The PDF triggers `CGImageSourceCreateThumbnailAtIndex` → CoreGraphics rasterizes the glyph → `aa_cache_render` allocates the coverage buffer and writes past its end.

The 16-bit increment at the controlled offset lands on the CFData's length field, expanding it.

### Phase 4: Use Expanded CFData

With the corrupted (expanded) length field:
1. **Read past the CFData buffer** → leak heap pointers (defeats ASLR)
2. **Write past the CFData buffer** → corrupt adjacent objects
3. This gives us a **wider read/write window** from which to build full arbitrary R/W

## 4. OOB Offset Control

The OOB offset is determined by where the edge alpha writes land relative to the buffer end. From the PoC:

```python
# In gen_font.py:
# Trigger point at x=32000 in leaf glyph
# After 9 levels of ~2x scaling: x ≈ 32000 * 2^9 = 16,384,000 font units
# At Tf=1024, UPM=16384: device_x = 16,384,000 * 1024/16384 = 1,024,000
# In fixed point (4096 per pixel): 1,024,000 * 4096 ≈ 4.19 billion > INT32_MAX

# aa_moveto: FCVTZS saturates to INT32_MAX (2,147,483,647)
# aa_lineto: XTN truncates to low 32 bits (wraps)
# Difference → bbox is computed incorrectly → buffer too small
```

To control the OOB offset precisely, we adjust:
- The trigger point x-coordinate (32000 in the leaf)
- The number of composite levels (N=9)
- The scaling factor (SCALE ≈ 2.0)
- The PDF text matrix / Tf size (1024)

The offset where the OOB write lands = (actual_edge_x - bbox_max_x) / 4096, converted to the buffer index.

## 5. Victim Object Candidates

### Option A: CFData / NSMutableData Length Corruption

CFData layout (64-bit):
```
Offset  Field
0x00    ISA pointer (8 bytes)
0x08    CFRuntimeBase info (8 bytes)  ← contains type ID + flags
0x10    _bytesDeallocator (8 bytes)
0x18    _length (CFIndex, 8 bytes)    ← TARGET
0x20    _capacity (CFIndex, 8 bytes)
0x28    _bytes pointer (8 bytes)
0x30+   inline data (if small enough)
```

Incrementing `_length` by a 16-bit value expands the data object's readable/writable range.

### Option B: ObjC Object ISA Corruption (Type Confusion)

Incrementing the low 16 bits of an ISA pointer changes the object's class → type confusion → method dispatch to wrong vtable.

### Option C: Bitmap Context Buffer Length

If we can place a CGBitmapContext's backing buffer metadata adjacent to the coverage buffer, corrupting its dimensions gives OOB access via the standard CoreGraphics API.

## 6. PDF-Based Heap Spray Techniques

The challenge: in a zero-click scenario (iMessage/WhatsApp), we can only control the PDF content. Heap spray must be done through PDF features:

### Embedded Resources
```pdf
% Multiple embedded images of target size
1 0 obj << /Type /XObject /Subtype /Image
  /Width W /Height H /ColorSpace /DeviceRGB
  /BitsPerComponent 8 /Length SIZE >>
stream
[controlled data of exact target size]
endstream
```

### Multiple Font Descriptors
Each font in the PDF triggers font loading allocations. By embedding multiple crafted fonts, we can spray the target size class.

### Content Stream Operations
PDF operators that trigger CoreGraphics allocations:
- `Do` (draw XObject) → image decoding allocations
- `Tf` (set font) → font loading allocations
- `re` / `f` (rectangle/fill) → path/coverage allocations

## 7. Enhanced PoC Architecture

```
enhanced_trigger.pdf
├── Multiple spray fonts (fill target size class)
├── Multiple inline images (spray objects)
├── Victim CFData object (placed via specific allocation pattern)
├── Trigger font (CVE-2026-86950, with tuned DX for target size class)
└── Content stream ordering:
    1. Spray operations (fill free lists)
    2. Victim allocation trigger
    3. Trigger glyph rendering (OOB write)
    4. Read-back via corrupted victim
```

## 8. Implementation Roadmap

### Step 1: Size Class Calibration
- Determine which malloc size class the coverage buffer lands in
- Adjust DX to target a useful size class
- Verify with heap logging (`MallocStackLogging=1`)

### Step 2: Spray Verification
- Build a PDF with spray objects of the target size
- Verify adjacency with heap inspection tools
- Confirm the spray objects fill the region predictably

### Step 3: OOB Offset Calibration
- Determine exactly where the OOB write lands relative to buffer end
- Adjust trigger coordinates to hit the victim's length field
- Verify with ASAN or guard malloc

### Step 4: Victim Corruption Verification
- Confirm the 16-bit increment hits the target field
- Verify the corrupted object is still usable (not corrupted beyond repair)
- Read past the original boundary to leak heap metadata

### Step 5: Arbitrary R/W Construction
- Use leaked pointers to build fake objects
- Establish stable read/write primitive
- Verify with controlled reads at known addresses

## 9. Tools Needed

- **Xcode** with Address Sanitizer (ASAN) for detecting OOB precisely
- **MallocStackLogging** for tracking allocation sites and sizes
- **lldb** for runtime inspection of heap layout
- **heap** command-line tool for heap analysis
- **vmmap** for virtual memory layout
- **Frida** (if device has developer mode) for runtime instrumentation

## References

- [Synacktiv — Heapple Pie (libmalloc internals)](https://synacktiv.com/ressources/Sthack_2018_Heapple_Pie.pdf)
- [Azeria Labs — Grooming the iOS Kernel Heap](https://azeria-labs.com/grooming-the-ios-kernel-heap/)
- [Calif.io — The Great Glyph Grift](https://github.com/califio/publications/tree/main/MADBugs/CVE-2026-86950)
- [Project Zero — FORCEDENTRY Sandbox Escape](https://projectzero.google/2022/03/forcedentry-sandbox-escape.html)
- [Brandon Azad — One Byte to Rule Them All](https://projectzero.google/2020/07/one-byte-to-rule-them-all.html)
