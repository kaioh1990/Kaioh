#!/bin/bash
# Turns a Jesuszilla/ikemen-droid checkout (dbc5cfb) into the "CvF Native"
# Android app: the CvF Online app (android/customize.sh: gestures, game-folder
# lookup, kept settings, signing) with the native C engine instead of the Go
# one, installable next to it. See engine/docs/ANDROID_PORT.md.
#
# Usage: android/native/customize-native.sh <ikemen-droid checkout> <jniLibs dir> <assets dir>
#   jniLibs dir  arm64-v8a/libmain.so + libSDL2.so (engine/tools/package/android_build.sh)
#   assets dir   external/script, external/gamecontrollerdb.txt, save/config.ini
#                (engine/tools/package/android_assets.sh)
# Optional env: ANDROID_VERSION_NAME (default 2.0-native), ANDROID_VERSION_CODE (integer).
set -euo pipefail
W="$1"
LIBS="$2"
ASSETS="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$W/app"
GRADLE="$APP/build.gradle"
JAVA="$APP/src/main/java/org/libsdl/app"

# 1. The CvF Online app changes, own signing key included (the same key as
#    the Go app: android/cvf-online.keystore).
ANDROID_VERSION_NAME="${ANDROID_VERSION_NAME:-2.0-native}" "$HERE/../customize.sh" "$W"

# 2. Own identity, so it installs next to the Go app (com.kaioh1990.cvfonline).
sed -i -e 's/applicationId "com.kaioh1990.cvfonline"/applicationId "com.kaioh1990.cvfnative"/' "$GRADLE"
grep -q 'applicationId "com.kaioh1990.cvfnative"' "$GRADLE"
sed -i -e 's#<string name="app_name" translatable="false">CvF Online</string>#<string name="app_name" translatable="false">CvF Native</string>#' \
  "$APP/src/main/res/values/strings.xml"
grep -q '>CvF Native<' "$APP/src/main/res/values/strings.xml"

# 3. Its own game folder: Internal storage > CvF Native > Game, then
#    Android/data/com.kaioh1990.cvfnative/files/Game (same order as the Go
#    app). The engine extracts its own Lua scripts into the game folder, which
#    differ from the Go app's, so the two apps must not share one folder.
python3 - "$JAVA/SDLActivity.java" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
n = s.count('"CvF Online/Game"')
assert n == 1, n
s = s.replace('"CvF Online/Game"', '"CvF Native/Game"')
# user-facing texts and log lines of the CvF patch
s = s.replace('CvF Online', 'CvF Native')
# The Go runtime's environment is not needed by the native engine.
s = s.replace('android.system.Os.setenv("GODEBUG", "asyncpreemptoff=1", true);',
              '// (native engine: no Go runtime settings)')
open(p, 'w').write(s)
EOF
grep -q '"CvF Native/Game"' "$JAVA/SDLActivity.java"
! grep -q 'CvF Online' "$JAVA/SDLActivity.java"

# 4. HTTP for the engine (lobby, public IP, UPnP): HttpURLConnection via JNI.
cp "$HERE/CvfHttp.java" "$JAVA/CvfHttp.java"
# plain HTTP is needed for UPnP (the router) and local lobby tests, as the
# Go engine's own HTTP client allows
python3 - "$APP/src/main/AndroidManifest.xml" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'android:hardwareAccelerated="true" >'
assert s.count(old) == 1
s = s.replace(old, 'android:hardwareAccelerated="true"\n        android:usesCleartextTraffic="true" >')
open(p, 'w').write(s)
EOF
grep -q 'usesCleartextTraffic="true"' "$APP/src/main/AndroidManifest.xml"

# 5. Engine libraries and assets.
mkdir -p "$APP/src/main/jniLibs/arm64-v8a"
rm -f "$APP/src/main/jniLibs/arm64-v8a/"*.so
cp "$LIBS/arm64-v8a/libmain.so" "$LIBS/arm64-v8a/libSDL2.so" "$APP/src/main/jniLibs/arm64-v8a/"
A="$APP/src/main/assets"
find "$A" -mindepth 1 -maxdepth 1 ! -name manifest.txt -exec rm -rf {} +
cp -R "$ASSETS/external" "$ASSETS/save" "$A/"
test -f "$A/external/script/main.lua"
test -f "$A/external/gamecontrollerdb.txt"
grep -q '^RenderMode *= OpenGL ES 3.2' "$A/save/config.ini"

echo "ikemen-droid customized for CvF Native"
