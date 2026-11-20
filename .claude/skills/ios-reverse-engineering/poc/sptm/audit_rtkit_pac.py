#!/usr/bin/env python3
"""Audit RTKit coprocessor firmware for PAC instructions.

Scans ARM64 firmware binaries for Pointer Authentication Code
instructions. Any coprocessor firmware WITHOUT PAC is a candidate
for the Titan-style SPTM bypass (page-table write via coprocessor DMA).

The Titan technique:
1. Corrupt coprocessor's saved thread state (TTBR1_EL1)
2. RTKit scheduler restores corrupted state → coprocessor runs
   with attacker-controlled page tables
3. Coprocessor DMA writes into XNU page-table page
4. Self-referencing PTE → full physical memory R/W → SPTM defeat

If coprocessor firmware has PAC, the saved thread state is
authenticated and can't be forged. If it LACKS PAC, game over.

Usage:
    python3 audit_rtkit_pac.py <firmware_dir>
    python3 audit_rtkit_pac.py firmware.im4p
    python3 audit_rtkit_pac.py --ipsw iPad16,1_27.0.1.ipsw

Educational jailbreak research only. For use on devices you own.
"""

import os
import struct
import sys
import glob
import zipfile
import tempfile

# ARM64 PAC instruction encodings (key opcodes to search for)
# Reference: Arm Architecture Reference Manual, Chapter C6
PAC_INSTRUCTIONS = {
    # PACIA variants (instruction address, A-key)
    'PACIA':   (0xDAC10000, 0xFFFFFC00),  # PACIA Xd, Xn
    'PACIZA':  (0xDAC123E0, 0xFFFFFFE0),  # PACIZA Xd
    'PACIA1716': (0xD503211F, 0xFFFFFFFF), # PACIA1716 (hint)
    'PACIASP': (0xD503233F, 0xFFFFFFFF),   # PACIASP (hint)
    'PACIAZ':  (0xD503231F, 0xFFFFFFFF),   # PACIAZ (hint)

    # PACIB variants (instruction address, B-key)
    'PACIB':   (0xDAC10400, 0xFFFFFC00),
    'PACIZB':  (0xDAC127E0, 0xFFFFFFE0),
    'PACIB1716': (0xD503215F, 0xFFFFFFFF),
    'PACIBSP': (0xD503237F, 0xFFFFFFFF),
    'PACIBZ':  (0xD503235F, 0xFFFFFFFF),

    # PACDA variants (data address, A-key)
    'PACDA':   (0xDAC10800, 0xFFFFFC00),
    'PACDZA':  (0xDAC12BE0, 0xFFFFFFE0),

    # PACDB variants (data address, B-key)
    'PACDB':   (0xDAC10C00, 0xFFFFFC00),
    'PACDZB':  (0xDAC12FE0, 0xFFFFFFE0),

    # PACGA (generic authentication)
    'PACGA':   (0x9AC03000, 0xFFE0FC00),

    # AUT (authenticate) variants
    'AUTIA':   (0xDAC11000, 0xFFFFFC00),
    'AUTIZA':  (0xDAC133E0, 0xFFFFFFE0),
    'AUTIA1716': (0xD503219F, 0xFFFFFFFF),
    'AUTIASP': (0xD50323BF, 0xFFFFFFFF),
    'AUTIAZ':  (0xD503239F, 0xFFFFFFFF),
    'AUTIB':   (0xDAC11400, 0xFFFFFC00),
    'AUTIZB':  (0xDAC137E0, 0xFFFFFFE0),
    'AUTIB1716': (0xD50321DF, 0xFFFFFFFF),
    'AUTIBSP': (0xD50323FF, 0xFFFFFFFF),
    'AUTIBZ':  (0xD50323DF, 0xFFFFFFFF),
    'AUTDA':   (0xDAC11800, 0xFFFFFC00),
    'AUTDZA':  (0xDAC13BE0, 0xFFFFFFE0),
    'AUTDB':   (0xDAC11C00, 0xFFFFFC00),
    'AUTDZB':  (0xDAC13FE0, 0xFFFFFFE0),

    # XPAC (strip PAC) variants
    'XPACI':   (0xDAC143E0, 0xFFFFFFE0),
    'XPACD':   (0xDAC147E0, 0xFFFFFFE0),

    # RETAA/RETAB (authenticated return)
    'RETAA':   (0xD65F0BFF, 0xFFFFFFFF),
    'RETAB':   (0xD65F0FFF, 0xFFFFFFFF),

    # BRAA/BRAB (authenticated branch)
    'BRAA':    (0xD71F0800, 0xFFFFFC00),
    'BRAB':    (0xD71F0C00, 0xFFFFFC00),
    'BRAAZ':   (0xD61F081F, 0xFFFFFFFF),
    'BRABZ':   (0xD61F0C1F, 0xFFFFFFFF),

    # BLRAA/BLRAB (authenticated branch with link)
    'BLRAA':   (0xD73F0800, 0xFFFFFC00),
    'BLRAB':   (0xD73F0C00, 0xFFFFFC00),
    'BLRAAZ':  (0xD63F081F, 0xFFFFFFFF),
    'BLRABZ':  (0xD63F0C1F, 0xFFFFFFFF),

    # ERETAA/ERETAB (authenticated exception return)
    'ERETAA':  (0xD69F0BFF, 0xFFFFFFFF),
    'ERETAB':  (0xD69F0FFF, 0xFFFFFFFF),
}

