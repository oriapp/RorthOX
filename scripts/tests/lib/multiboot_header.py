"""Static Multiboot 1 address-fields contract for the shared flat kernel."""

import struct
import sys
from pathlib import Path

from rsp import read_config, read_map


def bootstrap_size(path):
    """Read .text size from the NASM ELF32 object, before the partial link merges it."""
    elf = Path(path).read_bytes()
    if elf[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("bootstrap object is not little-endian ELF32")
    shoff, = struct.unpack_from("<I", elf, 32)
    entsize, count, names_index = struct.unpack_from("<HHH", elf, 46)
    if entsize != 40 or names_index >= count:
        raise ValueError("invalid bootstrap section table")
    sections = [struct.unpack_from("<10I", elf, shoff + i * entsize) for i in range(count)]
    names = sections[names_index]
    strings = elf[names[4]:names[4] + names[5]]
    text = [s for s in sections if strings[s[0]:].split(b"\0", 1)[0] == b".text"]
    if len(text) != 1 or text[0][1] != 1 or not text[0][2] & 2:
        raise ValueError("missing loaded bootstrap .text section")
    if any(s != text[0] and s[2] & 2 and s[5] for s in sections):
        raise ValueError("bootstrap has loaded sections outside .text")
    return text[0][5]


def main(kernel_path, map_path, config_path, object_path):
    image = Path(kernel_path).read_bytes()
    symbols, _ = read_map(map_path)
    config = read_config(config_path)
    failures = []

    def check(name, ok, detail):
        print(("PASS " if ok else "FAIL ") + name + ": " + detail)
        if not ok:
            failures.append(name)

    load = config["KERNEL_LOAD_ADDRESS"]
    check("raw entry", symbols.get("_start") == load, "_start == %#x" % load)
    size = bootstrap_size(object_path)
    check("bootstrap size", size == 512, ".text is %d bytes (expected 512)" % size)
    # Do not silently select a later header if the first magic has bad fields.
    magic = struct.pack("<I", 0x1BADB002)
    offsets = [i for i in range(0, min(len(image), 8192) - 3, 4) if image[i:i + 4] == magic]
    check("header found", bool(offsets), "aligned Multiboot magic in first 8192 bytes")
    if not offsets:
        return 1
    offset = offsets[0]
    check("header extent", offset + 32 <= min(512, len(image)),
          "offset %#x, 32 bytes, entirely in bootstrap and first 8192 bytes" % offset)
    if offset + 32 > min(8192, len(image)):
        return 1
    fields = struct.unpack_from("<8I", image, offset)
    magic_value, flags, checksum, header, load_addr, load_end, bss_end, entry = fields
    print("header words: " + " ".join("%08x" % value for value in fields))
    check("magic", magic_value == 0x1BADB002, "%#x" % magic_value)
    check("flags", flags == 0x00010000, "%#x" % flags)
    check("checksum", (magic_value + flags + checksum) & 0xFFFFFFFF == 0,
          "32-bit magic + flags + checksum == 0")
    check("header symbol", header == symbols.get("multiboot_header"),
          "header_addr %#x == multiboot_header" % header)
    check("header alignment", header % 4 == 0 and offset % 4 == 0,
          "address and file offset are 4-byte aligned")
    check("file offset", offset == header - load_addr,
          "%#x == header_addr - load_addr" % offset)
    check("load address", load_addr == load, "%#x == configured %#x" % (load_addr, load))
    check("load end", load_end == 0, "%#x (load the rest of the file)" % load_end)
    check("bss end symbol", bss_end == symbols.get("__bss_end"),
          "%#x == __bss_end" % bss_end)
    check("bss covers file", bss_end >= load_addr + len(image),
          "%#x >= %#x + %d" % (bss_end, load_addr, len(image)))
    check("separate entry", entry == symbols.get("multiboot_entry") and entry != symbols.get("_start"),
          "%#x == multiboot_entry != _start" % entry)
    check("entry extent", load <= entry < load + min(512, len(image)),
          "%#x is inside the first 512 loaded bytes" % entry)
    join = symbols.get("kernel_boot_init")
    check("shared join symbol", join is not None and load < join < load + 512,
          "kernel_boot_init is exported inside the bootstrap")
    print("multiboot-header: %d failed" % len(failures))
    return int(bool(failures))


if __name__ == "__main__":
    try:
        sys.exit(main(*sys.argv[1:]))
    except (OSError, ValueError, KeyError, struct.error, TypeError) as error:
        print("FAIL multiboot-header: %s (run 'make all' first)" % error, file=sys.stderr)
        sys.exit(1)
