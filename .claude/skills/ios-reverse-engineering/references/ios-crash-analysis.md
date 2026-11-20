# Detecting OOB Memory Corruption on iOS Without a Jailbreak

## Purpose

Techniques for observing CVE-2026-86950's OOB write behavior on a non-jailbroken iPad Mini (iOS 27.0.1) connected via USB to a Mac. Since macOS 27.0.1 is patched (confirmed by Guard Malloc), the OOB write only triggers on iOS — so we need iOS-side instrumentation.

---

## 1. Crash Log Extraction via pymobiledevice3

The most direct way to see what happened after triggering the PDF.

### Pull crash reports

```bash
# Pull all crash reports to a local directory
pymobiledevice3 crash pull /path/to/local/crashes

# On iOS 17+ you may need a tunnel first:
sudo python3 -m pymobiledevice3 remote tunneld
# Then pass --tunnel '' to commands
pymobiledevice3 crash pull --tunnel '' /path/to/crashes
```

### Live syslog streaming

```bash
# Stream all syslog lines (very noisy)
pymobiledevice3 syslog live

# Filter to CoreGraphics / ImageIO messages
pymobiledevice3 syslog live -m CoreGraphics
pymobiledevice3 syslog live -m ImageIO

# Filter for crash-related messages
pymobiledevice3 syslog live -m ReportCrash
pymobiledevice3 syslog live -m MobileSafari
```

### Python API (programmatic access)

```python
from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.syslog import SyslogService

lockdown = create_using_usbmux()
for line in SyslogService(service_provider=lockdown).watch():
    if 'CoreGraphics' in line or 'aa_cache' in line:
        print(line)
```

### What to look for in crash reports

- **Exception type:** `EXC_BAD_ACCESS (SIGSEGV)` or `EXC_BAD_ACCESS (SIGBUS)` with `KERN_INVALID_ADDRESS`
- **Fault address:** The address the OOB write tried to reach — compare against the coverage buffer's allocation range
- **Crashing thread backtrace:** Look for frames in CoreGraphics, particularly `aa_cache_render`, `aa_render`, `ripc_DrawGlyphs`
- **VM region info:** Shows whether the fault address falls in a mapped heap region or a guard page

**Limitation:** Our bare PoC doesn't crash because the OOB write hits mapped heap memory, not a guard page. We won't get a crash report unless we either: (a) increase the OOB offset enough to reach unmapped memory, or (b) the corrupted heap metadata causes a later crash during deallocation.

---

## 2. Xcode Malloc Debugging on iOS Devices

Xcode can inject malloc debugging environment variables into apps launched on a connected device.

### Setting up via Xcode Scheme Editor

1. Create a minimal iOS app that loads and renders the trigger PDF
2. In Xcode: **Product → Scheme → Edit Scheme → Run → Diagnostics**
3. Enable the following:

| Checkbox | Env Variable | Effect |
|----------|-------------|--------|
| **Guard Malloc** | `DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib` | Each allocation on its own page with guard pages on both sides. Catches any OOB byte. Very slow. |
| **Malloc Guard Edges** | `MallocGuardEdges=1` | Guard pages around large allocations only |
| **Malloc Scribble** | `MallocScribble=1` | Fills freed memory with `0x55`, new memory with `0xAA` |
| **Malloc Stack Logging** | `MallocStackLogging=1` | Records allocation call stacks for `leaks` and `malloc_history` |

Alternatively, in the **Arguments** tab, add environment variables directly:
- `MallocGuardEdges` = `1`
- `MallocScribble` = `1`
- `MallocStackLogging` = `1`
- `MallocCheckHeapStart` = `1`
- `MallocCheckHeapEach` = `100`

### Key limitation

These environment variables only apply to **your app process**, not to system daemons. If the PDF is opened in Safari or Files, Safari's process gets the default allocator — no guard pages, no scribble. You need a custom app that calls `CGImageSourceCreateThumbnailAtIndex` directly, launched from Xcode with these flags set.

### Recommended test app

