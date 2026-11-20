#!/bin/bash
# Extract RTKit coprocessor firmware from iPad Mini IPSW
# Educational jailbreak research only. For use on devices you own.
#
# Usage: ./extract_firmware.sh [ipsw_path]
#   If no IPSW provided, downloads iPad16,1 iOS 27.0.1 from Apple CDN.
#
# Requires: unzip, python3
# Optional: img4lib or pyimg4 (for IM4P decryption if needed)

set -e

DEVICE="iPad16,1"
BUILD="24A446"
WORK_DIR="$(pwd)/firmware_audit"
FW_DIR="$WORK_DIR/firmware"

echo "=== RTKit Firmware Extraction for $DEVICE ($BUILD) ==="
echo ""

mkdir -p "$WORK_DIR" "$FW_DIR"

# Step 1: Get IPSW
IPSW="$1"
if [ -z "$IPSW" ]; then
    echo "[*] No IPSW provided. Looking for download URL..."
    echo "[*] Check https://ipsw.me/download/$DEVICE/$BUILD"
    echo "[*] Or download from Apple CDN directly."
    echo ""
    echo "[*] Attempting to find IPSW URL via ipsw.me API..."

    # Try ipsw.me JSON API
    IPSW_URL=$(curl -sL "https://api.ipsw.me/v4/ipsw/$DEVICE/$BUILD" 2>/dev/null | \
        python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('url',''))" 2>/dev/null || true)

    if [ -n "$IPSW_URL" ]; then
        echo "[+] Found: $IPSW_URL"
        IPSW="$WORK_DIR/${DEVICE}_${BUILD}.ipsw"
        if [ ! -f "$IPSW" ]; then
            echo "[*] Downloading IPSW (~6GB, this will take a while)..."
            curl -L -o "$IPSW" "$IPSW_URL"
        else
            echo "[*] IPSW already downloaded: $IPSW"
        fi
    else
        echo "[-] Could not find IPSW URL automatically."
        echo "[-] Please download manually and provide path as argument."
        echo "    Download from: https://ipsw.me/download/$DEVICE/$BUILD"
        exit 1
    fi
fi

if [ ! -f "$IPSW" ]; then
    echo "[-] IPSW not found: $IPSW"
    exit 1
fi

echo "[+] Using IPSW: $IPSW"
echo ""

# Step 2: List firmware files in IPSW
echo "[*] Listing firmware files in IPSW..."
FIRMWARE_FILES=$(unzip -l "$IPSW" 2>/dev/null | \
    grep -iE '(firmware/|smartio|dcp|aop|ane|isp|agx|pmp|sio|rtkit|multitouch)' | \
    awk '{print $NF}' | sort)

echo "[*] Found firmware candidates:"
echo "$FIRMWARE_FILES" | head -50
echo ""

# Step 3: Extract firmware files
echo "[*] Extracting firmware files..."
for f in $FIRMWARE_FILES; do
    echo "  Extracting: $f"
    unzip -o -d "$FW_DIR" "$IPSW" "$f" 2>/dev/null || true
done

# Also extract any .im4p files
echo "[*] Extracting all .im4p files..."
unzip -o -d "$FW_DIR" "$IPSW" '*.im4p' 2>/dev/null || true
unzip -o -d "$FW_DIR" "$IPSW" '*.IM4P' 2>/dev/null || true

echo ""

# Step 4: Count extracted files
EXTRACTED=$(find "$FW_DIR" -type f | wc -l)
echo "[+] Extracted $EXTRACTED files to $FW_DIR"
echo ""

# Step 5: Quick binary grep for PAC instructions
echo "[*] Quick PAC instruction scan (binary grep)..."
echo ""

# PACGA opcode bytes (little-endian): 0x30 0xC0 0x?? 0x9A (varies by registers)
# PACIASP hint: 0x3F 0x23 0x03 0xD5
# RETAA: 0xFF 0x0B 0x5F 0xD6

find "$FW_DIR" -type f -size +1k | while read -r fw_file; do
    fname=$(basename "$fw_file")
    fsize=$(stat -f%z "$fw_file" 2>/dev/null || stat -c%s "$fw_file" 2>/dev/null)

    # Search for PACIASP (0xD503233F) and PACGA patterns
    pac_count=$(python3 -c "
import struct
with open('$fw_file', 'rb') as f:
    data = f.read()
count = 0
for i in range(0, len(data) - 3, 4):
    insn = struct.unpack_from('<I', data, i)[0]
    # PACIASP
    if insn == 0xD503233F: count += 1
    # PACIBSP
    if insn == 0xD503237F: count += 1
    # PACGA (mask)
    if (insn & 0xFFE0FC00) == 0x9AC03000: count += 1
    # RETAA
    if insn == 0xD65F0BFF: count += 1
    # RETAB
    if insn == 0xD65F0FFF: count += 1
print(count)
" 2>/dev/null || echo "?")

    if [ "$pac_count" = "0" ]; then
        echo "  [!!!] $fname ($fsize bytes) — ZERO PAC instructions found"
    elif [ "$pac_count" = "?" ]; then
        echo "  [???] $fname ($fsize bytes) — could not scan"
    else
        echo "  [OK]  $fname ($fsize bytes) — $pac_count PAC instructions"
    fi
done

echo ""
echo "=== Scan Complete ==="
echo ""
echo "[*] For detailed audit, run:"
echo "    python3 audit_rtkit_pac.py $FW_DIR"
echo ""
echo "[*] Files with ZERO PAC instructions are SPTM bypass candidates."
echo "    These coprocessors can be targeted with the Titan technique."
