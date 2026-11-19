# iPhone Duo Full-Screen IPA Patching

Patch iOS IPAs to use the full-screen display on iPhone Duo (or any device with a larger screen) by bumping the Mach-O SDK version and fixing binary malformations that newer dyld rejects.

## Overview

Apps built with an older SDK version are letterboxed on devices with newer, larger displays. Bumping the `LC_BUILD_VERSION` SDK field to 27.1 (iOS 27.1) tells the system the app supports the full screen. However, iOS 27.1's dyld is stricter about Mach-O validation — many older binaries have pre-existing malformations (non-zero offsets on zerofill sections) that were silently tolerated by older dyld but cause crashes on 27.1.

## The Zerofill Section Fix (Critical)

**Problem**: iOS 27.1 dyld strictly validates that zerofill sections (`S_ZEROFILL`, `S_THREAD_LOCAL_ZEROFILL`, `S_GB_ZEROFILL`) have `offset=0`. Many older frameworks have non-zero offsets in these sections — a pre-existing malformation tolerated by older dyld. When you bump SDK to 27.1, these apps crash on launch with:

```
Library not loaded: @rpath/SomeFramework.framework/SomeFramework
  Reason: section '__DATA/__thread_bss' has a zero-fill section type, but offset field is not zero
```

**Solution**: After bumping the SDK, scan ALL Mach-O binaries (main executable + every embedded framework/dylib) and set `offset=0` for any section whose type is a zerofill variant.

**Zerofill section types** (low byte of the `flags` field):
- `S_ZEROFILL` = `0x01`
- `S_GB_ZEROFILL` = `0x0C`
- `S_THREAD_LOCAL_ZEROFILL` = `0x12`

## Mach-O Layout Reference

### LC_BUILD_VERSION (cmd 0x32)
SDK field is at offset +16 from the load command start, 4 bytes little-endian.
Packed as: `(major << 16) | (minor << 8) | patch`
For SDK 27.1.0: `(27 << 16) | (1 << 8) | 0 = 0x001B0100`

### LC_SEGMENT_64 (cmd 0x19) — 64-bit
```
Offset  Size  Field
0       4     cmd (0x19)
4       4     cmdsize
8       16    segname
24      8     vmaddr
32      8     vmsize
40      8     fileoff
48      8     filesize
56      4     maxprot
60      4     initprot
64      4     nsects      <-- number of sections
68      4     flags
72      ...   sections start here
```

### section_64 struct (80 bytes each)
```
Offset  Size  Field
0       16    sectname
16      16    segname
32      8     addr
40      8     size
48      4     offset      <-- must be 0 for zerofill types
52      4     align
56      4     reloff
60      4     nreloc
64      4     flags       <-- low byte = section type
68      4     reserved1
72      4     reserved2
76      4     reserved3
```

### LC_SEGMENT (cmd 0x01) — 32-bit
```
Offset  Size  Field
0       4     cmd (0x01)
4       4     cmdsize
8       16    segname
24      4     vmaddr
28      4     vmsize
32      4     fileoff
36      4     filesize
40      4     maxprot
44      4     initprot
48      4     nsects
52      4     flags
56      ...   sections start here (68 bytes each)
```

### section struct (68 bytes each, 32-bit)
```
Offset  Size  Field
0       16    sectname
16      16    segname
32      4     addr
36      4     size
40      4     offset      <-- must be 0 for zerofill types
44      4     align
48      4     reloff
52      4     nreloc
56      4     flags       <-- low byte = section type
60      4     reserved1
64      4     reserved2
```

### FAT binary header
```
magic:     0xBEBAFECA (big-endian)
nfat_arch: big-endian uint32 at offset 4
Each fat_arch (20 bytes):
  offset 8:  cputype (big-endian)
  offset 12: cpusubtype (big-endian)
  offset 16: arch offset in file (big-endian)  <-- use this as base
  offset 20: size (big-endian)
  offset 24: align (big-endian)
```

## Complete Patching Algorithm (Python)

