"""Debugger side of isolation.sh: one fault.elf probe attack per call.

attack <socket> <map> <kernel elf> <offsets> <probe elf> <results> <tag> <op> <target> [key=value...]
    fault.elf (probe or reuse mode) has printed its mailbox address and waits for it. Find the
    probing process by that mailbox, resolve the target, record the target's PDE and PTE in the
    probe's directory, break at the exception stubs and process_terminate, then write the
    operation and target into the mailbox.
    op r/w/x: the probe must take a user #PF on exactly its read, write or call instruction:
    vector 14, error code 0x5 (read, fetch) or 0x7 (write), CR2 = target, saved CS ring 3, the
    faulting task is the probe and CR3 its directory. At that point "info mem" (the CPU's own
    view of the probe's directory) must give ring 3 exactly the probe's stack, ELF segments and
    live allocations, everything else present, supervisor-only and writable. Then
    interrupt_handler gets the frame, process_terminate gets the probe's process with the
    kernel directory loaded (whose "info mem" has no user page at all), returns 0 and the probe's
    slot and task are gone.
    op s: no exception: the probe hands the target to syscalls and exits normally.
    Options: box=<hex> (required), pde_u=0|1 (the target PDE's U bit), pte=supervisor|user-ro,
    owner=1 (the target is a live allocation of another fault.elf process: it stays a user page
    there and its bytes must not change), freed=1 (the target is box[2]: freed by the probe and
    now allocated by the kernel), done=<hex> (write 1 there afterwards: the owner's done word).
    Targets: 0x<hex>, owndir, owntask, shelldir, elfalias, stackalias, freed, afterprog.

Offsets file: "name value" lines (struct offsets, see isolation.sh).
Results: one "PASS <tag>_<id>" / "FAIL <tag>_<id>: ..." line per assertion.
"""

import re
import struct
import sys

from rsp import Results, Rsp, RspError, read_config, read_elf_symbols, read_map

PAGE = 4096
P, W, U = 1, 2, 4
MUST_BE_CLEAR = 0x198      # PWT, PCD, PS, G


def page_down(a):
    return a & ~(PAGE - 1)


def page_up(a):
    return (a + PAGE - 1) & ~(PAGE - 1)


