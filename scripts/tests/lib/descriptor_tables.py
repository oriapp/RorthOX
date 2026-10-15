"""descriptor-tables.sh, debugger side: the GDT and TSS the kernel loads.

QEMU waits at reset.
1. Break at gdt_load when kernel_main calls it and run it to its return address. The GDTR must hold
   the base of gdt_real and the inclusive limit 6 * 8 - 1 = 0x2f. The five code/data/null
   descriptors in memory must be exactly the flat ones, and the TSS descriptor must describe the tss
   object: base, limit 0x67 (104 bytes - 1), byte granularity, access 0xE9 and bit 54 clear.
2. Break at tss_load: before LTR the TSS must already hold esp0 (the TSS stack top), ss0 (the kernel
   data selector) and an I/O map base of 104 (past the limit, so there is no I/O bitmap and ring 3
   port I/O faults). Every other byte is 0.
3. Run tss_load to its return address. TR must hold the TSS selector, the base of tss and the limit
   0x67, and the descriptor in memory must now be busy (type 11, access 0xEB). QEMU's cached TR
   flags are not checked for busy: it loads the cache before it marks the descriptor in memory.
Then the debugger detaches so the OS boots on.

Usage: descriptor_tables.py <gdb socket> <kernel.map> <results file>
"""

import re
import struct
import sys

from rsp import Results, Rsp, RspError, read_config, read_map

GDT_ENTRIES = 6
FLAT = {
    0: bytes([0, 0, 0, 0, 0, 0, 0x40, 0]),
    1: bytes([0xff, 0xff, 0, 0, 0, 0x9a, 0xcf, 0]),
    2: bytes([0xff, 0xff, 0, 0, 0, 0x92, 0xcf, 0]),
    3: bytes([0xff, 0xff, 0, 0, 0, 0xf8, 0xcf, 0]),
    4: bytes([0xff, 0xff, 0, 0, 0, 0xf2, 0xcf, 0]),
}


def info_registers(r):
    text = r.monitor("info registers")
    gdt = re.search(r"^GDT=\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)", text, re.M)
    tr = re.search(r"^TR =([0-9a-fA-F]+) ([0-9a-fA-F]+) ([0-9a-fA-F]+) ([0-9a-fA-F]+)", text, re.M)
    if not gdt or not tr:
        raise RspError("no GDT or TR line in info registers")
    return {"gdt_base": int(gdt.group(1), 16), "gdt_limit": int(gdt.group(2), 16),
            "tr_sel": int(tr.group(1), 16), "tr_base": int(tr.group(2), 16), "tr_limit": int(tr.group(3), 16),
            "tr_flags": int(tr.group(4), 16), "tr_line": tr.group(0)}


def descriptor(d):
    base = d[2] | d[3] << 8 | d[4] << 16 | d[7] << 24
    limit = d[0] | d[1] << 8 | (d[6] & 0x0F) << 16
    return base, limit, d[5], d[6] & 0xF0


def run_function(r, addr, timeout):
    """Break at <addr>, then run that call to its return address. Returns (esp at entry, return address)."""
    r.add_breakpoint(addr)
    eip = r.cont(timeout)
    r.remove_breakpoint(addr)
    if eip != addr:
        raise RspError("stopped at %#x, expected %#x" % (eip, addr))
    esp = r.reg("esp")
    return esp, r.read32(esp)


