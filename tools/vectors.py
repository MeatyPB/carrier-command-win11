#!/usr/bin/env python3
"""Count indirect far calls through DS vectors and dump the vector tables."""
import collections
import re
import struct
import sys

a = sys.argv[1]
c = collections.Counter()
for name in ("seg0000.lst", "seg0FA8.lst"):
    for line in open(f"{a}/{name}"):
        m = re.search(r"lcall \[0x([0-9a-f]+)\]", line)
        if m:
            c[int(m.group(1), 16)] += 1
print("vector use:")
for v, n in sorted(c.items()):
    print(f"  [{v:04X}] x{n}")

img = open(f"{a}/carrier.bin", "rb").read()
ds = 0x19A00
print("\nraw DS:0B80-0E00 as far pointers:")
for o in range(0xB80, 0xE00, 4):
    off, seg = struct.unpack_from("<HH", img, ds + o)
    print(f"  {o:04X}: {seg:04X}:{off:04X}")
