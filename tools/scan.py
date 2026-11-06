#!/usr/bin/env python3
"""Survey the unpacked image: segments from relocations, INT/port usage in context."""
import collections
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_16, Cs

img = open(sys.argv[1], "rb").read()
rel = open(sys.argv[1] + ".rel", "rb").read()
md = Cs(CS_ARCH_X86, CS_MODE_16)

segs = collections.Counter()
for i in range(0, len(rel), 4):
    off, seg = struct.unpack_from("<HH", rel, i)
    lin = seg * 16 + off
    if lin + 2 <= len(img):
        segs[struct.unpack_from("<H", img, lin)[0]] += 1
print("segment values referenced by relocations:")
for s, n in sorted(segs.items()):
    print(f"  {s:04X} (lin {s * 16:06X}) x{n}")


def dis_before(pos, before=24, after=10):
    """Disassemble around pos, picking the start that syncs to pos."""
    for back in range(before, 0, -1):
        out = list(md.disasm(img[pos - back : pos + 40], pos - back))
        if any(i.address == pos for i in out):
            res, seen = [], 0
            for i in out:
                res.append(i)
                if i.address >= pos:
                    seen += 1
                    if seen > after:
                        break
            return res
    return list(md.disasm(img[pos : pos + 30], pos))[:after]


def show(pos):
    for i in dis_before(pos):
        mark = ">>" if i.address == pos else "  "
        print(f"{mark} {i.address:05X}: {i.mnemonic} {i.op_str}")
    print()


code_end = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1A000
print("\n=== INT 10/16/1A/33 and video memory segments ===")
for p in range(code_end):
    if img[p] == 0xCD and img[p + 1] in (0x10, 0x16, 0x1A, 0x33):
        print(f"--- INT {img[p + 1]:02X} @ {p:05X}")
        show(p)
for p in range(code_end):
    if img[p] == 0xB8 and img[p + 1 : p + 3] in (b"\x00\xa0", b"\x00\xb8", b"\x00\xa8"):
        print(f"--- MOV AX,video seg @ {p:05X}: {img[p + 2]:02X}00")
print("\n=== Port I/O (OUT DX) ===")
cnt = 0
for p in range(code_end):
    if img[p] in (0xEE, 0xEF, 0xEC, 0xE6, 0xE4) and cnt < 45:
        for back in range(12, 0, -1):
            o = list(md.disasm(img[p - back : p + 2], p - back))
            if o and any(i.address == p for i in o):
                if o[0].mnemonic.startswith(("mov", "out", "in", "push", "pop", "xor", "inc", "dec", "cmp")):
                    print(f"--- port op @ {p:05X}: " + "; ".join(f"{i.mnemonic} {i.op_str}" for i in o[-4:]))
                    cnt += 1
                break