class Attack:
    def __init__(self, args):
        (self.sock, map_path, kernel_elf, offsets_path, probe_elf, result_path, self.tag, self.op,
         self.target_spec) = args[:9]
        self.opts = dict(o.split("=", 1) for o in args[9:])
        self.config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc", "kernel/config.h")
        self.symbols, _ = read_map(map_path)
        self.kernel = read_elf_symbols(kernel_elf)
        self.probe_symbols = read_elf_symbols(probe_elf)
        self.segments = self.load_segments(probe_elf)
        self.off = {}
        with open(offsets_path) as f:
            for line in f:
                name, value = line.split()
                self.off[name] = int(value, 0)
        self.res = Results(result_path)
        self.r = None

    def add(self, ident, ok, detail=""):
        return self.res.add("%s_%s" % (self.tag, ident), ok, detail)

    @staticmethod
    def load_segments(path):
        """PT_LOAD segments of the probe: [(start, end, writable)] in whole pages."""
        with open(path, "rb") as f:
            elf = f.read()
        phoff, = struct.unpack_from("<I", elf, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
        out = []
        for i in range(phnum):
            p_type, _, vaddr, _, _, memsz, flags, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
            if p_type == 1 and memsz:
                out.append((page_down(vaddr), page_up(vaddr + memsz), bool(flags & 2)))
        return out

    # --- kernel structures ---------------------------------------------------------------

    def tasks(self):
        out = []
        t = self.r.read32(self.symbols["task_head"])
        while t and len(out) < 16:
            out.append(t)
            t = self.r.read32(t + self.off["task_next"])
        return out

    def process_of(self, task):
        return self.r.read32(task + self.off["task_process"])

    def directory_of(self, task):
        chunk = self.r.read32(task + self.off["task_page_directory"])
        return self.r.read32(chunk)

    def allocations(self, process):
        n = self.off["max_allocations"]
        size = self.off["allocation_size"]
        data = self.r.read(process + self.off["process_allocations"], n * size)
        out = []
        for i in range(n):
            ptr, length = struct.unpack_from("<II", data, i * size)
            if ptr:
                out.append((ptr, length))
        return out

    def filename(self, process):
        data = self.r.read(process + self.off["process_filename"], 64)
        return data.split(b"\0")[0].decode("latin-1")

    def entries(self, directory, address):
        pde = self.r.read32(directory + (address >> 22) * 4)
        pte = self.r.read32((pde & 0xFFFFF000) + ((address >> 12) & 0x3FF) * 4) if pde & P else None
        return pde, pte

    def stub(self, v):
        return self.symbols["int%d" % v]

    # --- "info mem" ------------------------------------------------------------------------

    def info_mem(self):
        ranges = []
        for line in self.r.monitor("info mem").splitlines():
            m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) [0-9a-f]+ ([u-])r([w-])\s*$", line.strip())
            if m:
                ranges.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3) + "r" + m.group(4)))
        return ranges

    @staticmethod
    def expected_map(user):
        """user: [(start, end, prot)] -> full 4 GiB list with "-rw" gaps, merged like info mem."""
        out = []
        at = 0
        for start, end, prot in sorted(user):
            if start > at:
                out.append([at, start, "-rw"])
            out.append([max(start, at), end, prot])
            at = max(at, end)
        if at < 1 << 32:
            out.append([at, 1 << 32, "-rw"])
        merged = []
        for start, end, prot in out:
            if start >= end:
                continue
            if merged and merged[-1][1] == start and merged[-1][2] == prot:
                merged[-1][1] = end
            else:
                merged.append([start, end, prot])
        return [tuple(x) for x in merged]

    @staticmethod
    def show(ranges):
        return " ".join("%x-%x:%s" % r for r in ranges)

    # --- the attack ----------------------------------------------------------------------

    def find_probe(self, box):
        for t in self.tasks():
            p = self.process_of(t)
            if p and any(ptr == box for ptr, _ in self.allocations(p)):
                return t, p
        return None, None

    def resolve(self, probe_dir, shell_task, box):
        spec = self.target_spec
        top = self.config["RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START"]
        if spec.startswith("0x"):
            return int(spec, 16)
        if spec == "owndir":
            return probe_dir
        if spec == "owntask":
            return self.probe_task
        if spec == "shelldir":
            return self.directory_of(shell_task)
        if spec == "elfalias":
            return self.entries(probe_dir, self.segments[0][0])[1] & 0xFFFFF000
        if spec == "stackalias":
            return self.entries(probe_dir, top - PAGE)[1] & 0xFFFFF000
        if spec == "freed":
            return self.r.read32(box + 8)
        if spec == "afterprog":
            return max(end for _, end, _ in self.segments)
        raise RspError("unknown target %s" % spec)

    def run(self):
        box = int(self.opts["box"], 16)
        top = self.config["RORTHOX_KERNEL_TSS_STACK_TOP"]
        user_cs = self.config["USER_CODE_SEGMENT"]
        self.r = r = Rsp(self.sock)
        tasks = self.tasks()
        self.probe_task, process = self.find_probe(box)
        if not self.add("probe_found", process is not None and self.filename(process).endswith("fault.elf"),
                        "no fault.elf process owns the mailbox %#x (tasks %s)" % (box, [hex(t) for t in tasks])):
            raise SystemExit(self.bail())
        probe_dir = self.directory_of(self.probe_task)
        target = self.resolve(probe_dir, tasks[0], box)
        pde, pte = self.entries(probe_dir, target)
        print("  %s: probe task %#x process %#x directory %#x, target %s = %#x, PDE %#x PTE %s" %
              (self.tag, self.probe_task, process, probe_dir, self.target_spec, target, pde,
               "-" if pte is None else "%#x" % pte), flush=True)

        # The target as the probe's directory maps it.
        want_pte = self.opts.get("pte", "supervisor")
        if want_pte == "supervisor":
            ok = pte is not None and (pte & 0xFFFFF007) == page_down(target) | P | W and not pte & MUST_BE_CLEAR
            self.add("target_pte", ok, "PTE %s, want identity %#x present writable supervisor" %
                     (pte if pte is None else "%#x" % pte, page_down(target)))
        elif want_pte == "user-ro":
            ok = pte is not None and (pte & 7) == P | U and not pte & MUST_BE_CLEAR
            self.add("target_pte", ok, "PTE %s, want present read-only user" % (pte if pte is None else "%#x" % pte))
        pde_ok = pde & (P | W) == P | W and not pde & MUST_BE_CLEAR
        if "pde_u" in self.opts:
            pde_ok = pde_ok and bool(pde & U) == (self.opts["pde_u"] == "1")
        self.add("target_pde", pde_ok, "PDE %#x, want present writable, U=%s" % (pde, self.opts.get("pde_u", "any")))

        before = None
        if self.opts.get("owner") == "1":
            owners = [(t, self.process_of(t)) for t in tasks if t != self.probe_task]
            found = [(t, p) for t, p in owners if p and any(ptr == target for ptr, _ in self.allocations(p))]
            ok = len(found) == 1 and self.filename(found[0][1]).endswith("fault.elf")
            if ok:
                o_pde, o_pte = self.entries(self.directory_of(found[0][0]), target)
                ok = o_pte is not None and (o_pte & 0xFFFFF007) == page_down(target) | P | W | U and o_pde & U
            self.add("owner_live", ok, "target %#x is not a live user page of exactly one other fault.elf" % target)
            before = r.read(page_down(target), PAGE)
        if self.opts.get("freed") == "1":
            table = self.config["RORTHOX_HEAP_TABLE_ADDRESS"]
            block = (target - self.config["RORTHOX_HEAP_ADDRESS"]) // PAGE
            entry = r.read(table + block, 1)[0]
            owners = [t for t in tasks if any(page_down(ptr) <= target < page_up(ptr + n)
                                              for ptr, n in self.allocations(self.process_of(t)))]
            what = ["task %#x" % t for t in tasks if page_down(t) == target]
            for t in tasks:
                d = self.directory_of(t)
                if d == target:
                    what.append("directory of task %#x" % t)
                elif any(e & 0xFFFFF000 == target for e in struct.unpack("<1024I", r.read(d, PAGE))):
                    what.append("page table of task %#x" % t)
            # A user page of another task (its program or stack backing) would not be kernel-only.
            aliases = []
            for t in tasks:
                d = self.directory_of(t)
                for pdi in (0, 1):
                    pde_t = r.read32(d + pdi * 4)
                    for i, e in enumerate(struct.unpack("<1024I", r.read(pde_t & 0xFFFFF000, PAGE))):
                        if e & (P | U) == P | U and e & 0xFFFFF000 == target:
                            aliases.append("task %#x at %#x" % (t, pdi << 22 | i << 12))
            print("  %s: the freed page is now %s" % (self.tag, ", ".join(what) or "other kernel data"), flush=True)
            self.add("freed_reused", entry != 0 and not owners and not aliases,
                     "heap table entry %#x for the freed page %#x, user allocation of tasks %s, user page of %s" %
                     (entry, target, [hex(t) for t in owners], aliases))

        stubs = {self.stub(v): v for v in (6, 13, 14)}
        terminate = self.symbols["process_terminate"]
        for addr in list(stubs) + [terminate]:
            r.add_breakpoint(addr)
        r.write(box + 4, struct.pack("<I", target))
        r.write(box, struct.pack("<I", ord(self.op)))
        try:
            eip = r.cont(30)
        except RspError as e:
            eip = None
            print("  %s: %s" % (self.tag, e), flush=True)

        if self.op == "s":
            arg = r.read32(r.reg("esp") + 4) if eip == terminate else None
            self.add("no_exception", eip == terminate and arg == process,
                     "stopped at %s, process_terminate argument %s" % (self.where(eip, stubs), arg))
            self.after_terminate(r, process)
        else:
            self.fault(r, eip, stubs, target, process, probe_dir, top, user_cs)
        if before is not None:
            self.add("owner_unchanged", r.read(page_down(target), PAGE) == before, "the owner's page changed")
        if "done" in self.opts:
            r.write(int(self.opts["done"], 16), struct.pack("<I", 1))
        r.detach()

    def where(self, eip, stubs):
        if eip is None:
            return "nothing within 30 s"
        if eip in stubs:
            return "int%d" % stubs[eip]
        if eip == self.symbols["process_terminate"]:
            return "process_terminate (no exception: the access went through)"
        return "%#x" % eip

    def fault(self, r, eip, stubs, target, process, probe_dir, top, user_cs):
        want_error = 0x7 if self.op == "w" else 0x5
        if not self.add("stub_vector", stubs.get(eip) == 14, "stopped at %s, expected int14" % self.where(eip, stubs)):
            if eip == self.symbols["process_terminate"]:
                self.after_terminate(r, process)   # the probe went on and exited
            return
        r.remove_breakpoint(eip)
        esp = r.reg("esp")
        frame_bytes = r.read(esp, 24)
        error, f_eip, f_cs, f_flags, f_esp, f_ss = struct.unpack("<6I", frame_bytes)
        cr = r.control_registers()
        print("  %s: #PF error %#x eip %#x cs %#x esp %#x cr2 %#x cr3 %#x" %
              (self.tag, error, f_eip, f_cs, f_esp, cr["cr2"], cr["cr3"]), flush=True)
        self.add("error_code", error == want_error, "error %#x, expected %#x" % (error, want_error))
        self.add("cr2", cr["cr2"] == target, "cr2 %#x, expected %#x" % (cr["cr2"], target))
        self.add("cpl3", f_cs == user_cs and f_cs & 3 == 3 and esp == top - 24,
                 "cs %#x, kernel esp %#x (want %#x)" % (f_cs, esp, top - 24))
        current = r.read32(self.symbols["current_task"])
        self.add("faulting_task", current == self.probe_task and cr["cr3"] == probe_dir,
                 "current_task %#x cr3 %#x, probe task %#x directory %#x" % (current, cr["cr3"], self.probe_task, probe_dir))
        syms = self.probe_symbols
        if self.op == "r":
            ok = f_eip == syms["fault_read"] + 4 and r.read(f_eip, 2) == b"\x8b\x00"
        elif self.op == "w":
            ok = f_eip == syms["fault_write"] + 4 and r.read(f_eip, 6) == b"\xc7\x00\x5a\x5a\x5a\x5a"
        else:
            ok = f_eip == target and r.read32(f_esp) == syms["fault_exec"] + 6
        self.add("instruction", ok, "saved eip %#x (fault_read %#x, fault_write %#x, fault_exec %#x)" %
                 (f_eip, syms["fault_read"], syms["fault_write"], syms["fault_exec"]))

        # The probe's whole address space as the CPU sees it.
        stack_top = self.config["RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START"]
        user = [(stack_top - self.config["USER_PROGRAM_STACK_SIZE"], stack_top, "urw")]
        user += [(s, e, "urw" if w else "ur-") for s, e, w in self.segments]
        user += [(page_down(p), page_up(p + n), "urw") for p, n in self.allocations(process)]
        want = self.expected_map(user)
        got = self.info_mem()
        self.add("user_map", got == want, "info mem %s, expected %s" % (self.show(got), self.show(want)))

        r.add_breakpoint(self.symbols["interrupt_handler"])
        hit = r.cont(10)
        r.remove_breakpoint(self.symbols["interrupt_handler"])
        bad = []
        if hit != self.symbols["interrupt_handler"]:
            bad.append("stopped at %s" % self.where(hit, stubs))
        else:
            h_esp = r.reg("esp")
            vector, frame, h_cr2 = struct.unpack("<III", r.read(h_esp + 4, 12))
            words = struct.unpack("<12I", r.read(frame, 48))
            if vector != 14 or frame != esp - 32 or h_cr2 != target or words[8] != error or words[9:12] != (f_eip, f_cs, f_flags):
                bad.append("vector %d frame %#x cr2 %#x words %s" % (vector, frame, h_cr2, [hex(w) for w in words[8:12]]))
        self.add("handler_frame", not bad, "; ".join(bad))

        eip = r.cont(15)
        arg = r.read32(r.reg("esp") + 4) if eip == self.symbols["process_terminate"] else None
        same = r.read(esp, 24) == frame_bytes
        self.add("terminated", eip == self.symbols["process_terminate"] and arg == process and same,
                 "stopped at %s, argument %s, probe process %#x, %s" %
                 (self.where(eip, stubs), arg, process, "from the exception" if same else "not from the exception"))
        if eip != self.symbols["process_terminate"]:
            return
        self.after_terminate(r, process)

    def after_terminate(self, r, process):
        """At process_terminate: kernel directory loaded and without user pages; then the process goes."""
        terminate = self.symbols["process_terminate"]
        if r.reg("eip") != terminate:
            return
        kernel_dir = r.read32(r.read32(self.kernel["kernel_chunk"]))
        cr3 = r.control_registers()["cr3"]
        got = self.info_mem()
        want = [(0, 1 << 32, "-rw")]
        self.add("kernel_map", cr3 == kernel_dir and got == want,
                 "cr3 %#x (kernel directory %#x), info mem %s" % (cr3, kernel_dir, self.show(got)))
        ret = r.read32(r.reg("esp"))
        r.remove_breakpoint(terminate)
        r.add_breakpoint(ret)
        eip = r.cont(15)
        r.remove_breakpoint(ret)
        slots = struct.unpack("<12I", r.read(self.kernel["processes"], 48))
        gone = eip == ret and r.reg("eax") == 0 and process not in slots and self.probe_task not in self.tasks()
        self.add("slot_cleared", gone, "returned to %s with %#x, slots %s" %
                 (self.where(eip, {}), r.reg("eax"), [hex(s) for s in slots if s]))

    def bail(self):
        try:
            self.r.detach()
        except Exception:
            pass
        return 1


def main():
    mode, args = sys.argv[1], sys.argv[2:]
    if mode != "attack":
        raise SystemExit("unknown mode %s" % mode)
    a = Attack(args)
    try:
        a.run()
    except (RspError, OSError, KeyError) as e:
        a.add("session", False, str(e))
        raise SystemExit(a.bail() if a.r else 1)


if __name__ == "__main__":
    main()
