# iOS 27.1 Jailbreak Research Handoff

## Project Goal
Install Sileo package manager on iPhone Duo (A20 Pro, iOS 27.1) via a kernel exploit chain. This is educational security research on the owner's own device.

---

## Device & Environment

| Field | Value |
|-------|-------|
| Device | iPhone Duo |
| SoC | A20 Pro (T8160) |
| iOS | 27.1 build 24A94232 |
| Kernel | xnu-13432.2.10~176/RELEASE_ARM64_T8160 |
| RAM | ~11.3 GB |
| Page size | 16384 bytes |
| CPUs | 6 (2P + 4E) |
| UDID | 00008160-000A4C840E400036 |
| Security | SPTM + MIE (Memory Integrity Enforcement via Enhanced Memory Tagging) |

**Key architectural facts:**
- **SPTM** protects page tables and code-signing. Does NOT protect proc/ucred/task credential structures.
- **MIE** is heap-level memory safety via Enhanced Memory Tagging Extension (A19/M5+). A20 Pro has MIE.
- **Lockdown Mode is OFF** — this means MIE/MTE is NOT active in PDF rendering / Preview / Quick Look. Standard memory corruption works in CoreGraphics.
- **kalloc_type isolation**: Objects grouped by type signature, not just size (iOS 15+).
- **Kernel R/W alone suffices** for credential patching to get root. No SPTM bypass needed.

---

## Repository & Code

- **Repo:** `github.com/kaioh1990/Kaioh`
- **Branch:** `claude/youthful-hamilton-ee3o5y`
- **Main exploit file:** `.claude/skills/ios-reverse-engineering/poc/ave/ave_full_chain.c`
  - ~101,000+ lines, single C file
  - Bundle ID: `com.research.avefullchain`
  - Compiled as C (NOT Objective-C) — no `@{}`, `NSDictionary`, `__bridge` allowed

### Build & Deploy Flow (on Mac)

```bash
# 1. Sync from git
cd /tmp/kaioh_repo && git fetch origin claude/youthful-hamilton-ee3o5y && git reset --hard origin/claude/youthful-hamilton-ee3o5y

# 2. Copy source
cp .claude/skills/ios-reverse-engineering/poc/ave/ave_full_chain.c ~/a20_research/ave_full_chain.c

# 3. Build
cd ~/a20_research && xcodebuild -project AVEFullChainProj.xcodeproj -scheme AVEFullChain -configuration Release -destination 'generic/platform=iOS' build

# 4. Install
xcrun devicectl device install app --device 00008160-000A4C840E400036 /Users/ttameirao/Library/Developer/Xcode/DerivedData/AVEFullChainProj-dtxgjtkpwbehmgexpsnrmjzxzreq/Build/Products/Release-iphoneos/AVEFullChain.app

# 5. Run (replace <N> with round number)
xcrun devicectl device process launch --device 00008160-000A4C840E400036 --console com.research.avefullchain -- iosurface_sel17 <N>
```

Build flags: `-framework IOSurface -framework IOKit -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation -lcompression -framework Metal -framework CoreGraphics -framework ImageIO`

Signing: Team 9QP5427NJJ, "Apple Development: Travis Tameirao (7TJQC38PMP)"

### Dispatch Table (round number → function)

| rounds= | Function | Description | Status |
|---------|----------|-------------|--------|
| 507 | r502b_exploit() | IOMFB deep probe sels 0-80, all 5 conn types + JPEG/KeyStore scan | RAN - IOMFB dead end |
| 508 | r503_exploit() | IOSurface sel 19 heap write + sels 30/37/45 deep probe | RAN - sel 19 works with sz=4 after surface creation |
| 509 | r504_exploit() | AppleKeyStore data extraction + IOSurface property UAF race | RAN - KeyStore returns all zeros |
| 510 | r505_exploit() | Sel 19 exact size boundary + registry observation + timing | RAN - sel 19 fails on bare conn (no surface) |
| 511 | r506_exploit() | NTL OOB + IOSurface property spray feedback channel | NOT RUN - has ObjC compile errors to fix |
| 512 | r507_exploit() | Sel 19 prereq test + KASLR timing + KeyStore info leak | NOT RUN - clean C, ready to build |

