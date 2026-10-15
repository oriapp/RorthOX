"""idt-load.sh, debugger side: idt_load must keep the cdecl callee-saved registers and load the IDTR.

QEMU waits at reset. Break at idt_load when kernel_main calls it, record the return address and the
idtr descriptor it was passed, put sentinels in EBX, ESI, EDI and EBP, break at the return address
and let the real function run. All four sentinels must survive, ESP must be back above the return
address, and the CPU's IDTR must hold the base and limit from the descriptor, with the limit of the
256 x86 vectors (0x7ff). Then the caller's registers are put back and the debugger detaches so the
OS boots on.

Usage: idt_load.py <gdb socket> <kernel.map> <results file>
"""

import struct
import sys

from rsp import Results, Rsp, read_config, read_map

SENTINELS = {"ebx": 0xB0B1B2B3, "esi": 0x51525354, "edi": 0xD1D2D3D4, "ebp": 0xEB0EB1E2}


def main():
    sock, map_path, result_path = sys.argv[1:4]
    res = Results(result_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    symbols, _ = read_map(map_path)
    idt_load = symbols["idt_load"]

    r = Rsp(sock)
    r.add_breakpoint(idt_load)
    eip = r.cont(60)
    r.remove_breakpoint(idt_load)
    ok = eip == idt_load and r.reg("cs") == config["KERNEL_CODE_SELECTOR"]
    res.add("at_idt_load", ok, "stopped at %#x" % eip)
    if not ok:
        raise SystemExit(1)

    caller = r.regs()
    esp = caller["esp"]
    ret, arg = struct.unpack("<II", r.read(esp, 8))
    limit, base = struct.unpack("<HI", r.read(arg, 6))
    print("  idt_load(%#x): base %#x limit %#x, returns to %#x" % (arg, base, limit, ret), flush=True)
    # The argument must be the kernel's idtr descriptor describing the kernel's IDT,
    # otherwise the IDTR check below proves nothing.
    res.add("argument_is_idtr", arg == symbols.get("idtr_descriptor") and base == symbols.get("idt_descriptors")
            and limit == config["RORTHOX_TOTAL_INTERRUPTS"] * 8 - 1,
            "argument %#x base %#x limit %#x" % (arg, base, limit))
    before = r.control_registers()
    res.add("idtr_differs_before", (before["idt_base"], before["idt_limit"]) != (base, limit),
            "IDTR already %#x/%#x before the call" % (base, limit))

    r.set_regs(SENTINELS)
    r.add_breakpoint(ret)
    eip = r.cont(30)
    r.remove_breakpoint(ret)
    res.add("returned", eip == ret, "stopped at %#x, expected the return address %#x" % (eip, ret))

    after = r.regs()
    for name, value in SENTINELS.items():
        detail = "%s=%#x, expected %#x" % (name, after[name], value)
        if after[name] == arg:
            detail += " (the idtr pointer)"
        res.add("%s_preserved" % name, after[name] == value, detail)
    res.add("stack_balanced", after["esp"] == esp + 4, "ESP=%#x, expected %#x" % (after["esp"], esp + 4))
    cr = r.control_registers()
    res.add("idtr_loaded", (cr["idt_base"], cr["idt_limit"]) == (base, limit),
            "IDTR %#x/%#x, expected %#x/%#x" % (cr["idt_base"], cr["idt_limit"], base, limit))
    # x86 has 256 vectors: the loaded limit must be 256 * 8 - 1, whatever the build config says.
    res.add("idtr_limit_256_vectors", cr["idt_limit"] == 256 * 8 - 1 and config["RORTHOX_TOTAL_INTERRUPTS"] == 256,
            "IDTR limit %#x (expected 0x7ff), TOTAL_INTERRUPTS %d (expected 256)" %
            (cr["idt_limit"], config["RORTHOX_TOTAL_INTERRUPTS"]))

    # Give the caller back its own registers before it runs on.
    r.set_regs({name: caller[name] for name in SENTINELS})
    r.detach()


if __name__ == "__main__":
    main()
