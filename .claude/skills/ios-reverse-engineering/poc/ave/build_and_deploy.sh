#!/bin/bash
# Build and deploy AVE research tools to iPhone Duo
# Target: iPhone19,4 (A20 Pro), iOS 27.1
# Signing: Apple Development: Travis Tameirao (7TJQC38PMP), Team: 9QP5427NJJ

set -e

UDID="00008160-000A4C840E400036"
TEAM="9QP5427NJJ"
SIGN="Apple Development: Travis Tameirao (7TJQC38PMP)"
SDK="iphoneos"
ARCH="arm64e"
MIN_IOS="17.0"
DEPFLAGS="-Wno-deprecated-declarations"
FRAMEWORKS="-framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation -framework IOSurface"

BUILD_DIR="$HOME/a20_research/builds"
mkdir -p "$BUILD_DIR"

build_app() {
    local SRC="$1"
    local NAME="$2"
    local BUNDLE_ID="$3"

    echo "=== Building $NAME from $SRC ==="

    local BIN="$BUILD_DIR/$NAME"
    local APP="$BUILD_DIR/$NAME.app"

    xcrun -sdk "$SDK" clang -arch "$ARCH" -miphoneos-version-min="$MIN_IOS" \
        $FRAMEWORKS $DEPFLAGS -o "$BIN" "$SRC"

    rm -rf "$APP"
    mkdir -p "$APP"
    cp "$BIN" "$APP/$NAME"

    cat > "$APP/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>$NAME</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleName</key><string>$NAME</string>
    <key>CFBundleVersion</key><string>1.0</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>MinimumOSVersion</key><string>$MIN_IOS</string>
    <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
    <key>UIDeviceFamily</key><array><integer>1</integer></array>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>LSRequiresIPhoneOS</key><true/>
</dict>
</plist>
PLIST

    cat > "$BUILD_DIR/entitlements.plist" << ENT
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>get-task-allow</key><true/>
</dict>
</plist>
ENT

    codesign -f -s "$SIGN" --entitlements "$BUILD_DIR/entitlements.plist" "$APP"
    echo "Signed: $APP"
}

deploy_app() {
    local NAME="$1"
    local APP="$BUILD_DIR/$NAME.app"

    echo "=== Deploying $NAME ==="
    pymobiledevice3 apps install --udid "$UDID" "$APP"
    echo "Installed: $NAME"
}

launch_app() {
    local BUNDLE_ID="$1"
    local ARGS="$2"

    echo "=== Launching $BUNDLE_ID $ARGS ==="
    pymobiledevice3 developer dvt launch --udid "$UDID" "$BUNDLE_ID" $ARGS
}

# Source files
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

case "${1:-all}" in
    controlled)
        build_app "$SCRIPT_DIR/ave_controlled_oob.c" "AVEControlled" "com.research.avecontrolled"
        deploy_app "AVEControlled"
        ;;
    kwrite)
        build_app "$SCRIPT_DIR/ave_kwrite_primitive.c" "AVEKWrite" "com.research.avekwrite"
        deploy_app "AVEKWrite"
        ;;
    infoleak)
        build_app "$SCRIPT_DIR/ave_infoleak_probe.c" "AVELeak" "com.research.aveleak"
        deploy_app "AVELeak"
        ;;
    propscan)
        build_app "$SCRIPT_DIR/ave_property_scan.c" "AVEPropScan" "com.research.avepropscan"
        deploy_app "AVEPropScan"
        ;;
    reuse)
        build_app "$SCRIPT_DIR/ave_session_reuse.c" "AVEReuse" "com.research.avereuse"
        deploy_app "AVEReuse"
        ;;
    all)
        build_app "$SCRIPT_DIR/ave_controlled_oob.c" "AVEControlled" "com.research.avecontrolled"
        build_app "$SCRIPT_DIR/ave_kwrite_primitive.c" "AVEKWrite" "com.research.avekwrite"
        build_app "$SCRIPT_DIR/ave_infoleak_probe.c" "AVELeak" "com.research.aveleak"
        build_app "$SCRIPT_DIR/ave_property_scan.c" "AVEPropScan" "com.research.avepropscan"
        build_app "$SCRIPT_DIR/ave_session_reuse.c" "AVEReuse" "com.research.avereuse"
        deploy_app "AVEControlled"
        deploy_app "AVEKWrite"
        deploy_app "AVELeak"
        deploy_app "AVEPropScan"
        deploy_app "AVEReuse"
        ;;
    launch)
        BUNDLE="${2:-com.research.avecontrolled}"
        ARGS="${3:-info}"
        launch_app "$BUNDLE" "$ARGS"
        ;;
    *)
        echo "Usage: $0 [controlled|kwrite|infoleak|all|launch] [bundle_id] [args]"
        ;;
esac

echo "=== Done ==="