Earlier rounds (496-506) cover prior exploration. The dispatch is at approximately line 101229+ in the file.

---

## Confirmed Primitives

### 1. NTL OOB Write (CVE-2026-112705)
- **What:** AVEVideoEncoder `NumberOfTemporalLayers` out-of-bounds write
- **How:** Set NumberOfTemporalLayers to a large value on an HEVC compression session, then encode a frame
- **Effect:** Writes the value `1` (uint32) at a 4-byte stride offset from an internal AVE buffer
- **Limitation:** BLIND — no observation from userspace. HEVC-only. Needs KASLR bypass to know where kernel objects are.
- **Code pattern:**
```c
VTCompressionSessionCreate(..., kCMVideoCodecType_HEVC, ..., &sess);
VTSessionSetProperty(sess, CFSTR("NumberOfTemporalLayers"), ntl_number);
VTCompressionSessionEncodeFrame(sess, pixbuf, pts, kCMTimeInvalid, NULL, NULL, NULL);
```

### 2. IOSurface Selector 19 (potential write primitive)
- Accepts exactly 4 bytes of struct input, returns KERN_SUCCESS
- Works ONLY after at least one IOSurface has been created (fails on bare connection)
- Side effect: adds properties to surface dictionary after many calls
- All other sizes (0-3, 5+) return kIOReturnNotPermitted
- Works with fake surface IDs and without a surface reference
- Timing: ~1.3us/call (vs 0.6us for rejected selectors — 115% overhead, so it does kernel work)

### 3. AppleKeyStore Accessible Selectors
- **sel 0, 1, 16:** Return OK with up to 512 bytes output (all zeros so far)
- Multiple connection types work (0-4)
- sel 5, 6, 20-22, 33-34, 36-39, 41: return kIOReturnExclusiveAccess (0xe00002c1) — exist but locked
- sel 19: returns kIOReturnNotReadable (0xe00002e2)
- **NOT YET TESTED:** crafted input opcodes (r507 Phase 4 does this)

---

## Dead Ends (confirmed, do not revisit)

| Attack Surface | Why It's Dead |
|----------------|---------------|
| **IOSurface sel 17/18 UAF** (CVE-2026-112477) | Selectors 17/18 return kIOReturnNotPermitted (0xe00002c2). Entitlement-gated via `sMethodDescsRestricted` table. |
| **IOSurface property race** | SetValue/RemoveValue/CopyValue fully serialized internally. 10000+ iterations, 0 corruption, 0 type confusion. |
| **IOSurface mach port race** | CreateMachPort/LookupFromMachPort/Destroy — port keeps kernel object alive. 500/500 double_lookup, not exploitable. |
| **IOMobileFrameBuffer** | Opens with ALL 5 connection types (unusual!) but every selector 0-80 returns kIOReturnNotPermitted or kIOReturnExclusiveAccess. |
| **AGXAccelerator / AGXFamily** | Returns 0xe00002c7 (kIOReturnNotReady) — cannot connect. |
| **AppleSEPManager** | kIOReturnNotPermitted — cannot connect. |
| **ANE (Neural Engine)** | 8 driver names tested, none accessible. |
| **IOSurface sels 9/10/11** (SetValue/GetValue/RemoveValue raw) | All return kIOReturnNotPermitted on direct IOConnectCallMethod. Only accessible via framework-level API (which serializes). |
| **IOSurface accessible sels** | Only 4 accessible: 19 (write), 30 (kIOReturnUnsupported/ExclusiveAccess), 37 (kIOReturnBadArgument), 45 (kIOReturnBadArgument). None useful for info leak. |

## Accessible IOKit Drivers (from Phase 6 scan)

| Driver | Connection Types | Notes |
|--------|-----------------|-------|
| IOSurfaceRoot | Type 0 | Main research target, heavily locked down |
| IOMobileFramebuffer | Types 0-4 (all 5!) | Opens but all selectors restricted |
| AppleJPEGDriver | Types 0-2 | Needs selector probing with crafted JPEG data |
| AppleKeyStore | Types 0-4 | Sels 0/1/16 accessible, promising for info leak |

