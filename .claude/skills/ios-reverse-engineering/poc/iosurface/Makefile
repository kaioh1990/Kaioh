# IOSurface Race Condition PoC - CVE-2026-112477
# Educational jailbreak research only. For use on devices you own.
#
# Requires Xcode with iOS SDK for arm64e compilation.
# Build on Mac: make
# Run on iPad: deploy via Xcode or sideload

CC = clang
CFLAGS = -arch arm64e -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework IOSurface -framework CoreFoundation
LDFLAGS = -lpthread

TARGETS = iosurface_race libbridge.dylib

all: $(TARGETS)

iosurface_race: iosurface_race.c
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(LDFLAGS) -o $@ $<

libbridge.dylib: webkit_to_iosurface.c
	$(CC) $(CFLAGS) $(FRAMEWORKS) -shared -o $@ $<

# Cross-compile from Mac for iPad (requires iOS SDK)
ios: CC = xcrun -sdk iphoneos clang
ios: CFLAGS = -arch arm64e -Wall -Wextra -O2 -mios-version-min=16.0
ios: $(TARGETS)

clean:
	rm -f $(TARGETS)

.PHONY: all ios clean