```python
import struct

TARGET_SDK = (27, 1, 0)

def patch_binary(binary_path, target=TARGET_SDK):
    """Patch SDK version AND fix zerofill section offsets."""
    with open(binary_path, 'rb') as f:
        data = bytearray(f.read())
    new_sdk = (target[0] << 16) | (target[1] << 8) | target[2]
    modified = False
    magic = struct.unpack('<I', data[:4])[0]
    if magic == 0xBEBAFECA:  # FAT binary
        num_archs = struct.unpack('>I', data[4:8])[0]
        for i in range(num_archs):
            arch_offset = struct.unpack('>I', data[8+i*20+8:8+i*20+12])[0]
            modified |= patch_arch(data, arch_offset, new_sdk)
    elif magic in (0xFEEDFACF, 0xFEEDFACE):  # Single arch
        modified = patch_arch(data, 0, new_sdk)
    if modified:
        with open(binary_path, 'wb') as f:
            f.write(data)
    return modified

def patch_arch(data, base, new_sdk):
    magic = struct.unpack('<I', data[base:base+4])[0]
    is64 = (magic == 0xFEEDFACF)
    hdr_size = 32 if is64 else 28
    ncmds = struct.unpack('<I', data[base+16:base+20])[0]
    pos = base + hdr_size
    modified = False
    for _ in range(ncmds):
        if pos + 8 > len(data): break
        cmd = struct.unpack('<I', data[pos:pos+4])[0]
        cmdsize = struct.unpack('<I', data[pos+4:pos+8])[0]
        if cmdsize < 8: break
        # Patch SDK in LC_BUILD_VERSION
        if cmd == 0x32:
            sdk_off = pos + 16
            if sdk_off + 4 <= len(data):
                struct.pack_into('<I', data, sdk_off, new_sdk)
                modified = True
        # Fix zerofill sections in LC_SEGMENT_64
        if cmd == 0x19:
            nsects = struct.unpack('<I', data[pos+64:pos+68])[0]
            for s in range(nsects):
                so = pos + 72 + s * 80
                if so + 80 > len(data): break
                offset_off = so + 48
                flags_off = so + 64
                offset = struct.unpack('<I', data[offset_off:offset_off+4])[0]
                flags = struct.unpack('<I', data[flags_off:flags_off+4])[0]
                sect_type = flags & 0xFF
                if sect_type in (1, 0x12, 0xC) and offset != 0:
                    struct.pack_into('<I', data, offset_off, 0)
                    modified = True
        # Fix zerofill sections in LC_SEGMENT (32-bit)
        if cmd == 0x1:
            nsects = struct.unpack('<I', data[pos+48:pos+52])[0]
            for s in range(nsects):
                so = pos + 56 + s * 68
                if so + 68 > len(data): break
                offset_off = so + 40
                flags_off = so + 56
                offset = struct.unpack('<I', data[offset_off:offset_off+4])[0]
                flags = struct.unpack('<I', data[flags_off:flags_off+4])[0]
                sect_type = flags & 0xFF
                if sect_type in (1, 0x12, 0xC) and offset != 0:
                    struct.pack_into('<I', data, offset_off, 0)
                    modified = True
        pos += cmdsize
    return modified
```

## Full IPA Patching Workflow

