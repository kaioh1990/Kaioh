#!/bin/sh
# Packages the native game as a macOS .app.
#   macos_app.sh BUILD_DIR SDL_LIB_DIR OUT.app [ICON.icns]
# SDL_LIB_DIR holds libSDL2-2.0.0.dylib (SDL2 or sdl2-compat) and, for
# sdl2-compat, libSDL3.0.dylib. The app is placed next to the game data
# (chars/, data/, ...): like Ikemen's bundles, the engine changes to the
# folder containing the .app at startup (os.Executable + app-bundle check).
set -e
BUILD=$1
LIBS=$2
APP=$3
ICON=$4
[ -x "$BUILD/cvf" ] || { echo "no $BUILD/cvf"; exit 1; }
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BUILD/cvf" "$APP/Contents/MacOS/cvf"
for l in libSDL2-2.0.0.dylib libSDL3.0.dylib; do
  if [ -f "$LIBS/$l" ]; then
    cp "$LIBS/$l" "$APP/Contents/Frameworks/$l"
    chmod u+w "$APP/Contents/Frameworks/$l"
    install_name_tool -id "@rpath/$l" "$APP/Contents/Frameworks/$l"
  fi
done
# the executable refers to SDL2 by its build-time install name: point it
# at the bundled copy
old=$(otool -L "$APP/Contents/MacOS/cvf" | awk '/libSDL2/ {print $1; exit}')
[ -n "$old" ] && install_name_tool -change "$old" "@rpath/libSDL2-2.0.0.dylib" "$APP/Contents/MacOS/cvf"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/cvf" 2>/dev/null || true
if [ -f "$APP/Contents/Frameworks/libSDL3.0.dylib" ]; then
  # sdl2-compat loads SDL3 at run time; let it find the bundled one
  old3=$(otool -L "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" | awk '/libSDL3/ {print $1; exit}')
  [ -n "$old3" ] && install_name_tool -change "$old3" "@rpath/libSDL3.0.dylib" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
  install_name_tool -add_rpath "@loader_path" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib" 2>/dev/null || true
  # sdl2-compat dlopens @loader_path/libSDL3.dylib (unversioned name)
  cp "$APP/Contents/Frameworks/libSDL3.0.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
fi
[ -n "$ICON" ] && [ -f "$ICON" ] && cp "$ICON" "$APP/Contents/Resources/icon.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>cvf</string>
    <key>CFBundleName</key><string>CvF Native</string>
    <key>CFBundleDisplayName</key><string>Castlevania Fighter (native)</string>
    <key>CFBundleIdentifier</key><string>net.cvf.native</string>
    <key>CFBundleIconFile</key><string>icon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST
# ad-hoc signature (required to run on Apple silicon)
codesign --force --deep -s - "$APP"
echo "built $APP"