```swift
import UIKit
import ImageIO

class ViewController: UIViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        
        guard let url = Bundle.main.url(forResource: "exploit", withExtension: "pdf") else {
            print("PDF not found in bundle")
            return
        }
        
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            print("CGImageSourceCreateWithURL failed")
            return
        }
        
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: 256
        ]
        
        print("Creating thumbnail...")
        let img = CGImageSourceCreateThumbnailAtIndex(source, 0, opts as CFDictionary)
        print("Result: \(String(describing: img))")
    }
}
```

Bundle `exploit.pdf` in the app, deploy to the iPad via Xcode with Guard Malloc or MallocGuardEdges enabled in the scheme, and watch for `EXC_BAD_ACCESS`.

---

## 3. Address Sanitizer (ASan) on iOS Devices

### How it works

ASan replaces `malloc`/`free` with an instrumented allocator that places "red zones" (poisoned shadow memory) around every allocation. Any read or write to a red zone triggers a detailed diagnostic report.

### Setup in Xcode

**Product → Scheme → Edit Scheme → Run → Diagnostics → Address Sanitizer** (checkbox)

This compiles YOUR code with ASan instrumentation and replaces the allocator.

### Critical limitation for CVE-2026-86950

ASan only catches OOB accesses in **instrumented code** — code compiled with `-fsanitize=address`. Apple's system frameworks (CoreGraphics, ImageIO) are NOT compiled with ASan. The OOB write happens inside `aa_cache_render` in CoreGraphics.framework, which is uninstrumented.