1. **Extract IPA** (it's a ZIP): `unzip app.ipa -d workdir/`
2. **Patch all binaries** — main executable + every `.framework` and `.dylib` under `Frameworks/`:
   - Bump `LC_BUILD_VERSION` SDK to 27.1
   - Fix zerofill section offsets (set offset=0 for S_ZEROFILL/S_THREAD_LOCAL_ZEROFILL/S_GB_ZEROFILL)
3. **Set `UIRequiresFullScreen` to false** in Info.plist (if it's set to true, the app won't use the full screen)
4. **Check for scene delegate support**: If the app already has `UIApplicationSceneManifest` in Info.plist, it has scene support — just the SDK bump is usually enough. If not, scene delegate injection *may* be needed for some apps.
5. **Re-sign everything**:
   - Sign all embedded frameworks/dylibs first: `codesign -f -s "IDENTITY" --preserve-metadata=entitlements framework`
   - Sign the main app last: `codesign -f -s "IDENTITY" --entitlements ent.plist app.app`
6. **Repackage**: `cd workdir && zip -r ../patched.ipa Payload/`
7. **Install**: `pymobiledevice3 apps install patched.ipa --udid DEVICE_UDID`

## Scene Delegate Injection

For apps WITHOUT scene delegate support (no `UIApplicationSceneManifest` in Info.plist), you may need to inject one. However:

**WARNING for React Native apps**: React Native manages its own window lifecycle. Injecting a scene delegate into React Native apps causes crashes. For React Native apps, the SDK bump + zerofill fix alone is usually sufficient.

When injection IS needed (pure UIKit apps without scene support):
1. Compile a DuoSceneDelegate dylib
2. Add it to the app's Frameworks directory
3. Add `LC_LOAD_DYLIB` to the main binary (use `install_name_tool` or `optool`)
4. Add `UIApplicationSceneManifest` to Info.plist with the delegate class name

**NEVER zero out an LC_LOAD_DYLIB command** to remove it — this corrupts the Mach-O because dyld reads cmd=0/cmdsize=0 and enters an infinite loop. To undo scene delegate injection, start from a clean copy of the original binary.

## Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| Crash: "section has zero-fill type but offset not zero" | Zerofill malformation | Run zerofill fix on ALL binaries |
| Crash on launch (React Native app with scene delegate) | Scene delegate conflicts with RN window management | Remove scene delegate, use SDK bump only |
| App installs but letterboxed | SDK not bumped in all architectures | Ensure FAT binary handling patches all arches |
| Signing fails | Frameworks not signed before main app | Sign frameworks first, app last |
| "Library not loaded" after patching | Binary corruption from improper LC_LOAD_DYLIB removal | Start from clean original binary |
| Install fails: InvalidCompanionAppBundleIdentifier | Watch app's WKCompanionAppBundleIdentifier doesn't match new bundle ID | Update WKCompanionAppBundleIdentifier in Watch app plist, CFBundleIdentifier in Watch app + extension, and WKAppBundleIdentifier in Watch extension's NSExtensionAttributes |
| Install fails: entitlements "invalid length in entitlement blob" | Entitlements extracted in DER format | Extract with `codesign -d --entitlements - --xml` to get XML format |
| Crash: `___UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption_block_invoke` | iOS 27 enforces scene lifecycle for SDK ≥27.0 apps without UIApplicationSceneManifest | Inject scene delegate: add UIApplicationSceneManifest to Info.plist + DuoSceneDelegate.dylib + LC_LOAD_DYLIB |
| WKWebView "Oops" / web content fails after re-sign | Re-signing with dev cert breaks server-side integrity checks (e.g. Steam Library) | Not fixable — this is a server-side check, not caused by SDK bump or scene delegate. Confirmed by testing re-sign-only (no SDK bump) which still fails. |
| Scene lifecycle enforced at SDK 27.0, not just 27.1 | Tested: SDK 27.0 without scene delegate still crashes with scene lifecycle enforcement | Use SDK 27.1 (same enforcement) with scene delegate |

## Scene Delegate Approaches (Lessons Learned)

For apps that need an injected scene delegate, the approach matters:

| Approach | Result | Why |
|----------|--------|-----|
| Scene delegate creates NEW window | App works but may break web views | Conflicts with app's own window management |
| Empty/minimal scene delegate (no-op) | Black screen | UIKit expects scene delegate to set up window when scene lifecycle is active |
| Swizzle UIWindow methods + minimal delegate | **Best approach** — app's own window management runs untouched | Swizzles `makeKeyAndVisible` and `setHidden:` to auto-assign windowScene to any window the app creates. Scene delegate just captures the scene reference. |

**Recommended swizzle approach**: Instead of a scene delegate that takes over window management, swizzle `UIWindow.makeKeyAndVisible` and `UIWindow.setHidden:` to automatically assign the global `UIWindowScene` to any window that doesn't have one. The scene delegate's only job is to capture the scene reference. This preserves the app's native window lifecycle.

## Tested Apps

| App | Scene Delegate Needed? | Zerofill Fix Needed? | Notes |
|-----|----------------------|---------------------|-------|
| Steam | Yes (swizzle approach) | No | Pure UIKit, no scene support. Library tab broken due to re-signing (server-side check), not fixable. Store/Profile tabs work. |
| LocalSend | No | No | Already had scene support |
| iTorrent | No | No | Already had scene support |
| BatteryCam (Eufy) | Yes (injected) | Yes (13 frameworks) | Kotlin Multiplatform (KMP) app, crashes in MegaCommonFramework regardless — incompatible with iOS 27. Has Watch app requiring bundle ID fixup. |
