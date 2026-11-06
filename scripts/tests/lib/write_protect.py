"""Debugger side of write-protect.sh.

boot <socket> <map> <results> disk|grub <boot timeout>
    QEMU waits at reset. Follow the boot to kernel_main and record which entry ran: disk = the boot
    sector's jump to _start and never multiboot_entry; grub = multiboot_entry with the Multiboot
    magic in EAX, then kernel_main without passing _start.
wp <socket> <map> <shell elf> <results> <tag>
    The shell is idle in its getkey syscall. Stop at isr80h_handler (ring 0, the shell's directory
    still loaded, before kernel_page()), record CR0 and CR4, find the shell's first read-only
    PT_LOAD page, check its PDE/PTE, and put "mov [dest], eax" (A3 imm32) at isr80h_handler, so
    the CPU itself stores to that page from ring 0. With CR0.WP set this must be a #PF: error 0x3
    (present, write, supervisor), eip = isr80h_handler, cs = the kernel's, CR2 = dest, then
    PANIC("Kernel exception") and PAUSE with the bytes unchanged. A breakpoint right after the
    store catches a kernel without WP at once.

Writes by the debugger itself ignore page permissions, so only the CPU's own store counts.
"""

import re
import struct
import sys

from rsp import Results, Rsp, RspError, read_config, read_map

CR0_PG = 0x80000000
CR0_WP = 0x00010000
CR4_PGE = 1 << 7
CR4_SMEP = 1 << 20
CR4_SMAP = 1 << 21
PF_W = 2
PT_LOAD = 1
MULTIBOOT_MAGIC = 0x2BADB002

RES = None
CONNECTION = None


def bail():
    try:
        CONNECTION.detach()
    except Exception:
        pass
    raise SystemExit(1)


def where(eip):
    return "nothing within the timeout" if eip is None else "%#x" % eip


def run_to(r, timeout):
    try:
        return r.cont(timeout)
    except RspError as e:
        if "no breakpoint hit" in str(e):
            return None
        raise


def read_string(r, addr, limit=128):
    return r.read(addr, limit).split(b"\0")[0].decode("latin-1")