---

## The Critical Blocker: KASLR Bypass

**We have no info leak.** This is the #1 blocker.

Attempted and failed:
- host_statistics64: works but returns no kernel pointers
- task_info (TASK_VM_INFO): returns user-space addresses only
- IOSurface IDs: sequential (no entropy leak)
- Mach port names: normal userspace values
- IOSurface property readback after races: no leaked pointers
- AppleKeyStore sel 0/1/16 output: all zeros
- IOMFB connection handles: no kernel pointers

**Untested approaches (for r507/r512):**
1. AppleKeyStore with crafted opcode bytes in input struct (32 opcodes planned)
2. AppleKeyStore wider selector sweep 0-100 checking for kernel pointers in output
3. Timing side channels on kernel calls for KASLR estimation
4. NTL OOB + property spray for feedback (can we observe the blind write?)

---

## NEW PRIMARY ATTACK VECTOR: CoreGraphics PDF/Font Parsing

### Critical Intel (November 2026)

Two CoreGraphics vulnerabilities were patched in iOS 27.2 (November 20, 2026) and are **LIVE on our iOS 27.1 device**:

| CVE | Component | Description |
|-----|-----------|-------------|
| **CVE-2026-112611** | CoreGraphics | PDF and font parsing bug, credited to Calif.io |
| **CVE-2026-113950** | CoreGraphics | PDF and font parsing bug, credited to Calif.io |

These were used in **Calif.io's October 2026 jailbreak** — a real, working exploit chain.

### Why This Is The Path Forward

1. **MTE/MIE is NOT active** in the PDF rendering path (Lockdown Mode is OFF on our device). Standard memory corruption works — no tag-matching dance needed.
2. **CoreGraphics is accessible from any app** — load a crafted PDF via `CGPDFDocument` / `CGContextDrawPDFPage`, no entitlements needed.
3. **Can also be triggered via AirDrop** — the receiving device renders a PDF preview through Quick Look / CoreGraphics.
4. **Font parsing** (CFF charstrings, TrueType glyph outlines, OpenType tables inside PDFs) is historically one of the richest exploit surfaces in iOS.
5. **These bugs were used in a real jailbreak** — meaning they provide sufficient primitives for a full chain.

### Calif.io's Documented Methodology (from Coruna research)

Based on their public research, modern iOS exploit chains:
1. Use **non-memory logic bugs or type confusions** to read memory first (info leak)
2. **Leak pointer tags or target un-tagged memory regions** to map address space
3. Achieve **arbitrary R/W with valid matching tags**

Since Lockdown Mode is off, step 2 is simplified — no tag leaking needed.

### Research Tasks for This Vector

**HIGH PRIORITY — do these first:**
1. Search for ANY public details on CVE-2026-112611 and CVE-2026-113950 (Apple security notes, blog posts, conference talks, commit diffs)
2. Check Apple's iOS 27.2 security release notes for exact descriptions of these CVEs
3. Look for CoreGraphics / ImageIO commits between iOS 27.1 and 27.2 builds
4. Research Calif.io's public disclosures and blog posts from October-November 2026
5. Study CFF (Compact Font Format) charstring interpreter in CoreGraphics — historically the #1 font parsing attack surface
6. Study TrueType/OpenType table parsing in CoreGraphics (cmap, glyf, head, hhea, hmtx, loca, maxp, name, post tables)

**IMPLEMENTATION:**
1. Add CGPDFDocument rendering to the test app (load PDF from app bundle, render it)
2. Craft malformed PDFs with:
   - Corrupt CFF charstring operands (stack overflows, type confusion)
   - Malformed TrueType glyph tables (out-of-bounds reads/writes)
   - Invalid ICC color profiles
   - Nested font streams with conflicting metadata
3. Monitor for crashes, then analyze crash logs for exploitability

### How to Trigger PDF Parsing From Our App

