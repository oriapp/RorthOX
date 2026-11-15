"""Debugger side of null-page.sh.

boot <socket> <map> <results> <directory file> disk|grub <boot timeout>
    QEMU waits at reset. Follow the boot to kernel_main (disk: through the boot sector's jump to
    _start, never multiboot_entry; grub: multiboot_entry with the Multiboot magic, never _start),
    then to enable_paging: paging is still off and CR3 holds the kernel directory that
    paging_new_4gb built, so page zero must already be absent there (PDE 0 present, PTE 0 = 0).
    The directory's address goes to <directory file>.
access <socket> <map> <directory file> <results> <tag>
    The shell is idle in its getkey syscall. Stop at isr80h_handler (ring 0, the shell's directory
    still loaded) and check the production mapping as the CPU walks it: PTE 0 is 0 under a
    present PDE 0 in the shell's directory and in the kernel directory, the other pages of the
    first 4 MiB are identity, present, writable and supervisor-only (the user stack's pages
    aside), QEMU's own walk can't read address 0 and can read 0x1000. Then the CPU itself loads
    from and stores to address 0 in ring 0, four times:
      task_load, task_store      injected at isr80h_handler, the shell's directory loaded
      kernel_load, kernel_store  injected right after kernel_page() returned in isr80h_handler,
                                 the kernel directory loaded
    "mov eax, [0]" (A1 00000000) and "mov [0], eax" (A3 00000000). Before each one the debugger
    sets CR2 to 0x5A5A5A5A with an injected "mov cr2, eax" (0F 22 D0) and stops right after it (CR2
    is 0 from reset until the first #PF, so without this a fault that didn't write CR2 would still
    show 0). Each access must give #PF (the int14
    stub): error 0x0 for the load, 0x2 for the store (not present, supervisor), the saved eip is
    the injected instruction, cs the kernel's, CR2 = 0 and CR3 the directory it ran under. The
    first three are rewound at the stub (esp, eflags, eip and eax as before the access: the stub
    has run no instruction yet); the last one goes on to interrupt_handler (vector 14, error 0x2,
    CR2 0), PANIC("Kernel exception") and PAUSE. Physical page zero is unchanged at the end. A
    breakpoint after each access reports at once when it completes (no #PF).

Reads and writes by the debugger itself ignore page permissions: only the CPU's own accesses count.
At most four hardware breakpoints are set at a time (KVM), and nothing is single-stepped after eip
was moved.
"""

import re
import struct
import sys

from rsp import Results, Rsp, RspError, read_config, read_map

CR0_PG = 0x80000000
P, W, U = 1, 2, 4
PAGE = 4096
MULTIBOOT_MAGIC = 0x2BADB002
LOAD = b"\xa1\x00\x00\x00\x00"     # mov eax, [0]
STORE = b"\xa3\x00\x00\x00\x00"    # mov [0], eax
SEED = b"\x0f\x22\xd0"               # mov cr2, eax
MARK = 0x5A5A5A5A                    # eax for every access, and the CR2 value it seeds
ERRORS = {"load": 0x0, "store": 0x2}

RES = None
CONNECTION = None


def bail():
    try:
        CONNECTION.detach()
    except Exception:
        pass
    raise SystemExit(1)


def where(eip, names):
    if eip is None:
        return "nothing within the timeout"
    return names.get(eip, "%#x" % eip)


def run_to(r, timeout):
    try:
        return r.cont(timeout)
    except RspError as e:
        if "no breakpoint hit" in str(e):
            return None
        raise


def read_string(r, addr, limit=128):
    return r.read(addr, limit).split(b"\0")[0].decode("latin-1")


def first_table(r, directory):
    """(PDE 0, the 1024 PTEs of its table, or None when PDE 0 is not present)."""
    pde = r.read32(directory)
    if not pde & P:
        return pde, None
    return pde, struct.unpack("<1024I", r.read(pde & ~0xFFF, PAGE))


def low_pages_bad(ptes, skip=()):
    """Pages 1..1023 of the first table that are not identity, present, writable, supervisor."""
    bad = []
    for i in range(1, 1024):
        if i in skip:
            continue
        if ptes[i] & 0xFFFFF007 != (i << 12) | P | W:
            bad.append("%#x: %#x" % (i << 12, ptes[i]))
    return bad


