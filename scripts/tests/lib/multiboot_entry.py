"""Observe the real GRUB handoff and the kernel's bootstrap through QEMU's gdbstub."""
import pathlib
import re
import sys

from rsp import EFLAGS_DF, EFLAGS_IF, Results, Rsp, RspError, read_config, read_map


def main():
    sock, map_path, kernel_path, out_path = sys.argv[1:]
    out = pathlib.Path(out_path)
    symbols, sections = read_map(map_path)
    cfg = read_config("build/config/rorthox-config.inc")
    results = Results(out / "rsp-result.txt")

    def require(name, ok, detail=""):
        if not results.add(name, ok, detail):
            raise RspError(name + ": " + detail)

    g = Rsp(sock, timeout=10)

    def state(name):
        text = g.monitor("info registers")
        (out / (name + "-registers.txt")).write_text(text)
        print(name + ":\n" + text, flush=True)
        cr0 = re.search(r"\bCR0=([0-9a-fA-F]+)", text)
        if not cr0:
            raise RspError("no CR0 in info registers")
        return g.regs(), int(cr0.group(1), 16), text

    entry = symbols["multiboot_entry"]
    join = symbols["kernel_boot_init"]
    kmain = symbols["kernel_main"]
    load = cfg["KERNEL_LOAD_ADDRESS"]
    top = cfg["KERNEL_BOOT_STACK_TOP"]
    bss0, bss1 = symbols["__bss_start"], symbols["__bss_end"]
    image = pathlib.Path(kernel_path).read_bytes()
    require("bss_bounds", sections[".bss"] == (bss0, bss1 - bss0)
            and load + len(image) <= bss0 < bss1 < top)
    # GRUB itself uses 1 MiB before loading the kernel; watch only the handoff
    # here, then guard against entering the raw path after the handoff.
    g.add_breakpoint(entry)
    eip = g.cont(25)
    require("at_multiboot_entry", eip == entry, hex(eip))
    regs, cr0, _ = state("entry")
    require("entry_magic", regs["eax"] == 0x2BADB002, hex(regs["eax"]))
    # EBX is recorded, not dereferenced; the kernel deliberately ignores the MBI.
    print("Multiboot information pointer EBX=%#x" % regs["ebx"], flush=True)
    require("entry_pe1_pg0", cr0 & 0x80000001 == 1, hex(cr0))
    require("entry_if0", not regs["eflags"] & EFLAGS_IF, hex(regs["eflags"]))
    require("kernel_loaded", g.read(load, len(image)) == image)
    poison = b"\xa5" * (bss1 - bss0)
    g.write(bss0, poison)
    require("bss_poisoned", g.read(bss0, len(poison)) == poison)
    g.set_reg("eflags", regs["eflags"] | EFLAGS_DF)
    require("df_set", bool(g.reg("eflags") & EFLAGS_DF))

    g.remove_breakpoint(entry)
    g.add_breakpoint(symbols["_start"])
    g.add_breakpoint(join)
    eip = g.cont(10)
    require("at_boot_init", eip == join, hex(eip))
    regs, cr0, text = state("join")
    gdtr = re.search(r"^GDT=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", text, re.M)
    require("join_gdtr", gdtr is not None and
            tuple(int(x, 16) for x in gdtr.groups()) == (symbols["multiboot_gdt"], 23))
    for name in ("cs", "ss", "ds", "es", "fs", "gs"):
        code = name == "cs"
        selector = cfg["KERNEL_CODE_SELECTOR" if code else "KERNEL_DATA_SELECTOR"]
        require("join_" + name + "_selector", regs[name] == selector, hex(regs[name]))
        segment = re.search(r"^%s\s*=([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"
                            r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)" % name.upper(), text, re.M)
        if not segment:
            raise RspError("missing segment cache: " + name)
        _, base, limit, attr = (int(x, 16) for x in segment.groups())
        # QEMU puts the CPU-set descriptor accessed bit at bit 8 of this word.
        require("join_" + name + "_flat", base == 0 and limit == 0xFFFFFFFF
                and attr & ~0x100 == (0xCF9A00 if code else 0xCF9200))
    require("join_stack", regs["esp"] == top and regs["ebp"] == top)
    require("join_if0_pg0", not regs["eflags"] & EFLAGS_IF and not cr0 & 0x80000000)

    g.remove_breakpoint(join)
    g.add_breakpoint(kmain)
    eip = g.cont(10)
    require("at_kernel_main", eip == kmain, hex(eip))
    regs, cr0, _ = state("main")
    require("main_if0_df0", not regs["eflags"] & (EFLAGS_IF | EFLAGS_DF))
    require("main_pg0", not cr0 & 0x80000000)
    require("main_stack", regs["esp"] == top - 4 and regs["ebp"] == top)
    require("main_bss_zero", g.read(bss0, bss1 - bss0) == bytes(bss1 - bss0))
    actual = bytearray(g.read(load, len(image)))
    expected = bytearray(image)
    # Loading the bootstrap descriptors may set their accessed bits in RAM.
    for slot in (cfg["KERNEL_CODE_SELECTOR"], cfg["KERNEL_DATA_SELECTOR"]):
        offset = symbols["multiboot_gdt"] - load + slot + 5
        actual[offset] &= ~1
        expected[offset] &= ~1
    require("image_unchanged", actual == expected)
    g.detach()
    require("detached", True)


if __name__ == "__main__":
    try:
        main()
    except (RspError, OSError, ValueError, KeyError, TimeoutError) as exc:
        print("FAIL Multiboot session: " + str(exc), file=sys.stderr, flush=True)
        sys.exit(1)
