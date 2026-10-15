"""interrupt-count.sh, object side: count the interrupt stubs, the pointer table and the IDT arrays.

Usage: interrupt_count.py <idt.asm.o> <idt.o> <kernel elf> <results file>

idt.asm.o   the int<N> labels defined (local or global) must be exactly int0 ... int255, and the
            pointer table (the last thing in .data) must hold 256 four-byte entries.
idt.o       idt_descriptors must be 2048 bytes (256 gates of 8 bytes) and interrupt_callbacks
            1024 bytes (256 pointers of 4 bytes).
kernel elf  the kernel linked as ELF: entry i of interrupt_pointer_table is the address of int<i>,
            for every i.
All numbers here are the architecture's (256 vectors), not values read from the build config.
"""

import re
import struct
import sys

from rsp import Results

VECTORS = 256


def elf_sections(elf):
    if elf[:6] != b"\x7fELF\x01\x01":
        raise SystemExit("not a 32-bit little-endian ELF file")
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    headers = [struct.unpack_from("<10I", elf, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    out = []
    for h in headers:
        start = names + h[0]
        out.append((elf[start:elf.index(b"\0", start)].decode("latin-1"), h))
    return out


def elf_symbols(elf, sections):
    """[(name, value, size, section index)] from the symbol table."""
    out = []
    for _, h in sections:
        if h[1] != 2:       # SHT_SYMTAB
            continue
        strtab = sections[h[6]][1]
        for off in range(h[4], h[4] + h[5], 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf, off)
            start = strtab[4] + name
            out.append((elf[start:elf.index(b"\0", start)].decode("latin-1"), value, size, shndx))
    return out


def main():
    asm_path, c_path, kernel_path, result_path = sys.argv[1:5]
    res = Results(result_path)

    elf = open(asm_path, "rb").read()
    sections = elf_sections(elf)
    symbols = elf_symbols(elf, sections)
    stubs = {}
    for name, value, size, shndx in symbols:
        m = re.match(r"^int(\d+)$", name)
        if m and shndx != 0:
            stubs[int(m.group(1))] = value
    numbers = sorted(stubs)
    print("  stubs in idt.asm.o: %d, from int%s to int%s" % (len(numbers), numbers[0] if numbers else "-",
                                                             numbers[-1] if numbers else "-"), flush=True)
    res.add("stubs_0_to_255", all(v in stubs for v in range(VECTORS)),
            "missing %s" % [v for v in range(VECTORS) if v not in stubs][:8])
    extra = [v for v in numbers if v >= VECTORS]
    res.add("no_stub_256_or_above", not extra, "%d stubs from int256 up, first int%s" % (len(extra), extra[0] if extra else ""))

    data = [h for name, h in sections if name == ".data"]
    table = [(value, shndx) for name, value, size, shndx in symbols if name == "interrupt_pointer_table"]
    if data and table and sections[table[0][1]][0] == ".data":
        entries = (data[0][5] - table[0][0]) / 4
        print("  pointer table: .data is %d bytes, the table starts at %d: %g entries" %
              (data[0][5], table[0][0], entries), flush=True)
        res.add("pointer_table_256", entries == VECTORS, "%g entries" % entries)
    else:
        res.add("pointer_table_256", False, "no interrupt_pointer_table in .data")

    elf = open(c_path, "rb").read()
    sizes = {name: size for name, value, size, shndx in elf_symbols(elf, elf_sections(elf))}
    print("  idt.o: idt_descriptors %s bytes, interrupt_callbacks %s bytes" %
          (sizes.get("idt_descriptors"), sizes.get("interrupt_callbacks")), flush=True)
    res.add("idt_2048_bytes", sizes.get("idt_descriptors") == VECTORS * 8, "idt_descriptors %s bytes" % sizes.get("idt_descriptors"))
    res.add("callbacks_1024_bytes", sizes.get("interrupt_callbacks") == VECTORS * 4,
            "interrupt_callbacks %s bytes" % sizes.get("interrupt_callbacks"))

    elf = open(kernel_path, "rb").read()
    sections = elf_sections(elf)
    ksyms = {name: value for name, value, size, shndx in elf_symbols(elf, sections) if shndx != 0}
    base = ksyms.get("interrupt_pointer_table")
    bad = []
    if base is None:
        bad.append("no interrupt_pointer_table")
    else:
        for name, h in sections:
            if h[1] == 1 and h[3] <= base < h[3] + h[5]:      # SHT_PROGBITS holding the table
                off = h[4] + base - h[3]
                words = struct.unpack_from("<%dI" % VECTORS, elf, off)
                bad = [i for i in range(VECTORS) if words[i] != ksyms.get("int%d" % i)]
                break
        else:
            bad.append("table not in a section")
    res.add("pointer_table_order", not bad, "entries %s do not point to their stub" % bad[:8])


if __name__ == "__main__":
    main()