def check_directory(ident, r, directory, skip=()):
    pde, ptes = first_table(r, directory)
    pte0 = None if ptes is None else ptes[0]
    RES.add(ident + "_page_zero", ptes is not None and pte0 == 0,
            "directory %#x PDE 0 %#x PTE 0 %s" % (directory, pde, "-" if pte0 is None else "%#x" % pte0))
    if ptes is None:
        RES.add(ident + "_neighbours", False, "PDE 0 %#x is not present: no low page is mapped" % pde)
        return
    bad = low_pages_bad(ptes, skip)
    RES.add(ident + "_neighbours", not bad and pde & (P | W) == P | W,
            "PDE 0 %#x; %d pages wrong: %s; PTE 1 %#x, 0x7000 %#x, 0xb8000 %#x" %
            (pde, len(bad), " ".join(bad[:4]), ptes[1], ptes[7], ptes[0xB8]))


def boot_mode(sock, map_path, result_path, directory_path, path, boot_timeout):
    global RES, CONNECTION
    RES = Results(result_path)
    symbols, _ = read_map(map_path)
    entry, start, kmain = symbols["multiboot_entry"], symbols["_start"], symbols["kernel_main"]
    enable = symbols["enable_paging"]
    names = {entry: "multiboot_entry", start: "_start", kmain: "kernel_main", enable: "enable_paging"}
    try:
        r = CONNECTION = Rsp(sock)
        if path == "grub":
            # GRUB runs code around 1 MiB before it loads the kernel: watch only its handoff first.
            r.add_breakpoint(entry)
            eip = run_to(r, int(boot_timeout))
            eax = r.reg("eax") if eip == entry else None
            RES.add("grub_multiboot_entry", eip == entry and eax == MULTIBOOT_MAGIC,
                    "stopped at %s, eax %s" % (where(eip, names), "-" if eax is None else "%#x" % eax))
            if eip != entry:
                bail()
            r.add_breakpoint(start)
            r.add_breakpoint(kmain)
            eip = run_to(r, 20)
            RES.add("grub_kernel_main", eip == kmain, "stopped at %s" % where(eip, names))
        else:
            r.add_breakpoint(entry)
            r.add_breakpoint(start)
            eip = run_to(r, int(boot_timeout))
            RES.add("disk_start", eip == start, "stopped at %s" % where(eip, names))
            if eip != start:
                bail()
            r.add_breakpoint(kmain)
            eip = run_to(r, 20)
            RES.add("disk_kernel_main", eip == kmain, "stopped at %s" % where(eip, names))
        if eip != kmain:
            bail()
        for addr in list(r.breakpoints):
            r.remove_breakpoint(addr)
        r.add_breakpoint(enable)
        eip = run_to(r, 20)
        cr = r.control_registers() if eip == enable else None
        RES.add("before_paging", eip == enable and not cr["cr0"] & CR0_PG and cr["cr3"],
                "stopped at %s, cr0 %s cr3 %s" % (where(eip, names), "-" if cr is None else "%#x" % cr["cr0"],
                                                   "-" if cr is None else "%#x" % cr["cr3"]))
        if eip != enable:
            bail()
        # Paging is off: the debugger reads physical memory, the tables as paging_new_4gb left them.
        directory = cr["cr3"] & ~0xFFF
        check_directory("built", r, directory)
        with open(directory_path, "w") as f:
            f.write("%#x\n" % directory)
        r.remove_breakpoint(enable)
        r.detach()
    except (RspError, OSError) as e:
        RES.add("session", False, str(e))
        bail()


