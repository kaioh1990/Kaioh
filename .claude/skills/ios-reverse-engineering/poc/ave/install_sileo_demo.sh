#!/bin/bash
# install_sileo_demo.sh — Install Sileo Demo (repo browser) on device
# This uses a pre-built IPA from the official Sileo Demo releases.
#
# Option A: Build from source (requires fixing iOS 27 SDK compatibility issues)
# Option B: Download pre-built IPA from getsileo.app or sideload sources
# Option C: Use pymobiledevice3 to install a resigned .app bundle
#
# Usage: ./install_sileo_demo.sh [udid]

set -e

TEAM="9QP5427NJJ"
SIGN_ID="Apple Development: Travis Tameirao (7TJQC38PMP)"
DEST="$HOME/a20_research"
mkdir -p "$DEST"

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"
if [ -z "$UDID" ]; then echo "ERROR: No USB device found"; exit 1; fi

echo "Target UDID: $UDID"
echo ""

# Check for existing Sileo IPA
IPA_PATH="$DEST/Sileo_Demo.ipa"
if [ ! -f "$IPA_PATH" ]; then
    echo "No Sileo IPA found at $IPA_PATH"
    echo ""
    echo "To get the IPA, choose one of:"
    echo "  1. Download from https://getsileo.app (official)"
    echo "  2. Build from source: cd ~/a20_research/Sileo && open Sileo.xcodeproj"
    echo "     - Select 'Sileo Demo' scheme"
    echo "     - Fix: Alderis tab->colorTab rename (iOS 27 UITab conflict)"
    echo "     - Fix: dpkgversion.h add #include <sys/types.h>"
    echo "     - Fix: Linker needs Deps/ static libraries rebuilt"
    echo "     - Archive for iOS device, export IPA"
    echo "  3. Use Semsemq/Sileo fork (community maintained)"
    echo ""
    echo "Place the IPA at: $IPA_PATH"
    echo "Then re-run this script."
    exit 1
fi

echo "Found IPA: $IPA_PATH"
echo ""

# Extract, resign, and install
WORK="$DEST/sileo_resign"
rm -rf "$WORK"
mkdir -p "$WORK"

echo "=== Extracting IPA ==="
unzip -q "$IPA_PATH" -d "$WORK"

# Find the .app bundle
APP_PATH=$(find "$WORK/Payload" -name "*.app" -maxdepth 1 -type d | head -1)
if [ -z "$APP_PATH" ]; then
    echo "ERROR: No .app found in IPA"
    exit 1
fi
APP_NAME=$(basename "$APP_PATH")
echo "App: $APP_NAME"

# Create entitlements
BUNDLE_ID="com.research.sileodemo"
cat > "$WORK/entitlements.plist" << ENT
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

# Update bundle ID
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID" "$APP_PATH/Info.plist" 2>/dev/null || true

echo "=== Resigning with dev cert ==="
# Remove existing code signature
rm -rf "$APP_PATH/_CodeSignature"
# Remove embedded provisioning (we'll use automatic)
rm -f "$APP_PATH/embedded.mobileprovision"

# Sign frameworks first if any
if [ -d "$APP_PATH/Frameworks" ]; then
    for fw in "$APP_PATH/Frameworks"/*.framework; do
        [ -d "$fw" ] && codesign -f -s "$SIGN_ID" "$fw" 2>&1
    done
    for dylib in "$APP_PATH/Frameworks"/*.dylib; do
        [ -f "$dylib" ] && codesign -f -s "$SIGN_ID" "$dylib" 2>&1
    done
fi

# Sign the app
codesign -f -s "$SIGN_ID" --entitlements "$WORK/entitlements.plist" "$APP_PATH" 2>&1

echo "=== Installing ==="
pymobiledevice3 apps install --udid "$UDID" "$APP_PATH" 2>&1

echo ""
echo "=== Done ==="
echo "Sileo Demo installed as $BUNDLE_ID"
echo "Note: Developer-signed apps expire after 7 days (free) or 1 year (paid dev account)."
echo "Sileo Demo is a REPO BROWSER only — no package installation without jailbreak."

# Cleanup
rm -rf "$WORK"