# Known RTKit coprocessor firmware file patterns in IPSW
RTKIT_FIRMWARE_PATTERNS = [
    '**/SmartIO*',
    '**/smartio*',
    '**/dcp*',
    '**/DCP*',
    '**/aop*',
    '**/AOP*',
    '**/ane*',
    '**/ANE*',
    '**/isp*',
    '**/ISP*',
    '**/agx*',
    '**/AGX*',
    '**/pmp*',
    '**/PMP*',
    '**/multitouch*',
    '**/Multitouch*',
    '**/sio*',
    '**/SIO*',
    '**/*.im4p',
    '**/Firmware/all_flash/*',
    '**/Firmware/dfu/*',
]


def scan_firmware(data, name='unknown'):
    """Scan firmware binary for PAC instructions."""
    results = {}
    total_instructions = len(data) // 4

    for pac_name, (opcode, mask) in PAC_INSTRUCTIONS.items():
        count = 0
        offsets = []
        for i in range(0, len(data) - 3, 4):
            insn = struct.unpack_from('<I', data, i)[0]
            if (insn & mask) == opcode:
                count += 1
                if len(offsets) < 5:  # Keep first 5 offsets
                    offsets.append(i)
        if count > 0:
            results[pac_name] = {'count': count, 'offsets': offsets}

    total_pac = sum(r['count'] for r in results.values())

    return {
        'name': name,
        'size': len(data),
        'instructions': total_instructions,
        'pac_total': total_pac,
        'pac_detail': results,
        'has_pac': total_pac > 0,
    }


def strip_im4p_header(data):
    """Strip IM4P container header to get raw firmware payload.

    IM4P format: DER-encoded ASN.1 sequence containing:
    - IA5String "IM4P"
    - IA5String <type> (e.g., "rtki", "dcp", "aop")
    - IA5String <description>
    - OctetString <payload>  ← this is the firmware binary
    """
    # Quick check for IM4P magic
    if b'IM4P' not in data[:32]:
        return data

    # Simple approach: find the largest OCTET STRING in the DER encoding
    # The payload is the last and largest element
    best_offset = 0
    best_length = 0

    i = 0
    while i < len(data) - 4:
        # Look for OCTET STRING tag (0x04) with long-form length
        if data[i] == 0x04:
            if data[i+1] & 0x80:
                num_len_bytes = data[i+1] & 0x7F
                if num_len_bytes <= 4 and i + 2 + num_len_bytes < len(data):
                    length = int.from_bytes(data[i+2:i+2+num_len_bytes], 'big')
                    offset = i + 2 + num_len_bytes
                    if length > best_length and offset + length <= len(data):
                        best_offset = offset
                        best_length = length
            elif data[i+1] > 0:
                length = data[i+1]
                offset = i + 2
                if length > best_length and offset + length <= len(data):
                    best_offset = offset
                    best_length = length
        i += 1

    if best_length > 1024:
        return data[best_offset:best_offset + best_length]

    return data


def identify_firmware_type(name, data):
    """Try to identify what coprocessor this firmware belongs to."""
    name_lower = name.lower()
    if 'smartio' in name_lower or 'sio' in name_lower:
        return 'SmartIO'
    if 'dcp' in name_lower:
        return 'DCP (Display Coprocessor)'
    if 'aop' in name_lower:
        return 'AOP (Always-On Processor)'
    if 'ane' in name_lower:
        return 'ANE (Neural Engine)'
    if 'isp' in name_lower:
        return 'ISP (Image Signal Processor)'
    if 'agx' in name_lower or 'gpu' in name_lower:
        return 'AGX (GPU)'
    if 'pmp' in name_lower:
        return 'PMP (Power Management)'
    if 'multitouch' in name_lower:
        return 'Multitouch'

    # Check for RTKit magic in binary
    if b'RTKit' in data[:4096] or b'rtkit' in data[:4096]:
        return 'RTKit (unknown subsystem)'

    return 'Unknown'


