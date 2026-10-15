#!/usr/bin/env python3
"""Heap blocks the kernel takes to load an ELF file (scripts/tests/heap-oom.sh).

Usage: elfblocks.py <file.elf> [block size, default 4096]

Prints "E=<n> M=<n> S=<n> D=<n> H=<n>":
  E  blocks of the whole file (the loader reads it into one buffer)
  M  blocks of the pointer array, one 4-byte pointer per program header
  S  blocks of the segment memory: every PT_LOAD with p_memsz > 0 gets
     ceil((p_vaddr % 4096 + p_memsz) / 4096) blocks
  D  M + S, the blocks the loaded program keeps besides the file buffer
  H  how many of the D blocks first fit puts into the 3-block hole that the
     parsed path leaves while the file is open (the array first, then the
     segments in program header order)

Reads the headers with the struct module only, so it does not depend on the
kernel's own parser. Exits with 1 if the file is not a 32-bit LSB ELF file.
"""
import struct
import sys


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: elfblocks.py <file.elf> [block size]")
    block = int(sys.argv[2]) if len(sys.argv) == 3 else 4096
    data = open(sys.argv[1], "rb").read()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        print("elfblocks: not a 32-bit little-endian ELF file", file=sys.stderr)
        sys.exit(1)
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if phentsize != 32 or phoff + phnum * 32 > len(data):
        print("elfblocks: bad program header table", file=sys.stderr)
        sys.exit(1)

    def blocks(n):
        return (n + block - 1) // block

    e = blocks(len(data))
    m = blocks(phnum * 4)
    segments = []
    for i in range(phnum):
        p_type, _off, vaddr, _paddr, _filesz, memsz, _flags, _align = struct.unpack_from(
            "<8I", data, phoff + i * 32)
        if p_type == 1 and memsz > 0:
            segments.append(blocks(vaddr % block + memsz))
    s = sum(segments)
    hole = 3
    h = 0
    for n in [m] + segments:
        if n <= hole:
            hole -= n
            h += n
    print(f"E={e} M={m} S={s} D={m + s} H={h}")


if __name__ == "__main__":
    main()
