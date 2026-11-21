#!/usr/bin/env python3
"""Drop branch relocations (R_ARM_CALL/JUMP24, THM_CALL/JUMP24) whose target is
in the same segment as the branch: they are PC-relative and need no fixing, and
the Vita loader rewrites conditional BL (R_ARM_JUMP24) into a plain B, which
breaks every 'blcc' call. Usage: velf_fixrel.py in.velf out.velf"""
import struct, sys
f = bytearray(open(sys.argv[1], 'rb').read())
phoff = struct.unpack_from('<I', f, 28)[0]; phnum = struct.unpack_from('<H', f, 44)[0]
for k in range(phnum):
    p = list(struct.unpack_from('<8I', f, phoff + 32 * k))
    if p[0] != 0x60000000: continue
    i, end = p[1], p[1] + p[4]; out = bytearray(); dropped = kept = 0
    while i < end:
        w0 = struct.unpack_from('<I', f, i)[0]
        sz = 8 if w0 & 0xF else 12
        code = (w0 >> 8) & 0xFF; symseg = (w0 >> 4) & 0xF; datseg = (w0 >> 16) & 0xF
        if code in (28, 29, 10, 30) and symseg == datseg:
            dropped += 1
        else:
            out += f[i:i + sz]; kept += 1
        i += sz
    f[p[1]:p[1] + len(out)] = out
    p[4] = len(out)
    struct.pack_into('<8I', f, phoff + 32 * k, *p)
    print('velf_fixrel: kept %d relocations, dropped %d same-segment branch relocations' % (kept, dropped))
open(sys.argv[2], 'wb').write(f)