```c
// Load a crafted PDF from the app bundle
CFURLRef pdfURL = CFBundleCopyResourceURL(CFBundleGetMainBundle(), 
    CFSTR("malformed"), CFSTR("pdf"), NULL);
CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(pdfURL);
if (doc) {
    CGPDFPageRef page = CGPDFDocumentGetPage(doc, 1);
    // Create a bitmap context and render — this triggers font/glyph parsing
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, 256, 256, 8, 1024, cs,
        kCGImageAlphaPremultipliedLast);
    CGContextDrawPDFPage(ctx, page);
    // If we get here without crashing, check context contents for leaked data
    void *data = CGBitmapContextGetData(ctx);
    // Scan rendered output for kernel pointers or unexpected values
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGPDFDocumentRelease(doc);
}
CFRelease(pdfURL);
```

### Revised Exploit Chain Architecture

```
1. CoreGraphics PDF/Font Bug        → Type confusion or OOB in font parser
   (CVE-2026-112611 / 113950)         (info leak + write primitive)
2. KASLR Bypass                     → Leaked kernel pointer from step 1
3. Kernel R/W                       → Target proc/ucred via corrupted primitive
4. Credential Patching              → Set uid=0, disable sandbox
5. Post-exploitation                → Remount rootfs, install Sileo
```

This replaces the previous IOKit-based chain which was blocked at the KASLR bypass step.

---

## Previous Approach (IOKit — deprioritized but not abandoned)

The IOKit research below is kept for reference. If the CoreGraphics vector doesn't pan out, the NTL OOB write and AppleKeyStore leads remain available.

---

## Immediate TODO (Priority Order)

### 0. (NEW) Research CoreGraphics CVEs and build PDF fuzzing into the app

### 1. Fix r506 compile errors (rounds=511)
The function at line ~30324 uses Objective-C syntax that won't compile in a .c file:
- `NSDictionary *pba = @{...}` → convert to CFDictionaryCreateMutable + CFDictionarySetValue
- `@(width)` → CFNumberCreate
- `@YES` → kCFBooleanTrue
- `@{}` → CFDictionaryCreateMutable(NULL, 0, ...)
- `(__bridge CFDictionaryRef)` → just use CFDictionaryRef directly (no bridge needed in C)
- `(NSString *)kVTVideoEncoderSpecification_...` → just use the constant directly

### 2. Run r507 (rounds=512) — already compiles
Tests:
- Phase 1: sel 19 before surface creation (expect fail)
- Phase 2: sel 19 after surface creation (expect success) 
- Phase 3: KASLR timing via kernel call latency (50000 iterations each)
- Phase 4: AppleKeyStore crafted opcodes 0-31 on sels 0/1/16 + wider selector probe 0-100

### 3. Run r506 (rounds=511) after fixing
Tests NTL OOB with IOSurface property spray — 256 surfaces with sentinel properties, free alternates to create holes, trigger HEVC NTL OOB at 10 different values, check for property corruption.

### 4. Based on results, either:
- **If AppleKeyStore leaks pointers:** Build KASLR calculation from leaked address
- **If NTL corrupts properties:** We have a feedback channel — develop targeted write
- **If neither works:** Need to find new attack surface or technique

---

## Exploit Chain Architecture (once KASLR is solved)

```
1. KASLR Bypass (info leak)           → Learn kernel slide
2. Heap Feng Shui                     → Position target objects predictably  
3. NTL OOB Write (CVE-2026-112705)    → Corrupt kernel object (proc/ucred)
4. Credential Patching                → Set uid=0, disable sandbox
5. Post-exploitation                  → Remount rootfs, install Sileo
```

Step 4 is straightforward once we have kernel R/W because proc/ucred is NOT SPTM-protected.

---

## Code Style Notes

- All code must be pure C (compiled as .c, not .m)
- Use `rlog(fmt, ...)` for output (macro wrapping printf with timestamp)
- Each exploit round is `static void rNNN_exploit(void)` 
- Dispatch in main() around line 101229+
- Fast-path gate around line 104300 must exclude new round numbers
- Always `fflush(stdout)` after key output sections
- `#include` everything needed is already at the top of the file
- Use CoreFoundation (CFDictionaryCreateMutable, CFNumberCreate, etc.) not Foundation/ObjC
