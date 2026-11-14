"""boot-segments.sh, debugger side: the machine state the boot sector reads the kernel with.

QEMU waits at reset. The addresses come from the NASM listing of kernel/boot/boot.asm (load32, the
first "call ata_lba_read" after it, the first "rep insw" in ata_lba_read). At most two breakpoints
are set at once, and nothing single-steps after the debugger moved eip.

Mode "bios": the boot sector runs from the state the BIOS left.
Mode "dirty": at load32 the debugger leaves state a BIOS may leave too, but SeaBIOS doesn't: DF=1,
the upper half of ESP not 0, and the A20 gate off (an 11-byte stub at STUB clears bit 1 of port
0x92, then jumps back to load32). The second stop at load32 proves the stub worked.

At the first call and at the first rep insw: DS, ES, FS, GS and SS hold the data selector with base
0, limit 0xffffffff, a writable data segment, G and D/B set ("info registers" segment caches). TCG
doesn't check segment limits on rep insw, so this is the only way to see a 64 KiB limit there.
ESP is the boot stack 0x7c00 at the call, DF is 0 and A20 is on at rep insw, and rep insw reads 256
words from port 0x1f0 to the kernel load address.

Usage: boot_segments.py <gdb socket> <listing> <boot.bin> <rorthox-config.inc> <bios|dirty> <results file>
"""

import re
import sys

from rsp import Results, Rsp, RspError, read_config

STUB = 0x1000
DIRTY_ESP = 0xDEAD7C00
BOOT_STACK = 0x7C00
LISTING = re.compile(r"^\s*\d+\s+([0-9A-F]{8})\s+[0-9A-F]+-?\s+(.*)$")


def addresses(path):
    rows, label_at = [], {}
    for line in open(path):
        m = LISTING.match(line)
        if m:
            rows.append((0x7C00 + int(m.group(1), 16), m.group(2).split(";")[0].strip()))
            continue
        m = re.match(r"^\s*\d+\s+([.\w]+):\s*(;.*)?$", line)
        if m:
            label_at[m.group(1)] = len(rows)    # the next instruction row

    def first(text, after):
        for addr, insn in rows[label_at[after]:]:
            if insn == text:
                return addr
        raise RspError("no %r after %s in the listing" % (text, after))
    return {"load32": rows[label_at["load32"]][0], "call": first("call ata_lba_read", "load32"),
            "insw": first("rep insw", "ata_lba_read")}


def state(r):
    text = r.monitor("info registers")
    seg = {m.group(1): tuple(int(x, 16) for x in m.groups()[1:]) for m in
           re.finditer(r"^(ES|CS|SS|DS|FS|GS) =([0-9a-f]{4}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})", text, re.M)}
    reg = {k: int(re.search(r"\b%s=([0-9a-f]+)" % k, text).group(1), 16)
           for k in ("EIP", "ESP", "EFL", "EDI", "ECX", "EDX", "A20")}
    return seg, reg


def check_state(res, where, seg, reg, data_sel):
    for name in ("DS", "ES", "FS", "GS", "SS"):
        sel, base, limit, flags = seg[name]
        # flags = descriptor bits 8..23: access byte P=1 DPL=0 S=1 data, not expand-down, writable
        # (accessed may be set), and G=1 D/B=1.
        ok = sel == data_sel and base == 0 and limit == 0xFFFFFFFF and (flags >> 8) & 0xFE == 0x92 \
            and (flags >> 22) & 3 == 3
        res.add("%s_%s_flat" % (where, name.lower()), ok, "%s=%04x %08x %08x %08x" % ((name,) + seg[name]))


def run_to(r, addr, timeout):
    r.add_breakpoint(addr)
    try:
        return r.cont(timeout)
    finally:
        r.remove_breakpoint(addr)


def main():
    sock, listing, boot_bin, config_inc, mode, results = sys.argv[1:7]
    res = Results(results)
    cfg = read_config(config_inc)
    a = addresses(listing)
    print("  addresses: " + " ".join("%s=%#x" % kv for kv in sorted(a.items())), flush=True)
    image = open(boot_bin, "rb").read()
    r = Rsp(sock)
    eip = run_to(r, a["load32"], 30)
    seg, reg = state(r)
    code = r.read(a["load32"], 16)
    res.add("load32", eip == a["load32"] and seg["CS"][0] == cfg["KERNEL_CODE_SELECTOR"] and
            code == image[a["load32"] - 0x7C00:][:16], "eip %#x cs %04x code %s" % (eip, seg["CS"][0], code.hex()))
    if mode == "dirty":
        jump = (a["load32"] - (STUB + 11)) & 0xFFFFFFFF
        # in al, 0x92 / and al, 0xfc / out 0x92, al / jmp load32
        r.write(STUB, bytes([0xE4, 0x92, 0x24, 0xFC, 0xE6, 0x92, 0xE9]) + jump.to_bytes(4, "little"))
        r.set_reg("esp", DIRTY_ESP)
        r.set_reg("eflags", r.reg("eflags") | 0x400)
        r.set_reg("eip", STUB)
        eip = run_to(r, a["load32"], 30)
        seg, reg = state(r)
        res.add("dirty_state", eip == a["load32"] and reg["A20"] == 0 and reg["ESP"] == DIRTY_ESP and
                reg["EFL"] & 0x400 != 0, "eip %#x A20=%d ESP=%08x EFL=%08x" % (eip, reg["A20"], reg["ESP"], reg["EFL"]))
    eip = run_to(r, a["call"], 30)
    seg, reg = state(r)
    res.add("call_reached", eip == a["call"], "eip %#x" % eip)
    check_state(res, "call", seg, reg, cfg["KERNEL_DATA_SELECTOR"])
    res.add("call_esp", reg["ESP"] == BOOT_STACK, "ESP=%08x" % reg["ESP"])
    eip = run_to(r, a["insw"], 30)
    seg, reg = state(r)
    res.add("insw_reached", eip == a["insw"], "eip %#x" % eip)
    check_state(res, "insw", seg, reg, cfg["KERNEL_DATA_SELECTOR"])
    res.add("insw_df", not reg["EFL"] & 0x400, "EFL=%08x" % reg["EFL"])
    res.add("insw_a20", reg["A20"] == 1, "A20=%d" % reg["A20"])
    res.add("insw_operands", reg["EDI"] == cfg["KERNEL_LOAD_ADDRESS"] and reg["ECX"] == cfg["SECTOR_SIZE"] // 2
            and reg["EDX"] & 0xFFFF == 0x1F0, "EDI=%08x ECX=%08x EDX=%08x" % (reg["EDI"], reg["ECX"], reg["EDX"]))
    r.detach()


if __name__ == "__main__":
    try:
        main()
    except (RspError, OSError, KeyError, AttributeError) as e:
        print("  boot_segments: %s: %s" % (type(e).__name__, e), flush=True)
        sys.exit(1)
