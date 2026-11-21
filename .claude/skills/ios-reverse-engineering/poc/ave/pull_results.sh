#!/bin/bash
# pull_results.sh — Pull test results from all AVE probe apps
#
# Usage: ./pull_results.sh [udid]

RESEARCH_DIR="$HOME/a20_research"
RESULTS_DIR="$RESEARCH_DIR/results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

UDID="${1:-}"
if [ -z "$UDID" ]; then
    UDID=$(pymobiledevice3 usbmux list --usb 2>/dev/null | grep -o '"serial": "[^"]*"' | head -1 | cut -d'"' -f4)
fi

if [ -z "$UDID" ]; then
    echo "[!] No device found"
    exit 1
fi

echo "[*] Device: $UDID"
echo "[*] Results dir: $RESULTS_DIR"
echo ""

BUNDLES=(
    "com.research.avesurgical"
    "com.research.aveexploit"
    "com.research.avepipe"
    "com.research.avekaslr"
    "com.research.avespray"
    "com.research.aveleak"
    "com.research.avegrad"
)

for bundle in "${BUNDLES[@]}"; do
    shortname="${bundle##*.}"
    echo "[*] Pulling from $bundle..."
    mkdir -p "$RESULTS_DIR/$shortname"

    # List and pull all .txt files from the app's Documents
    pymobiledevice3 apps afc --udid "$UDID" --documents "$bundle" << EOF 2>/dev/null || echo "  [!] Could not access $bundle"
ls
EOF

    # Try pulling known output patterns
    for prefix in "ave_surgical" "ave_exploit" "ave_pipe" "ave_kaslr" "ave_spray" "ave_leak" "ave_grad"; do
        for mode in "info" "spray_check" "spray_ntl9" "spray_ntl16" "spray_ntl32" "multi_round" "pattern_aa" "pattern_00" \
                    "safe" "dead_zone" "first_ptr" "key_ptr" "scan" "map_test" \
                    "pipe_spray" "pipe_ntl9" "pipe_ntl16" "pipe_rw_test" \
                    "spray_leak" "bitstream_leak" "multi_spray" \
                    "spray_only" "baseline" "ntl_low" "low"; do
            fname="${prefix}_${mode}.txt"
            printf "pull $fname $RESULTS_DIR/$shortname/$fname\n" | \
                pymobiledevice3 apps afc --udid "$UDID" --documents "$bundle" 2>/dev/null && \
                echo "  [+] Got $fname" || true
        done
    done
    echo ""
done

echo "[*] Results saved to: $RESULTS_DIR"
echo ""
echo "[*] Files found:"
find "$RESULTS_DIR" -name "*.txt" -size +0c | while read f; do
    echo "  $(wc -l < "$f") lines: $f"
done