def main():
    sock, map_path, result_path = sys.argv[1:4]
    res = Results(result_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    symbols, _ = read_map(map_path)
    gdt_real = symbols["gdt_real"]
    tss = symbols["tss"]
    tss_sel = config["RORTHOX_TSS_SELECTOR"]

    r = Rsp(sock)
    try:
        # 1. gdt_load
        esp, ret = run_function(r, symbols["gdt_load"], 60)
        r.add_breakpoint(ret)
        eip = r.cont(20)
        r.remove_breakpoint(ret)
        res.add("gdt_load_returned", eip == ret, "stopped at %#x, expected %#x" % (eip, ret))
        regs = info_registers(r)
        print("  after gdt_load: GDTR base %#x limit %#x" % (regs["gdt_base"], regs["gdt_limit"]), flush=True)
        res.add("gdtr_base", regs["gdt_base"] == gdt_real, "GDTR base %#x, gdt_real %#x" % (regs["gdt_base"], gdt_real))
        res.add("gdtr_limit", regs["gdt_limit"] == GDT_ENTRIES * 8 - 1,
                "GDTR limit %#x, expected %#x" % (regs["gdt_limit"], GDT_ENTRIES * 8 - 1))
        gdt = r.read(gdt_real, GDT_ENTRIES * 8)
        for i in range(GDT_ENTRIES):
            print("  descriptor %d (selector %#04x): %s" % (i, i * 8, gdt[i * 8:i * 8 + 8].hex(" ")), flush=True)
        bad = [i for i, want in FLAT.items() if gdt[i * 8:i * 8 + 8] != want]
        res.add("flat_descriptors", not bad, "descriptors %s differ from the flat ones" % bad)
        base, limit, access, flags = descriptor(gdt[tss_sel:tss_sel + 8])
        res.add("tss_descriptor_base", base == tss, "base %#x, tss at %#x" % (base, tss))
        res.add("tss_descriptor_limit", limit == 0x67 and not flags & 0x80,
                "limit %#x, granularity %s, expected 0x67 in bytes" % (limit, "4 KiB" if flags & 0x80 else "byte"))
        res.add("tss_descriptor_access", access == 0xE9, "access %#x, expected 0xe9" % access)
        res.add("tss_descriptor_bit54_clear", not flags & 0x40, "byte 6 flags %#x: bit 54 set" % flags)

        # 2. tss_load: the TSS before LTR
        esp, ret = run_function(r, symbols["tss_load"], 30)
        arg = r.read32(esp + 4)
        res.add("tss_load_selector", arg == tss_sel, "tss_load(%#x), expected %#x" % (arg, tss_sel))
        t = r.read(tss, 104)
        esp0, ss0 = struct.unpack_from("<II", t, 4)
        iomap, = struct.unpack_from("<H", t, 102)
        print("  TSS before LTR: esp0 %#x ss0 %#x I/O map base %#x (offset 102)" % (esp0, ss0, iomap), flush=True)
        res.add("tss_esp0", esp0 == config["RORTHOX_KERNEL_TSS_STACK_TOP"], "esp0 %#x" % esp0)
        res.add("tss_ss0", ss0 == config["KERNEL_DATA_SELECTOR"], "ss0 %#x" % ss0)
        res.add("tss_iomap_base_before_ltr", iomap == 104, "I/O map base %#x, expected 0x68 (104)" % iomap)
        rest = bytearray(t)
        rest[4:12] = bytes(8)
        rest[102:104] = bytes(2)
        res.add("tss_rest_zero", not any(rest), "nonzero bytes at %s" % [i for i, b in enumerate(rest) if b][:8])

        # 3. after LTR
        r.add_breakpoint(ret)
        eip = r.cont(20)
        r.remove_breakpoint(ret)
        res.add("tss_load_returned", eip == ret, "stopped at %#x, expected %#x" % (eip, ret))
        regs = info_registers(r)
        print("  after tss_load: %s" % regs["tr_line"], flush=True)
        res.add("tr_selector", regs["tr_sel"] == tss_sel, "TR selector %#x" % regs["tr_sel"])
        res.add("tr_base", regs["tr_base"] == tss, "TR base %#x, tss at %#x" % (regs["tr_base"], tss))
        res.add("tr_limit", regs["tr_limit"] == 0x67, "TR limit %#x, expected 0x67" % regs["tr_limit"])
        d = r.read(gdt_real + tss_sel, 8)
        res.add("tss_descriptor_busy", d[5] == 0xEB, "access byte in memory %#x, expected 0xeb (busy)" % d[5])
        r.detach()
    except (RspError, OSError, KeyError) as e:
        res.add("session", False, str(e))
        try:
            r.detach()
        except Exception:
            pass
        raise SystemExit(1)


if __name__ == "__main__":
    main()
