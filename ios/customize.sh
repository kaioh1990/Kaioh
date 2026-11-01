#!/bin/bash
# Turns a Jesuszilla/ikemen-ios checkout into the "CvF Online" iOS/iPadOS app.
# Usage: ios/customize.sh <ikemen-ios checkout>
set -euo pipefail
W="$1"
PBX="$W/I.K.E.M.E.N-Go.xcodeproj/project.pbxproj"
PLIST="$W/I.K.E.M.E.N-Go/Info.plist"

# 1. Only ship the (patched) engine scripts + controller DB as "default assets".
#    The stock manifest would unzip Ikemen's own fight.def/common.snd/... over
#    Castlevania Fighter's files in the game folder you pick.
printf 'external/script\nexternal/gamecontrollerdb.txt\n' > "$W/DefaultAssets/manifest.txt"

# 2. Own bundle id + name, no hard-coded signing team (pick yours in Xcode).
sed -i '' \
  -e 's/PRODUCT_BUNDLE_IDENTIFIER = "ikemen-engine.I-K-E-M-E-N-Go";/PRODUCT_BUNDLE_IDENTIFIER = "com.kaioh1990.cvfonline";/' \
  -e 's/DEVELOPMENT_TEAM = 6W73QW64V7;/DEVELOPMENT_TEAM = "";/' \
  -e 's/GENERATE_INFOPLIST_FILE = YES;/GENERATE_INFOPLIST_FILE = YES;\
				INFOPLIST_KEY_CFBundleDisplayName = "CvF Online";/' \
  "$PBX"
grep -q 'com.kaioh1990.cvfonline' "$PBX"
grep -q 'INFOPLIST_KEY_CFBundleDisplayName = "CvF Online"' "$PBX"

# 3. Show the app's folder in Files / Finder so the game folder can be copied
#    straight onto the device, and explain the local-network prompt (UPnP).
plutil -replace UIFileSharingEnabled -bool YES "$PLIST"
plutil -replace LSSupportsOpeningDocumentsInPlace -bool YES "$PLIST"
plutil -replace NSLocalNetworkUsageDescription -string "CvF Online uses your local network to open a port on your router for online matches." "$PLIST"
plutil -lint "$PLIST"
echo "ikemen-ios customized for CvF Online"