class Access:
    def __init__(self, r, symbols, kernel_cs):
        self.r = r
        self.symbols = symbols
        self.kernel_cs = kernel_cs
        self.stub14 = symbols["int14"]
        self.names = {self.stub14: "int14", symbols["PANIC"]: "PANIC", symbols["PAUSE"]: "PAUSE",
                      symbols["interrupt_handler"]: "interrupt_handler"}

    def run(self, ident, kind, at, directory, last):
        """The CPU runs SEED at `at`, then LOAD or STORE right after it. Returns the stub's esp for
        the last access when it faulted as expected, else None (after rewinding the others)."""
        r = self.r
        saved = {name: r.reg(name) for name in ("eax", "esp", "eflags", "eip")}
        code = LOAD if kind == "load" else STORE
        access = at + len(SEED)
        original = r.read(at, len(SEED) + len(code))
        r.set_reg("eax", MARK)
        r.write(at, SEED)
        r.add_breakpoint(access)
        eip = run_to(r, 10)
        r.remove_breakpoint(access)
        cr2 = r.control_registers()["cr2"] if eip == access else None
        RES.add(ident + "_cr2_seeded", eip == access and cr2 == MARK,
                "stopped at %s, cr2 %s" % (where(eip, self.names), "-" if cr2 is None else "%#x" % cr2))
        if eip != access:
            bail()
        # The breakpoint is gone, so the next continue starts here without a step.
        r.write(access, code)
        after = access + len(code)
        self.names[after] = "after the %s" % kind
        r.add_breakpoint(self.stub14)
        r.add_breakpoint(after)
        eip = run_to(r, 10)
        r.remove_breakpoint(self.stub14)
        r.remove_breakpoint(after)
        if eip == after:
            RES.add(ident + "_faulted", False, "the %s through address 0 went through: no #PF%s" %
                    (kind, ", eax %#x" % r.reg("eax") if kind == "load" else ""))
        else:
            RES.add(ident + "_faulted", eip == self.stub14, "stopped at %s" % where(eip, self.names))
        if eip not in (self.stub14, after):
            bail()
        cr = r.control_registers()
        if eip == self.stub14:
            esp = r.reg("esp")
            error, f_eip, f_cs, f_flags = struct.unpack("<4I", r.read(esp, 16))
            try:
                insn = r.read(f_eip, len(code))
            except RspError:
                insn = b""    # the saved eip is not readable: the frame is wrong
            RES.add(ident + "_frame", error == ERRORS[kind] and f_eip == access and f_cs == self.kernel_cs and
                    esp == saved["esp"] - 16 and insn == code,
                    "error %#x eip %#x cs %#x esp %#x, instruction %s (want %#x %#x %#x %#x)" %
                    (error, f_eip, f_cs, esp, insn.hex() or "unreadable", ERRORS[kind], access, self.kernel_cs,
                     saved["esp"] - 16))
            RES.add(ident + "_cr2", cr["cr2"] == 0, "cr2 %#x (%#x before the access)" % (cr["cr2"], MARK))
        RES.add(ident + "_cr3", cr["cr3"] == directory, "cr3 %#x, expected %#x" % (cr["cr3"], directory))
        if last and eip == self.stub14:
            return esp
        # Back to the state before the access: the stub ran nothing yet, so only the CPU's push of
        # error code, eip, cs and eflags (and its eflags changes) are undone. CR2 stays 0: the
        # kernel reads it only in the stubs, right after a #PF wrote it.
        r.set_reg("esp", saved["esp"])
        r.set_reg("eflags", saved["eflags"])
        r.set_reg("eax", saved["eax"])
        r.set_reg("eip", at)
        r.write(at, original)
        return None