**What ASan CAN catch:**
- The allocation itself (ASan's malloc replacement adds red zones)
- If the OOB write reaches ASan's red zone, the hardware fault handler catches it
- Downstream corruption when your instrumented code reads the corrupted memory

**What ASan CANNOT catch:**
- Writes within the same malloc quantum (ASan red zones are typically 16–256 bytes)
- Writes that land in another live allocation's valid range (no red zone between adjacent same-size allocations in the same magazine)

### Verdict

ASan is worth enabling but is not reliable for this specific bug. Guard Malloc (which uses hardware page protection) is more reliable because it catches every OOB byte regardless of instrumentation. We confirmed this on macOS: Guard Malloc with the original DX=33 trigger survived, proving no OOB on the patched macOS build.

---

## 4. Instruments Profiling

### Allocations Instrument

Track every `malloc`/`free` call and inspect the heap state:

1. Open Instruments → choose **Allocations** template
2. Select the iPad as target, choose the test app
3. Record while the app renders the PDF
4. After rendering, inspect allocations in the ~4KB–5KB range (our target size class)
5. Look for allocations from `aa_cache_render` or `CGGlyphBitmapCreate`

### Zombies Instrument

Detects use-after-free — if the OOB write corrupts a freed object's metadata and something later accesses it, Zombies will catch the access to the freed ("zombie") object.

### Leaks Instrument

Detects leaked allocations — less directly useful, but if the OOB write corrupts a pointer, the original allocation may become unreachable (leaked).

### Limitation

Instruments runs on the Mac and communicates with the device over USB. It can profile the target app process but cannot instrument system framework internals. The profiling data tells you WHAT was allocated and WHERE, but not whether an OOB write happened unless it causes a secondary observable effect (crash, zombie access, leaked block).

---

## 5. DTrace / ktrace (Limited on iOS)

DTrace is available on macOS but NOT on non-jailbroken iOS. The `ktrace` facility exists on iOS but requires elevated privileges.

**On macOS (for comparison testing):**
```bash
# Trace malloc calls in the render harness
sudo dtrace -n 'pid$target::malloc:entry { printf("malloc(%d)", arg0); }' \
  -c './render_debug exploit.pdf'

# Trace CoreGraphics aa_ functions (if symbols available)
sudo dtrace -n 'pid$target:CoreGraphics:aa_*:entry { printf("%s", probefunc); }' \
  -c './render_debug exploit.pdf'
```

This won't help on iOS without a jailbreak.

---

## 6. Practical Strategy for iPad Mini Testing

Given the limitations above, here's the recommended approach:

### Tier 1: Immediate (no Xcode project needed)

1. **Stream syslog** while opening the PDF in Safari:
   ```bash
   pymobiledevice3 syslog live -m CoreGraphics &
   # Open exploit.pdf in Safari on the iPad
   ```
2. **Pull crash reports** after the test:
   ```bash
   pymobiledevice3 crash pull ./ipad-crashes/
   ```
3. **Check for new reports** — if the OOB caused a delayed crash (e.g., during GC or deallocation), it will show up here

### Tier 2: Custom test app via Xcode

1. Build a minimal iOS app that bundles `exploit.pdf` and calls `CGImageSourceCreateThumbnailAtIndex`
2. Deploy to iPad with **Guard Malloc** enabled in the scheme
3. Guard Malloc will catch ANY OOB byte — if the write happens, you'll get `EXC_BAD_ACCESS` with the exact fault address
4. This is the **definitive test** for whether iOS 27.0.1 has the OOB write

### Tier 3: Heap state inspection

1. Run the test app under Instruments → Allocations
2. Look for the coverage buffer allocation (~4KB for DX=2, ~66KB for DX=33)
3. Record the allocation address and surrounding allocations
4. If a crash occurs, correlate the fault address with the heap layout

### Tier 4: Induced crash for analysis

If the bare PoC doesn't crash, we can try to FORCE a crash by:
- Increasing the OOB offset (larger `TRIGGER_X` values) until the write reaches unmapped memory
- Using `width_px` values just above 1015 (smallest possible heap buffer) to minimize the gap between buffer end and next page boundary
- Adding `MallocCheckHeapStart=1 MallocCheckHeapEach=1` to validate the heap after every operation

---

## 7. Reading iOS Crash Reports

### Format

```
Incident Identifier: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
Hardware Model:      iPad16,1
OS Version:          iPhone OS 27.0.1 (24A446)
Exception Type:      EXC_BAD_ACCESS (SIGSEGV)
Exception Subtype:   KERN_INVALID_ADDRESS at 0x00000001DEADBEEF
VM Region Info: 0x1deadbeef is not in any region.

Thread 0 Crashed:
0   CoreGraphics    0x00000001a2345678 aa_cache_render + 2120
1   CoreGraphics    0x00000001a2340000 aa_render + 456
2   CoreGraphics    0x00000001a2300000 ripc_DrawGlyphs + 1024
3   ImageIO         0x00000001a1200000 CGImageSourceCreateThumbnailAtIndex + 892
```

### Key fields for CVE-2026-86950

- **Fault address:** Shows exactly where the OOB write landed. Compare with the coverage buffer base + size to compute the OOB offset.
- **`aa_cache_render + offset`:** The offset within the function tells you which instruction caused the fault — correlate with disassembly to confirm it's the coverage buffer write.
- **Thread state registers:** `x0`–`x3` may hold the buffer pointer, the write offset, and the alpha value being written.

### Symbolication

For system frameworks, Apple ships dSYM-equivalent symbols via the shared cache. Use:
```bash
# With atos (requires the device's shared cache symbols):
atos -o /path/to/dyld_shared_cache_arm64e -l 0xLOADADDR 0xFAULTADDR

# Or paste the crash log into Xcode → Devices & Simulators → View Device Logs
# Xcode auto-symbolicates if it has the matching iOS support files
```

---

## References

- [pymobiledevice3 on PyPI](https://pypi.org/project/pymobiledevice3/)
- [Apple — Enabling Malloc Debugging Features](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/ManagingMemory/Articles/mallocDebug.html)
- [Apple — Malloc Debug Environment Variables](https://developer.apple.com/library/archive/releasenotes/DeveloperTools/RN-MallocOptions/)
- [Apple — Diagnosing Memory, Thread, and Crash Issues Early](https://developer.apple.com/documentation/xcode/diagnosing-memory-thread-and-crash-issues-early)
- [Apple — Overflow and Underflow of Buffers](https://developer.apple.com/documentation/xcode/overflow-and-underflow-of-buffers)
- [Apple — Investigating Memory Access Crashes](https://developer.apple.com/documentation/xcode/investigating-memory-access-crashes)
- [Apple — Diagnosing Issues Using Crash Reports and Device Logs](https://developer.apple.com/documentation/xcode/diagnosing-issues-using-crash-reports-and-device-logs)
- [NSHipster — Launch Arguments & Environment Variables](https://nshipster.com/launch-arguments-and-environment-variables/)
- [PSPDFKit — Running Tests with ASan](https://www.nutrient.io/blog/test-with-asan/)
