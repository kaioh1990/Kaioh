#!/usr/bin/env python3
"""CVE-2026-86950 size class calibration tool.

Calculates coverage buffer sizes for different DX values and maps them
to libmalloc size classes. Helps choose the right DX to target a specific
heap bin for feng shui.

libmalloc size classes (approximate, 64-bit):
  TINY:   16–1008 bytes (quantum 16)
  SMALL:  1024–15,360 bytes (quantum 512)
  LARGE:  15,361+ bytes (page-aligned, mmap)
"""

N = 9           # composite levels
SCALE = 2.0     # approximate scaling per level

print(f"{'DX':>4} | {'width_px':>10} | {'buf_size':>10} | {'heap?':>5} | {'bin':>8} | notes")
print("-" * 72)

for dx in range(1, 65):
    width_px = int(dx * (SCALE ** N))
    buf_size = 4 * (width_px + 16)
    heap = width_px > 1015

    if buf_size <= 1008:
        bin_name = "TINY"
    elif buf_size <= 15360:
        bin_name = "SMALL"
    else:
        bin_name = "LARGE"

    notes = ""
    if dx == 33:
        notes = "← current PoC"
    elif not heap and dx >= 1:
        notes = "(stack OOB)"

    if dx <= 5 or dx in (8, 10, 12, 16, 20, 24, 32, 33, 48, 64) or not heap:
        print(f"{dx:>4} | {width_px:>10,} | {buf_size:>10,} | {'HEAP' if heap else 'STACK':>5} | {bin_name:>8} | {notes}")

print()
print("=== Interesting targets for heap feng shui ===")
print()

targets = [
    (2, "~4KB - could target CFData/NSData in SMALL bin"),
    (3, "~6KB - SMALL bin, common CoreFoundation object size"),
    (4, "~8KB - SMALL bin, page-half aligned"),
    (5, "~10KB - SMALL bin"),
    (8, "~16KB - LARGE bin boundary"),
]

for dx, desc in targets:
    width_px = int(dx * (SCALE ** N))
    buf_size = 4 * (width_px + 16)
    heap = width_px > 1015
    print(f"  DX={dx}: width_px={width_px:,}, buf_size={buf_size:,} bytes {'(HEAP)' if heap else '(STACK)'}")
    print(f"    → {desc}")
    print()

print("=== Recommended: DX=2 (buffer ~4KB, SMALL bin) ===")
print("Rationale:")
print("  - SMALL magazine has predictable region layout")
print("  - 4KB is a common allocation size for CoreFoundation objects")
print("  - CFData, NSMutableData often allocated in this range")
print("  - Sequential same-size allocations are adjacent in SMALL regions")
