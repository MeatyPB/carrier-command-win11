#!/usr/bin/env python3
"""Unpack a Microsoft EXEPACK DOS executable into a flat memory image.

Usage: unexepack.py CARRIER.EXE out.bin
Writes the decompressed load image (loads at paragraph 0 of the image) and
prints the real entry point and stack. Relocations from the EXEPACK table are
written to out.bin.rel as little-endian (offset, segment) word pairs.
"""
import struct
import sys


def main(src, dst):
    d = open(src, "rb").read()
    last, pages, nrel, hdr, minal, maxal, ss, sp, _, ip, cs, _, _ = struct.unpack_from("<13H", d, 2)
    size = (pages - 1) * 512 + last if last else pages * 512
    base = hdr * 16
    stub = base + cs * 16
    real_ip, real_cs, _mem, ex_size, real_sp, real_ss, dest_len, skip, sig = struct.unpack_from("<8H2s", d, stub)
    print(f"image={size - base:#x} minalloc={minal:#x} stub at {stub:#x} sig={sig!r} skip={skip}")
    assert sig == b"RB", "not EXEPACK"

    # Compressed data ends at the stub; output occupies dest_len paragraphs.
    src_end = cs * 16
    out = bytearray(dest_len * 16)
    data = d[base:stub]
    si = src_end - 1
    # Strip 0xFF padding at the end of the compressed stream.
    while data[si] == 0xFF:
        si -= 1
    di = len(out) - 1
    while True:
        op = data[si]
        cnt = data[si - 1] << 8 | data[si - 2]
        si -= 3
        kind = op & 0xFE
        if kind == 0xB0:
            fill = data[si]
            si -= 1
            for _ in range(cnt):
                out[di] = fill
                di -= 1
        elif kind == 0xB2:
            for _ in range(cnt):
                out[di] = data[si]
                di -= 1
                si -= 1
        else:
            raise SystemExit(f"bad op {op:#x} at {si + 3:#x}")
        if op & 1:
            break
    print(f"decompressed: dest={len(out):#x} remaining_out={di + 1:#x} remaining_in={si + 1:#x}")
    # Any bytes below the decompressed area are the unmoved head of the stream.
    if si >= 0:
        out[: si + 1] = data[: si + 1]

    # Relocation table: after the stub code, 16 lists of (count, offsets).
    stub_blob = d[stub:]
    # The table follows the "Packed file is corrupt" message.
    msg = stub_blob.find(b"Packed file is corrupt")
    assert msg >= 0
    p = msg + len(b"Packed file is corrupt")
    rel = []
    for seg_hi in range(16):
        n, = struct.unpack_from("<H", stub_blob, p)
        p += 2
        for _ in range(n):
            off, = struct.unpack_from("<H", stub_blob, p)
            p += 2
            rel.append((off, seg_hi * 0x1000))
    print(f"relocs={len(rel)} real entry {real_cs:04X}:{real_ip:04X} stack {real_ss:04X}:{real_sp:04X}")
    open(dst, "wb").write(out)
    with open(dst + ".rel", "wb") as f:
        for off, seg in rel:
            f.write(struct.pack("<HH", off, seg))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