def access_mode(sock, map_path, directory_path, result_path, tag):
    global RES, CONNECTION
    RES = Results(result_path)
    symbols, _ = read_map(map_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    kernel_cs = config["KERNEL_CODE_SELECTOR"]
    stack_top = config["RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START"]
    stack_pages = range((stack_top - config["USER_PROGRAM_STACK_SIZE"]) >> 12, stack_top >> 12)
    with open(directory_path) as f:
        kernel_dir = int(f.read().strip(), 16)
    handler = symbols["isr80h_handler"]
    try:
        r = CONNECTION = Rsp(sock)
        r.add_breakpoint(handler)
        if run_to(r, 20) != handler:
            raise RspError("no syscall reached isr80h_handler")
        r.remove_breakpoint(handler)
        cr3 = r.control_registers()["cr3"]
        task = r.read32(symbols["current_task"])
        directory = r.read32(r.read32(task + 4)) if task else 0       # task->page_directory->directory_entry
        process = r.read32(task + 56) if task else 0                  # task->process
        name = read_string(r, process + 2) if process else ""         # process->filename
        RES.add("in_shell_syscall", r.reg("cs") == kernel_cs and task and cr3 == directory and
                cr3 != kernel_dir and name.endswith("shell.elf"),
                "cs %#x cr3 %#x task directory %#x kernel directory %#x process %r" %
                (r.reg("cs"), cr3, directory, kernel_dir, name))

        # The production mapping, in both directory classes.
        check_directory("task", r, cr3, skip=stack_pages)
        check_directory("kernel", r, kernel_dir)
        zero = r.monitor("x /1wx 0")
        one = r.monitor("x /1wx 0x1000")
        RES.add("cpu_walk", "Cannot access memory" in zero and re.match(r"^0*1000:", one.strip()) is not None,
                "x 0: %r, x 0x1000: %r" % (zero.strip(), one.strip()))
        m = re.search(r":\s*0x([0-9a-fA-F]+)", r.monitor("xp /1wx 0"))
        physical_before = m.group(1) if m else None
        print("  %s: shell directory %#x, kernel directory %#x, physical 0 holds %s" %
              (tag, cr3, kernel_dir, physical_before), flush=True)

        a = Access(r, symbols, kernel_cs)
        a.run("task_load", "load", handler, cr3, False)
        a.run("task_store", "store", handler, cr3, False)

        # Let isr80h_handler select the kernel directory itself: stop where kernel_page() returns.
        r.add_breakpoint(symbols["kernel_page"])
        eip = run_to(r, 10)
        if eip != symbols["kernel_page"]:
            RES.add("kernel_cr3", False, "kernel_page not reached, stopped at %s" % where(eip, a.names))
            bail()
        r.remove_breakpoint(eip)
        back = r.read32(r.reg("esp"))
        r.add_breakpoint(back)
        eip = run_to(r, 10)
        r.remove_breakpoint(back)
        cr3_now = r.control_registers()["cr3"] if eip == back else None
        RES.add("kernel_cr3", eip == back and handler < back < handler + 0x40 and cr3_now == kernel_dir,
                "returned to %s (isr80h_handler %#x), cr3 %s, kernel directory %#x" %
                (where(eip, a.names), handler, "-" if cr3_now is None else "%#x" % cr3_now, kernel_dir))
        if eip != back:
            bail()
        a.run("kernel_load", "load", back, kernel_dir, False)
        esp = a.run("kernel_store", "store", back, kernel_dir, True)
        if esp is None:
            bail()

        stops = [symbols["interrupt_handler"], symbols["PANIC"], symbols["PAUSE"]]
        for addr in stops:
            r.add_breakpoint(addr)
        eip = run_to(r, 10)
        if eip == symbols["interrupt_handler"]:
            sp = r.reg("esp")
            vector, f, h_cr2 = struct.unpack("<III", r.read(sp + 4, 12))
            words = struct.unpack("<12I", r.read(f, 48))
            RES.add("handler_frame", vector == 14 and f == esp - 32 and h_cr2 == 0 and
                    words[8] == 0x2 and words[9] == back + len(SEED),
                    "vector %d frame %#x (want %#x) cr2 %#x error %#x ip %#x (want %#x)" %
                    (vector, f, esp - 32, h_cr2, words[8], words[9], back + len(SEED)))
            eip = run_to(r, 10)
        else:
            RES.add("handler_frame", False, "interrupt_handler not reached, stopped at %s" % where(eip, a.names))
        msg = read_string(r, r.read32(r.reg("esp") + 4)) if eip == symbols["PANIC"] else None
        RES.add("panic", msg == "Kernel exception", "stopped at %s, message %r" % (where(eip, a.names), msg))
        if msg is None:
            bail()
        eip = run_to(r, 10)
        RES.add("halted_in_pause", eip == symbols["PAUSE"], "stopped at %s" % where(eip, a.names))

        m = re.search(r":\s*0x([0-9a-fA-F]+)", r.monitor("xp /1wx 0"))
        physical_after = m.group(1) if m else None
        RES.add("page_zero_unchanged", physical_before is not None and physical_after == physical_before,
                "physical 0 held %s, now %s" % (physical_before, physical_after))
        RES.add("cr3_kernel", r.control_registers()["cr3"] == kernel_dir, "cr3 is not the kernel directory")
        r.detach()
    except (RspError, OSError) as e:
        RES.add("session", False, str(e))
        bail()


if __name__ == "__main__":
    mode, args = sys.argv[1], sys.argv[2:]
    {"boot": boot_mode, "access": access_mode}[mode](*args)
