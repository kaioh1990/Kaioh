#!/bin/bash
set -e
TEAM="9QP5427NJJ"
SIGN_ID="Apple Development: Travis Tameirao (7TJQC38PMP)"
DEST="$HOME/a20_research"
SRC_DIR="$DEST"
DEVICE_UDID=""

# Find iPhone Duo
DEVICE_UDID=$(pymobiledevice3 usbmux list 2>/dev/null | python3 -c "
import json,sys
devs=json.load(sys.stdin)
for d in devs:
    if 'Duo' in d.get('DeviceName','') or d.get('ProductType','').startswith('iPhone18') or d.get('ProductType','').startswith('iPhone19'):
        print(d['Identifier']); break
" 2>/dev/null || true)

build_and_deploy() {
    local NAME=$1
    local SRC=$2
    local BUNDLE_ID=$3
    local EXTRA_FLAGS="${4:-}"

    echo ""
    echo "========================================"
    echo "Building $NAME"
    echo "========================================"

    # Create .app bundle
    local APP_DIR="$DEST/${NAME}.app"
    mkdir -p "$APP_DIR"

    # Info.plist
    cat > "$APP_DIR/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>${NAME}</string>
    <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
    <key>CFBundleName</key><string>${NAME}</string>
    <key>CFBundleVersion</key><string>1.0</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>MinimumOSVersion</key><string>17.0</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleSupportedPlatforms</key>
    <array><string>iPhoneOS</string></array>
    <key>LSRequiresIPhoneOS</key><true/>
</dict>
</plist>
PLIST

    # Compile
    xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
        -framework IOKit -framework IOSurface -framework CoreFoundation \
        $EXTRA_FLAGS \
        -o "$APP_DIR/$NAME" "$SRC" \
        -Wno-deprecated-declarations -O1 2>&1
    echo "[+] Compiled $NAME"

    # Entitlements
    local ENT_FILE="$DEST/${NAME}.entitlements"
    cat > "$ENT_FILE" << ENT
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>application-identifier</key><string>${TEAM}.${BUNDLE_ID}</string>
    <key>com.apple.developer.team-identifier</key><string>${TEAM}</string>
    <key>get-task-allow</key><true/>
</dict>
</plist>
ENT

    # Sign
    codesign -f -s "$SIGN_ID" --entitlements "$ENT_FILE" "$APP_DIR" 2>&1
    echo "[+] Signed $NAME"

    # Deploy
    if [ -n "$DEVICE_UDID" ]; then
        echo "[*] Deploying $NAME to iPhone Duo ($DEVICE_UDID)..."
        pymobiledevice3 apps install --udid "$DEVICE_UDID" "$APP_DIR" 2>&1
        echo "[+] $NAME deployed"
    else
        echo "[!] No iPhone Duo found, skipping deploy"
        echo "    Build at: $APP_DIR"
    fi
}

echo "=== IOSurface PoC Build & Deploy ==="
echo "Target: iPhone Duo (iOS 27.1)"
[ -n "$DEVICE_UDID" ] && echo "Device: $DEVICE_UDID" || echo "Device: NOT CONNECTED"

# Build selector scanner
build_and_deploy "IOSurfScan" \
    "$SRC_DIR/iosurface_selector_scan.c" \
    "com.research.iosurfscan"

# Build property race PoC
build_and_deploy "IOSurfRace" \
    "$SRC_DIR/iosurface_property_race.c" \
    "com.research.iosurfrace" \
    "-lpthread"

echo ""
echo "=== All builds complete ==="
echo "Apps: IOSurfScan.app, IOSurfRace.app"