def read_only_segment(path):
    """(vaddr, file bytes at vaddr) of the first PT_LOAD without PF_W and with file bytes."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:6] != b"\x7fELF\x01\x01":
        raise RspError("%s is not a 32-bit little-endian ELF file" % path)
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if p_type == PT_LOAD and not p_flags & PF_W and p_filesz >= 4:
            return p_vaddr, elf[p_offset:p_offset + 4]
    raise RspError("%s has no read-only PT_LOAD with file bytes" % path)


def boot_mode(sock, map_path, result_path, path, boot_timeout):
    global RES, CONNECTION
    RES = Results(result_path)
    symbols, _ = read_map(map_path)
    entry, start, kmain = symbols["multiboot_entry"], symbols["_start"], symbols["kernel_main"]
    names = {entry: "multiboot_entry", start: "_start", kmain: "kernel_main"}
    try:
        r = CONNECTION = Rsp(sock)
        if path == "grub":
            # GRUB runs code around 1 MiB before it loads the kernel: watch only its handoff first.
            r.add_breakpoint(entry)
            eip = run_to(r, int(boot_timeout))
            eax = r.reg("eax") if eip == entry else None
            RES.add("grub_multiboot_entry", eip == entry and eax == MULTIBOOT_MAGIC,
                    "stopped at %s, eax %s" % (names.get(eip, where(eip)), "-" if eax is None else "%#x" % eax))
            if eip != entry:
                bail()
            r.add_breakpoint(start)
            r.add_breakpoint(kmain)
            eip = run_to(r, 20)
            RES.add("grub_kernel_main", eip == kmain, "stopped at %s" % names.get(eip, where(eip)))
        else:
            r.add_breakpoint(entry)
            r.add_breakpoint(start)
            eip = run_to(r, int(boot_timeout))
            RES.add("disk_start", eip == start, "stopped at %s" % names.get(eip, where(eip)))
            if eip != start:
                bail()
            r.add_breakpoint(kmain)
            eip = run_to(r, 20)
            RES.add("disk_kernel_main", eip == kmain, "stopped at %s" % names.get(eip, where(eip)))
        r.detach()
    except (RspError, OSError) as e:
        RES.add("session", False, str(e))
        bail()


def wp_mode(sock, map_path, shell_path, result_path, tag):
    global RES, CONNECTION
    RES = Results(result_path)
    symbols, _ = read_map(map_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    kernel_cs = config["KERNEL_CODE_SELECTOR"]
    handler = symbols["isr80h_handler"]
    stub14 = symbols["int14"]
    after = handler + 5                  # the instruction after the injected 5-byte store
    dest, file_bytes = read_only_segment(shell_path)
    try:
        r = CONNECTION = Rsp(sock)
        r.add_breakpoint(handler)
        if run_to(r, 20) != handler:
            raise RspError("no syscall reached isr80h_handler")
        r.remove_breakpoint(handler)
        cr = r.control_registers()
        cr0, cr3, cr4 = cr["cr0"], cr["cr3"], cr["cr4"]
        print("  %s: cr0 %#010x cr3 %#010x cr4 %#010x" % (tag, cr0, cr3, cr4), flush=True)
        RES.add("cr0_pg_wp", cr0 & (CR0_PG | CR0_WP) == CR0_PG | CR0_WP, "cr0 %#x" % cr0)
        RES.add("cr4_no_pge_smep_smap", not cr4 & (CR4_PGE | CR4_SMEP | CR4_SMAP), "cr4 %#x" % cr4)

        # Ring 0 with the shell's own directory loaded (kernel_page() has not run yet).
        task = r.read32(symbols["current_task"])
        directory = r.read32(r.read32(task + 4)) if task else 0       # task->page_directory->directory_entry
        process = r.read32(task + 56) if task else 0                  # task->process
        name = read_string(r, process + 2) if process else ""         # process->filename
        RES.add("in_shell_syscall", r.reg("cs") == kernel_cs and task and cr3 == directory and
                name.endswith("shell.elf"),
                "cs %#x cr3 %#x task directory %#x process %r" % (r.reg("cs"), cr3, directory, name))

        pde = r.read32(cr3 + (dest >> 22) * 4)
        pte_at = (pde & ~0xFFF) + ((dest >> 12) & 0x3FF) * 4
        pte = r.read32(pte_at) if pde & 1 else 0
        phys = (pte & ~0xFFF) | (dest & 0xFFF)
        before = r.read(dest, 4)
        RES.add("read_only_user_page", pde & 7 == 7 and pte & 7 == 5 and before == file_bytes,
                "dest %#x pde %#x pte %#x bytes %s, file %s" % (dest, pde, pte, before.hex(), file_bytes.hex()))

        # The CPU's own store: mov [dest], eax with eax != the current bytes.
        value = struct.unpack("<I", before)[0] ^ 0xFFFFFFFF
        r.set_reg("eax", value)
        r.write(handler, b"\xa3" + struct.pack("<I", dest))
        stops = {stub14: "int14", symbols["interrupt_handler"]: "interrupt_handler",
                 symbols["PANIC"]: "PANIC", symbols["PAUSE"]: "PAUSE", after: "after the store"}
        for addr in stops:
            r.add_breakpoint(addr)

        eip = run_to(r, 10)
        if eip == after:
            now = r.read(dest, 4)
            RES.add("store_faulted", False, "the store went through: no #PF, %#x now holds %s (was %s)" %
                    (dest, now.hex(), before.hex()))
            bail()
        frame = struct.unpack("<4I", r.read(r.reg("esp"), 16))
        cr2 = r.control_registers()["cr2"]
        RES.add("store_faulted", eip == stub14, "stopped at %s" % stops.get(eip, where(eip)))
        if eip != stub14:
            bail()
        RES.add("pf_frame", frame[0] == 0x3 and frame[1] == handler and frame[2] == kernel_cs,
                "error %#x eip %#x cs %#x (want 0x3 %#x %#x)" % (frame[0], frame[1], frame[2], handler, kernel_cs))
        RES.add("pf_cr2", cr2 == dest, "cr2 %#x, dest %#x" % (cr2, dest))

        esp = r.reg("esp")
        eip = run_to(r, 10)
        if eip == symbols["interrupt_handler"]:
            sp = r.reg("esp")
            vector, f, h_cr2 = struct.unpack("<III", r.read(sp + 4, 12))
            words = struct.unpack("<12I", r.read(f, 48))
            RES.add("handler_frame", vector == 14 and f == esp - 32 and h_cr2 == dest and
                    words[8] == 0x3 and words[9] == handler,
                    "vector %d frame %#x (want %#x) cr2 %#x error %#x ip %#x" %
                    (vector, f, esp - 32, h_cr2, words[8], words[9]))
            eip = run_to(r, 10)
        else:
            RES.add("handler_frame", False, "interrupt_handler not reached, stopped at %s" % stops.get(eip, where(eip)))
        msg = read_string(r, r.read32(r.reg("esp") + 4)) if eip == symbols["PANIC"] else None
        RES.add("panic", msg == "Kernel exception", "stopped at %s, message %r" % (stops.get(eip, where(eip)), msg))
        if msg is None:
            bail()
        eip = run_to(r, 10)
        RES.add("halted_in_pause", eip == symbols["PAUSE"], "stopped at %s" % stops.get(eip, where(eip)))

        # Nothing was written: through the shell's mapping and at the physical page behind it.
        now = r.read(dest, 4)
        xp = r.monitor("xp /1wx %#x" % phys)
        m = re.search(r":\s*0x([0-9a-fA-F]+)", xp)
        physical = struct.pack("<I", int(m.group(1), 16)) if m else None
        RES.add("bytes_unchanged", now == before and physical == before,
                "virtual %s, physical %#x %s, before %s" % (now.hex(), phys, physical.hex() if physical else xp.strip(),
                                                            before.hex()))
        RES.add("cr3_untouched", r.control_registers()["cr3"] == cr3, "cr3 changed from %#x" % cr3)
        print("  %s: store to %#x (physical %#x) faulted, error 0x3" % (tag, dest, phys), flush=True)
        r.detach()
    except (RspError, OSError) as e:
        RES.add("session", False, str(e))
        bail()


if __name__ == "__main__":
    mode, args = sys.argv[1], sys.argv[2:]
    {"boot": boot_mode, "wp": wp_mode}[mode](*args)
