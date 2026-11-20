# SPTM Bypass PoCs — iOS 27.0.1 / A17 Pro
# Educational jailbreak research only. For use on devices you own.
#
# Requires Xcode with iOS SDK for arm64e compilation.
# Build on Mac: make ios

CC = clang
CFLAGS = -arch arm64e -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework Metal
LDFLAGS = -lpthread

TARGETS = sptm_tlb_race ptdesc_overflow trust_cache_inject

all: $(TARGETS)

sptm_tlb_race: sptm_tlb_race.c
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(LDFLAGS) -o $@ $<

ptdesc_overflow: ptdesc_overflow.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

trust_cache_inject: trust_cache_inject.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(FRAMEWORKS) -o $@ $<

# Cross-compile from Mac for iPad (requires iOS SDK)
ios: CC = xcrun -sdk iphoneos clang
ios: CFLAGS = -arch arm64e -Wall -Wextra -O2 -mios-version-min=16.0
ios: $(TARGETS)

clean:
	rm -f $(TARGETS)

.PHONY: all ios clean
