#!/bin/bash
# Turns a Jesuszilla/ikemen-droid checkout into the "CvF Online" Android app.
# Usage: android/customize.sh <ikemen-droid checkout>
# Optional env: ANDROID_VERSION_NAME (e.g. 0.8.5), ANDROID_VERSION_CODE (integer, must grow).
set -euo pipefail
W="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$W/app"
GRADLE="$APP/build.gradle"

# 0. App changes (cvf-online.patch):
#    - finds the game by itself: Internal storage/CvF Online/Game, or Android/data/<app>/files/Game
#    - never overwrites save/ files such as config.ini, and recreates a deleted config.ini
#    - menu gestures instead of the on-screen gamepad, same rules as iOS
#    - passes the screen size so menus fit the screen's shape
(cd "$W" && patch -p1 < "$HERE/cvf-online.patch")

# 1. Only ship the (patched) engine scripts + controller DB + default settings.
#    The stock manifest would write Ikemen's own fight.def/common.snd/... over
#    Castlevania Fighter's files in the game folder.
printf 'external/script\nexternal/gamecontrollerdb.txt\nsave/config.ini\n' > "$APP/src/main/assets/manifest.txt"

# 2. Own application id and name. The Java namespace (and R class) stays as it is.
sed -i -e 's/applicationId "org.ikemen_engine.ikemen_go"/applicationId "com.kaioh1990.cvfonline"/' "$GRADLE"
grep -q 'applicationId "com.kaioh1990.cvfonline"' "$GRADLE"
sed -i -e 's#<string name="app_name" translatable="false">[^<]*</string>#<string name="app_name" translatable="false">CvF Online</string>#' \
  "$APP/src/main/res/values/strings.xml"
grep -q '>CvF Online<' "$APP/src/main/res/values/strings.xml"

if [ -n "${ANDROID_VERSION_CODE:-}" ]; then
  sed -i -e "s/versionCode 1$/versionCode ${ANDROID_VERSION_CODE}/" "$GRADLE"
fi
if [ -n "${ANDROID_VERSION_NAME:-}" ]; then
  sed -i -e "s/versionName \"1.0\"/versionName \"${ANDROID_VERSION_NAME}\"/" "$GRADLE"
fi

# 3. Sign every build with the same key, so a new APK installs over the old one
#    (Android refuses an update signed with a different key, and uninstalling first
#    would delete a game kept in Android/data).
cp "$HERE/cvf-online.keystore" "$APP/cvf-online.keystore"
python3 - "$GRADLE" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
sign = '''    compileSdkVersion 34
    signingConfigs {
        cvf {
            storeFile file("cvf-online.keystore")
            storePassword "cvfonline"
            keyAlias "cvfonline"
            keyPassword "cvfonline"
        }
    }'''
assert s.count('    compileSdkVersion 34\n') == 1
s = s.replace('    compileSdkVersion 34', sign, 1)
assert s.count('    buildTypes {\n') == 1
s = s.replace('    buildTypes {\n', '''    buildTypes {
        debug {
            signingConfig signingConfigs.cvf
        }
''', 1)
open(p, 'w').write(s)
EOF
grep -q 'signingConfig signingConfigs.cvf' "$GRADLE"

echo "ikemen-droid customized for CvF Online"