def audit_file(filepath):
    """Audit a single firmware file."""
    with open(filepath, 'rb') as f:
        data = f.read()

    name = os.path.basename(filepath)
    fw_type = identify_firmware_type(name, data)

    # Strip IM4P container if present
    payload = strip_im4p_header(data)

    result = scan_firmware(payload, name)
    result['path'] = filepath
    result['type'] = fw_type
    result['raw_size'] = len(data)
    result['payload_size'] = len(payload)

    return result


def audit_directory(dirpath):
    """Audit all firmware files in a directory."""
    results = []

    for pattern in ['*.im4p', '*.bin', '*.fw', '*.rtkit', '*']:
        for filepath in glob.glob(os.path.join(dirpath, '**', pattern), recursive=True):
            if os.path.isfile(filepath) and os.path.getsize(filepath) > 1024:
                try:
                    result = audit_file(filepath)
                    results.append(result)
                except Exception as e:
                    print(f"  [-] Error reading {filepath}: {e}")

    return results


def audit_ipsw(ipsw_path):
    """Extract and audit firmware files from an IPSW (ZIP archive)."""
    results = []

    with zipfile.ZipFile(ipsw_path, 'r') as z:
        firmware_files = []
        for info in z.infolist():
            name = info.filename.lower()
            if any(kw in name for kw in
                   ['firmware/', 'smartio', 'dcp', 'aop', 'ane', 'isp',
                    'agx', 'pmp', 'rtkit', 'sio', 'multitouch']):
                if info.file_size > 1024:
                    firmware_files.append(info)

        print(f"[*] Found {len(firmware_files)} candidate firmware files in IPSW")

        with tempfile.TemporaryDirectory() as tmpdir:
            for info in firmware_files:
                try:
                    z.extract(info, tmpdir)
                    filepath = os.path.join(tmpdir, info.filename)
                    result = audit_file(filepath)
                    results.append(result)
                except Exception as e:
                    print(f"  [-] Error extracting {info.filename}: {e}")

    return results


def print_report(results):
    """Print the PAC audit report."""
    if not results:
        print("[-] No firmware files found to audit")
        return

    # Sort: PAC-free first (most interesting), then by name
    results.sort(key=lambda r: (r['has_pac'], r['name']))

    print(f"\n{'='*70}")
    print(f" RTKit Coprocessor PAC Audit Report")
    print(f"{'='*70}\n")

    vulnerable = []
    hardened = []

    for r in results:
        status = "HARDENED (PAC present)" if r['has_pac'] else "*** NO PAC — VULNERABLE ***"
        icon = "X" if r['has_pac'] else "!!!"

        print(f"[{icon}] {r['name']}")
        print(f"     Type: {r['type']}")
        print(f"     Size: {r['payload_size']:,} bytes ({r['instructions']:,} instructions)")
        print(f"     PAC instructions: {r['pac_total']}")
        print(f"     Status: {status}")

        if r['has_pac']:
            hardened.append(r)
            detail = r['pac_detail']
            top = sorted(detail.items(), key=lambda x: -x[1]['count'])[:5]
            for name, info in top:
                print(f"       {name}: {info['count']} instances")
        else:
            vulnerable.append(r)

        print()

    print(f"{'='*70}")
    print(f" Summary")
    print(f"{'='*70}")
    print(f"  Total firmware images audited: {len(results)}")
    print(f"  PAC-hardened: {len(hardened)}")
    print(f"  PAC-free (VULNERABLE): {len(vulnerable)}")

    if vulnerable:
        print(f"\n  *** SPTM BYPASS CANDIDATES ***")
        for v in vulnerable:
            print(f"  → {v['name']} ({v['type']}) — {v['payload_size']:,} bytes, 0 PAC instructions")
        print(f"\n  These coprocessors can be used for Titan-style page-table writes.")
        print(f"  Next: build RTKit ROP chain targeting the vulnerable firmware.")
    else:
        print(f"\n  All coprocessor firmware has PAC hardening.")
        print(f"  Titan-style bypass not directly applicable.")
        print(f"  Consider: TXM interface analysis, or wait for public bypass.")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <firmware_dir|firmware.im4p|--ipsw file.ipsw>")
        print(f"\nAudit RTKit coprocessor firmware for PAC instructions.")
        print(f"Any firmware WITHOUT PAC is a candidate for Titan-style SPTM bypass.")
        sys.exit(1)

    target = sys.argv[1]

    if target == '--ipsw' and len(sys.argv) > 2:
        print(f"[*] Auditing IPSW: {sys.argv[2]}")
        results = audit_ipsw(sys.argv[2])
    elif os.path.isdir(target):
        print(f"[*] Auditing directory: {target}")
        results = audit_directory(target)
    elif os.path.isfile(target):
        print(f"[*] Auditing file: {target}")
        results = [audit_file(target)]
    else:
        print(f"[-] Not found: {target}")
        sys.exit(1)

    print_report(results)


if __name__ == '__main__':
    main()
