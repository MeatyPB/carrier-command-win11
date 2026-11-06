#!/usr/bin/env python3
"""Linear-sweep listing of a range of the unpacked image.

Usage: dis.py image SEG START END   (hex). Addresses print as SEG:OFF and the
linear address, with call/jump targets as written.
"""
import sys

from capstone import CS_ARCH_X86, CS_MODE_16, Cs

img = open(sys.argv[1], "rb").read()
seg, start, end = (int(x, 16) for x in sys.argv[2:5])
md = Cs(CS_ARCH_X86, CS_MODE_16)
md.skipdata = True
base = seg * 16
for i in md.disasm(img[base + start : base + end], start):
    print(f"{seg:04X}:{i.address:04X}  {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
