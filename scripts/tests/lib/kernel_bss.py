"""kernel-bss.sh, debugger side: the kernel's .bss must be all zero when kernel_main starts.

QEMU waits at reset. Break at _start (the kernel is loaded by then), fill .bss with a nonzero
pattern, put a canary byte on each side of it, set DF, and break again at the first instruction
of kernel_main. Every .bss byte must be zero there, the canaries and the loaded kernel image must
be untouched, and DF and IF must be clear on the boot stack. Then detach and let the OS boot.

Usage: kernel_bss.py <gdb socket> <kernel.map> <kernel.bin> <results file>
                     [<fixture symbol> <fixture size>]
The optional fixture is a test-only .bss.* input section linked last, so it ends at an odd address.
"""

import sys

from rsp import EFLAGS_DF, EFLAGS_IF, Results, Rsp, read_config, read_map

POISON = 0xA5
CANARY = 0x3C


def main():
    sock, map_path, kernel_path, result_path = sys.argv[1:5]
    fixture = sys.argv[5] if len(sys.argv) > 5 else None
    fixture_size = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0
    res = Results(result_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    symbols, sections = read_map(map_path)
    with open(kernel_path, "rb") as f:
        kernel = f.read()

    start = symbols["_start"]
    kernel_main = symbols["kernel_main"]
    bss_addr, bss_size = sections[".bss"]
    lo, hi = bss_addr, bss_addr + bss_size
    if fixture:
        fix_lo = symbols[fixture]
        fix_hi = fix_lo + fixture_size
        # The fixture must be zeroed with the rest, so poison and check the span covering both.
        lo, hi = min(lo, fix_lo), max(hi, fix_hi)
        res.add("fixture_odd_end", fix_hi % 2 == 1 and fix_hi == hi,
                "fixture %#x-%#x is not the odd-sized end of .bss" % (fix_lo, fix_hi))
    print("  .bss %#x-%#x (%d bytes), poisoned span %#x-%#x" % (bss_addr, bss_addr + bss_size, bss_size, lo, hi),
          flush=True)

    r = Rsp(sock)
    r.add_breakpoint(start)
    # Only the jump from the boot sector counts: kernel code selector, kernel already loaded.
    for _ in range(5):
        eip = r.cont(60)
        if eip == start and r.reg("cs") == config["KERNEL_CODE_SELECTOR"]:
            break
    else:
        raise SystemExit("never reached _start from the boot sector")
    r.remove_breakpoint(start)
    res.add("at_start", True)

    cr = r.control_registers()
    flags = r.reg("eflags")
    res.add("start_paging_off", not cr["cr0"] & 0x80000000, "CR0=%#x" % cr["cr0"])
    res.add("start_interrupts_off", not flags & EFLAGS_IF, "EFLAGS=%#x" % flags)
    res.add("kernel_loaded", r.read(start, len(kernel)) == kernel, "memory at _start differs from kernel.bin")

    if "__bss_start" in symbols and "__bss_end" in symbols:
        b0, b1 = symbols["__bss_start"], symbols["__bss_end"]
        res.add("bss_symbols", (b0, b1) == (lo, hi),
                "__bss_start..__bss_end = %#x..%#x, expected %#x..%#x" % (b0, b1, lo, hi))
    else:
        res.add("bss_symbols", False, "__bss_start/__bss_end not in the map")

    below = r.read(lo - 1, 1)
    above = r.read(hi, 1)
    r.write(lo - 1, bytes([CANARY]))
    r.write(hi, bytes([CANARY]))
    image = r.read(start, lo - 1 - start)        # everything loaded below .bss
    r.write(lo, bytes([POISON]) * (hi - lo))
    r.set_reg("eflags", flags | EFLAGS_DF)

    r.add_breakpoint(kernel_main)
    eip = r.cont(60)
    r.remove_breakpoint(kernel_main)
    res.add("at_kernel_main", eip == kernel_main, "stopped at %#x" % eip)

    bss = r.read(lo, hi - lo)
    dirty = [i for i, b in enumerate(bss) if b != 0]
    res.add("bss_zero", not dirty,
            "%d of %d bytes not zero, first at %#x, last at %#x" %
            (len(dirty), len(bss), lo + dirty[0], lo + dirty[-1]) if dirty else "")
    res.add("canary_below", r.read(lo - 1, 1) == bytes([CANARY]), "byte at %#x changed" % (lo - 1))
    res.add("canary_above", r.read(hi, 1) == bytes([CANARY]), "byte at %#x changed" % hi)
    res.add("image_unchanged", r.read(start, len(image)) == image, "kernel image below .bss changed")
    flags = r.reg("eflags")
    esp = r.reg("esp")
    res.add("main_interrupts_off", not flags & EFLAGS_IF, "EFLAGS=%#x" % flags)
    res.add("main_direction_clear", not flags & EFLAGS_DF, "EFLAGS=%#x (DF set)" % flags)
    res.add("main_boot_stack", esp == config["KERNEL_BOOT_STACK_TOP"] - 4, "ESP=%#x" % esp)

    r.write(lo - 1, below)
    r.write(hi, above)
    r.detach()


if __name__ == "__main__":
    main()
