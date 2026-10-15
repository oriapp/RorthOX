"""Debugger side of exceptions.sh, kernel-exceptions.sh and exception-frames.sh.

Every mode connects to QEMU's gdbstub, writes one "PASS <id>" / "FAIL <id>: ..." line per
assertion into a results file (the bash test checks the ids) and detaches, so the guest runs on.

Interrupt frame the C code must get (struct interrupt_frame), from the frame pointer up:
    0 edi, 4 esi, 8 ebp, 12 (esp from pushad), 16 ebx, 20 edx, 24 ecx, 28 eax,
    32 error code (the CPU's, or 0), 36 eip, 40 cs, 44 eflags, 48 esp and 52 ss (from ring 3 only)
interrupt_handler(int vector, struct interrupt_frame *frame, uint32_t cr2)
isr80h_handler(int command, struct interrupt_frame *frame)

catch <socket> <map> <results> <ready file> <case>
    The guest runs a fault.elf case and waits for its key. Break at the exception stubs, write the
    ready file, then (after the script's key) check the CPU's own frame at the stub, the frame
    interrupt_handler gets, and what follows: process_terminate on the faulting task's process for
    a ring-3 fault, PANIC("Kernel exception") and PAUSE for a kernel fault or #DF.
list <socket> <map> <kernel elf> <results> <tag> <tasks> [save=<file>|shell=<file>] [owner=first|last]
    Walk the task list and the process slots and check that they are consistent.
layout <socket> <map> <results>
    QEMU waits at reset. At the first task_return, check every IDT gate, then enter each of the
    256 stubs and the syscall stub with a frame laid out like the CPU's (error code only for the
    error-code vectors) and check what the C code gets. Three entries also run back out (iretd).
early <socket> <map> <results> [nested]
    QEMU waits at reset. Right after idt_load, put a ud2 in the kernel's path (ring 0, no task yet):
    exception stub, PANIC("Kernel exception"), PAUSE, CR3 untouched. "nested": also put a ud2 at
    PANIC's first instruction; the second fault must go straight to PAUSE.
nmi <socket> <map> <results>
    The kernel is halted after a panic. Inject an NMI: it must enter the NMI stub and go straight
    back to PAUSE, with no other exception and no PANIC.
"""

import struct
import sys

from rsp import EFLAGS_IF, Results, Rsp, RspError, read_config, read_elf_symbols, read_map

ERROR_VECTORS = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30}
CONFIG = None
SYMBOLS = None
RES = None


def setup(map_path, result_path):
    global CONFIG, SYMBOLS, RES
    CONFIG = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    SYMBOLS, _ = read_map(map_path)
    RES = Results(result_path)


def stub(v):
    return SYMBOLS["int%d" % v]


def read_string(r, addr, limit=128):
    data = r.read(addr, limit)
    return data.split(b"\0")[0].decode("latin-1")


CONNECTION = None


def connect(sock):
    global CONNECTION
    CONNECTION = Rsp(sock)
    return CONNECTION


def bail():
    """Stop after a failed assertion: let the guest run on without the debugger and exit 1."""
    try:
        CONNECTION.detach()
    except Exception:
        pass
    raise SystemExit(1)


def fail_session(e):
    RES.add("session", False, str(e))
    bail()


def run_to(r, stops, timeout):
    """Continue until one of <stops> (addresses); returns its address or None on timeout."""
    try:
        eip = r.cont(timeout)
    except RspError as e:
        if "no breakpoint hit" in str(e):
            return None
        raise
    return eip


def where(eip):
    return "nothing within the timeout" if eip is None else "%#x" % eip


def handler_frame(r):
    """At the entry of interrupt_handler: (vector, frame address, cr2 argument, frame words)."""
    esp = r.reg("esp")
    vector, frame, cr2 = struct.unpack("<III", r.read(esp + 4, 12))
    words = struct.unpack("<12I", r.read(frame, 48))
    return vector, frame, cr2, words


# --- catch -------------------------------------------------------------------------------------

CASES = {
    #        stubs watched   vector error  ring insn at eip
    "ud2":   ([6],           6,    None,   3,   b"\x0f\x0b"),
    "int3":  ([3],           3,    None,   3,   b"\xcc"),
    "cli":   ([13],          13,   0,      3,   b"\xfa"),
    "pf":    ([14],          14,   0x4,    3,   b"\x8b\x00"),
    "int14": ([13, 14],      13,   0x72,   3,   b"\xcd\x0e"),
    "int18": ([18],          18,   None,   3,   b"\xcd\x12"),
    "in":    ([13],          13,   0,      3,   b"\xe4\x80"),   # in al, 0x80
    "out":   ([13],          13,   0,      3,   b"\xe6\x80"),   # out 0x80, al
    "in21":  ([13],          13,   0,      3,   b"\xe4\x21"),   # in al, 0x21
    "out20": ([13],          13,   0,      3,   b"\xe6\x20"),   # out 0x20, al
    "inffff": ([13],         13,   0,      3,   b"\xec"),       # in al, dx (dx = 0xffff)
    "df":    ([8, 11, 13],   8,    0,      3,   None),
    "kcopy": ([14],          14,   0x0,    0,   None),
}


def catch_mode(sock, map_path, result_path, ready_path, case):
    setup(map_path, result_path)
    watched, want_vector, want_error, ring, insn = CASES[case]
    top = CONFIG["RORTHOX_KERNEL_TSS_STACK_TOP"]
    user_cs = CONFIG["USER_CODE_SEGMENT"]
    kernel_cs = CONFIG["KERNEL_CODE_SELECTOR"]
    try:
        r = connect(sock)
        if case == "df":
            # Make the #GP gate not present: delivering the #GP then raises #NP, and a second
            # contributory exception while delivering the first one is a double fault.
            gate = SYMBOLS["idt_descriptors"] + 13 * 8 + 5
            attr = r.read(gate, 1)[0]
            r.write(gate, bytes([attr & 0x7F]))
            RES.add("gp_gate_cleared", r.read(gate, 1)[0] == attr & 0x7F, "type_attr %#x" % r.read(gate, 1)[0])
        stubs = {stub(v): v for v in watched}
        for addr in stubs:
            r.add_breakpoint(addr)
        r._send_packet("c")
        with open(ready_path, "w") as f:
            f.write("ready\n")
        try:
            r._check_stop(r._read_packet(60))
        except TimeoutError:
            r._send_raw(b"\x03")
            raise RspError("no exception within 60 s of the key")
        eip = r.reg("eip")
        vector = stubs.get(eip)
        RES.add("stub_vector", vector == want_vector, "entered the stub of vector %r, expected %d" % (vector, want_vector))
        if vector != want_vector:
            bail()
        for addr in stubs:
            r.remove_breakpoint(addr)
        task = r.read32(SYMBOLS["current_task"])   # nothing has switched tasks yet

        esp = r.reg("esp")
        has_error = vector in ERROR_VECTORS
        cpu_bytes = r.read(esp, 24)
        cpu = struct.unpack("<6I", cpu_bytes)
        error = cpu[0] if has_error else None
        f_eip, f_cs, f_flags = cpu[1:4] if has_error else cpu[0:3]
        print("  stub int%d: error %s eip %#x cs %#x eflags %#x" %
              (vector, "-" if error is None else "%#x" % error, f_eip, f_cs, f_flags), flush=True)
        if want_error is not None:
            RES.add("cpu_error_code", error == want_error, "error code %r, expected %#x" % (error, want_error))
        ok = (f_cs == user_cs and esp == top - (24 if has_error else 20)) if ring == 3 else f_cs == kernel_cs
        RES.add("cpu_ring%d" % ring, ok, "cs %#x esp %#x" % (f_cs, esp))
        cr = r.control_registers()
        print("  cpu cr2 0x%08x" % cr["cr2"], flush=True)
        if insn is not None:
            if case in ("int3", "int18"):   # traps: the saved eip is after the instruction
                at = r.read(f_eip - len(insn), len(insn))
            else:
                at = r.read(f_eip, len(insn))
            RES.add("faulting_instruction", at == insn, "bytes %s at eip %#x, expected %s" % (at.hex(), f_eip, insn.hex()))
        if case == "inffff":
            # The stub has not run yet: edx still holds the port the program used.
            dx = r.reg("edx") & 0xFFFF
            RES.add("io_port", dx == 0xFFFF, "dx %#x, expected 0xffff" % dx)
        want_cr2 = None
        if case == "pf":
            want_cr2 = r.reg("eax")      # fault_read keeps the pointer in eax
        elif case == "kcopy":
            user_esp = r.read32(top - 8)  # the syscall's ring-3 frame at the top of the TSS stack
            want_cr2 = r.read32(user_esp)  # print's argument: the freed pointer
        if want_cr2 is not None:
            RES.add("cpu_cr2", cr["cr2"] == want_cr2, "cr2 %#x, expected %#x" % (cr["cr2"], want_cr2))

        # The frame interrupt_handler gets.
        r.add_breakpoint(SYMBOLS["interrupt_handler"])
        if run_to(r, None, 10) != SYMBOLS["interrupt_handler"]:
            RES.add("handler_entered", False, "interrupt_handler not reached")
            bail()
        r.remove_breakpoint(SYMBOLS["interrupt_handler"])
        h_vector, frame, h_cr2, words = handler_frame(r)
        want_frame = esp - 32 if has_error else esp - 36
        bad = []
        if h_vector != vector:
            bad.append("vector %d" % h_vector)
        if frame != want_frame:
            bad.append("frame at %#x, expected %#x" % (frame, want_frame))
        if words[8] != (error or 0):
            bad.append("error_code %#x, expected %#x" % (words[8], error or 0))
        if words[9:12] != (f_eip, f_cs, f_flags):
            bad.append("ip/cs/flags %s" % ["%#x" % w for w in words[9:12]])
        if vector == 14 and h_cr2 != cr["cr2"]:
            bad.append("cr2 argument %#x, cr2 %#x" % (h_cr2, cr["cr2"]))
        RES.add("handler_frame", not bad, "; ".join(bad))

        # What the kernel does with it.
        stops = {SYMBOLS["process_terminate"]: "process_terminate", SYMBOLS["PANIC"]: "PANIC"}
        for addr in stops:
            r.add_breakpoint(addr)
        hit = stops.get(run_to(r, stops, 15))
        if ring == 3 and case != "df":
            process = r.read32(task + 56) if task else None   # struct task: process at 56
            arg = r.read32(r.reg("esp") + 4) if hit else None
            # Still handling this exception: its CPU frame is still the one at the top of the TSS
            # stack (a later exit syscall would have put its own frame there).
            same = hit is not None and r.read(esp, 24) == cpu_bytes
            RES.add("process_terminated", hit == "process_terminate" and arg == process and same,
                    "stopped at %s, argument %s, faulting task's process %s, %s" %
                    (hit, "-" if arg is None else "%#x" % arg, "-" if process is None else "%#x" % process,
                     "from the exception" if same else "not from the exception"))
        else:
            msg = read_string(r, r.read32(r.reg("esp") + 4)) if hit == "PANIC" else None
            RES.add("kernel_panic", hit == "PANIC" and msg == "Kernel exception", "stopped at %s, message %r" % (hit, msg))
            for addr in stops:
                r.remove_breakpoint(addr)
            r.add_breakpoint(SYMBOLS["process_terminate"])
            r.add_breakpoint(SYMBOLS["PAUSE"])
            eip = run_to(r, None, 15)
            RES.add("halted_in_pause", eip == SYMBOLS["PAUSE"], "stopped at %s" % where(eip))
        r.detach()
    except (RspError, OSError) as e:
        fail_session(e)


# --- list --------------------------------------------------------------------------------------

def list_mode(sock, map_path, elf_path, result_path, tag, want_tasks, *opts):
    setup(map_path, result_path)
    want_tasks = int(want_tasks)
    options = dict(o.split("=", 1) for o in opts)
    elf = read_elf_symbols(elf_path)
    # The ELF linked from the same objects must agree with kernel.map for every global symbol.
    differ = [n for n, a in SYMBOLS.items() if n in elf and elf[n] != a]
    RES.add("%s_elf_matches_map" % tag, not differ and "processes" in elf, "differ: %s" % differ[:5])
    max_processes = 12
    try:
        r = connect(sock)
        head = r.read32(SYMBOLS["task_head"])
        tail = r.read32(SYMBOLS["task_tail"])
        current = r.read32(SYMBOLS["current_task"])
        owner = r.read32(SYMBOLS["current_process"])
        slots = struct.unpack("<%dI" % max_processes, r.read(elf["processes"], 4 * max_processes))
        tasks = []
        bad = []
        prev = 0
        t = head
        while t and len(tasks) < 16:
            words = struct.unpack("<19I", r.read(t, 76))
            t_process, t_next, t_prev = words[14], words[17], words[18]
            if t_prev != prev:
                bad.append("task %#x prev %#x, expected %#x" % (t, t_prev, prev))
            p_task = r.read32(t_process + 112) if t_process else 0   # struct process: task at 112
            p_id = struct.unpack("<H", r.read(t_process, 2))[0] if t_process else -1
            if p_task != t:
                bad.append("task %#x: process %#x points to task %#x" % (t, t_process, p_task))
            if not (0 <= p_id < max_processes) or slots[p_id] != t_process:
                bad.append("process %#x (id %d) not in its slot" % (t_process, p_id))
            tasks.append((t, t_process))
            prev, t = t, t_next
        if tasks and tail != tasks[-1][0]:
            bad.append("tail %#x, last task %#x" % (tail, tasks[-1][0]))
        if not tasks and tail:
            bad.append("empty list, tail %#x" % tail)
        live = [s for s in slots if s]
        if sorted(live) != sorted(p for _, p in tasks):
            bad.append("slots %s, processes in the list %s" % (["%#x" % s for s in live], ["%#x" % p for _, p in tasks]))
        if current not in [t for t, _ in tasks]:
            bad.append("current_task %#x not in the list" % current)
        print("  %s: tasks %s, current %#x, keyboard owner %#x" %
              (tag, ["%#x/%#x" % tp for tp in tasks], current, owner), flush=True)
        RES.add("%s_list_consistent" % tag, not bad, "; ".join(bad))
        RES.add("%s_task_count" % tag, len(tasks) == want_tasks, "%d tasks, expected %d" % (len(tasks), want_tasks))
        if "owner" in options and tasks:
            want = tasks[0][1] if options["owner"] == "first" else tasks[-1][1]
            RES.add("%s_keyboard_owner" % tag, owner == want, "current_process %#x, expected %#x" % (owner, want))
        if "save" in options and tasks:
            with open(options["save"], "w") as f:
                f.write("%#x %#x\n" % tasks[0])
        if "shell" in options:
            with open(options["shell"]) as f:
                shell = tuple(int(x, 16) for x in f.read().split())
            RES.add("%s_same_shell" % tag, bool(tasks) and tasks[0] == shell,
                    "first task %s, shell was %#x/%#x" % (["%#x" % x for x in tasks[0]] if tasks else "-", shell[0], shell[1]))
        r.detach()
    except (RspError, OSError) as e:
        fail_session(e)


# --- layout ------------------------------------------------------------------------------------

def layout_mode(sock, map_path, result_path):
    setup(map_path, result_path)
    total = CONFIG["RORTHOX_TOTAL_INTERRUPTS"]
    syscall = CONFIG["RORTHOX_SYSCALL_VECTOR"]
    kernel_cs = CONFIG["KERNEL_CODE_SELECTOR"]
    user_ds = CONFIG["USER_DATA_SEGMENT"]
    task_return = SYMBOLS["task_return"]
    handler = SYMBOLS["interrupt_handler"]
    syscall_handler = SYMBOLS["isr80h_handler"]
    try:
        r = connect(sock)
        r.add_breakpoint(task_return)
        if r.cont(60) != task_return:
            raise RspError("did not stop at task_return")

        # IDT gates: present, ring 0 code selector, the right stub, DPL 0 only on the error-code vectors.
        idt = r.read(SYMBOLS["idt_descriptors"], total * 8)
        bad_attr, bad_target = [], []
        for v in range(total):
            lo, sel, zero, attr, hi = struct.unpack_from("<HHBBH", idt, v * 8)
            target = SYMBOLS["isr80h_wrapper"] if v == syscall else stub(v)
            if (hi << 16 | lo) != target or sel != kernel_cs:
                bad_target.append(v)
            want = 0x8E if v in ERROR_VECTORS else 0xEE
            if attr != want:
                bad_attr.append("%d:%#x/%#x" % (v, attr, want))
        RES.add("idt_targets", not bad_target, "vectors %s" % bad_target[:8])
        RES.add("idt_gate_dpl", not bad_attr, "%d gates wrong (vector:got/want) %s" % (len(bad_attr), " ".join(bad_attr[:12])))

        entry = r.regs()
        task = r.read32(SYMBOLS["current_task"])
        saved_task = r.read(task, 76)
        base = entry["esp"] - 1024
        stack = r.read(base, 1280)
        cr2 = r.control_registers()["cr2"]
        sentinels = {"edi": 0xD1D1D100, "esi": 0x51515100, "ebp": 0xB0B0B000, "ebx": 0xB1B1B100,
                     "edx": 0xD0D0D000, "ecx": 0xC0C0C000, "eax": 0xA0A0A000}
        order = ["edi", "esi", "ebp", None, "ebx", "edx", "ecx", "eax"]
        frame_flags = 0x00000046     # ZF PF, IF clear
        r.add_breakpoint(handler)
        r.add_breakpoint(syscall_handler)

        def enter(target, v, with_error, segments=None, eax=None):
            """Build a CPU-like same-privilege frame at base+512 and jump to <target>."""
            r.write(base, stack[:1280])
            regs = dict(entry)
            regs.update({k: s | (v & 0xFF) for k, s in sentinels.items()})
            if eax is not None:
                regs["eax"] = eax
            error = 0xE0000000 | v << 8 | 0x5A
            words = ([error] if with_error else []) + [task_return, kernel_cs, frame_flags]
            top = base + 512
            r.write(top, struct.pack("<%dI" % len(words), *words))
            regs["esp"] = top
            regs["eip"] = target
            regs["eflags"] = 0x00000002
            if segments:
                for s in ("ds", "es", "fs", "gs"):
                    regs[s] = segments
            r.set_regs(regs)
            return regs, error, top, top + 4 * len(words)

        def check_entry(regs, v, error, top, with_error, want_vector):
            eip = run_to(r, None, 10)
            if eip != (handler if want_vector is not None else syscall_handler):
                return "stopped at %s" % where(eip)
            esp = r.reg("esp")
            first, frame = struct.unpack("<II", r.read(esp + 4, 8))
            words = struct.unpack("<12I", r.read(frame, 48))
            bad = []
            if want_vector is not None:
                h_cr2 = r.read32(esp + 12)
                if first != want_vector:
                    bad.append("vector %d" % first)
                if h_cr2 != cr2:
                    bad.append("cr2 argument %#x, cr2 %#x" % (h_cr2, cr2))
            elif first != regs["eax"]:
                bad.append("command %#x" % first)
            cpu = top + (4 if with_error else 0)
            if frame + 36 != cpu:
                bad.append("frame at %#x, expected %#x" % (frame, cpu - 36))
            for i, name in enumerate(order):
                if name and words[i] != regs[name]:
                    bad.append("%s %#x" % (name, words[i]))
            if words[8] != (error if with_error else 0):
                bad.append("error_code %#x" % words[8])
            if words[9:12] != (task_return, kernel_cs, frame_flags):
                bad.append("ip/cs/flags %s" % ["%#x" % w for w in words[9:12]])
            return "; ".join(bad)

        def rewind():
            r.write(base, stack)
            r.write(task, saved_task)
            r.set_regs(entry)

        bad_entries = []
        for v in range(total):
            with_error = v in ERROR_VECTORS
            regs, error, top, _ = enter(stub(v), v, with_error)
            problem = check_entry(regs, v, error, top, with_error, v)
            if problem:
                bad_entries.append("int%d: %s" % (v, problem))
            rewind()
        RES.add("stub_frames", not bad_entries, "%d of %d stubs, first: %s" %
                (len(bad_entries), total, " | ".join(bad_entries[:3])))

        regs, error, top, _ = enter(SYMBOLS["isr80h_wrapper"], 0, False, eax=0x3FF)
        problem = check_entry(regs, 0, error, top, False, None)
        RES.add("syscall_frame", not problem, problem)

        # Run three entries back out: the syscall stub with an unknown command (returns 0), an
        # unused vector (no callback) and an NMI in ring 0 (no save, no switch, segments kept).
        def check_return(name, regs, end, eax, segments, keep_task):
            eip = run_to(r, None, 10)
            bad = []
            if eip != task_return:
                bad.append("returned to %s" % where(eip))
            else:
                now = r.regs()
                if now["esp"] != end:
                    bad.append("esp %#x, expected %#x" % (now["esp"], end))
                for k in sentinels:
                    want = eax if (k == "eax" and eax is not None) else regs[k]
                    if now[k] != want:
                        bad.append("%s %#x, expected %#x" % (k, now[k], want))
                if now["eflags"] & 0xFFF != frame_flags:
                    bad.append("eflags %#x" % now["eflags"])
                if segments and [now[s] for s in ("ds", "es", "fs", "gs")] != [segments] * 4:
                    bad.append("segments %s" % [hex(now[s]) for s in ("ds", "es", "fs", "gs")])
                if keep_task and r.read(task, 76) != saved_task:
                    bad.append("the current task's saved state changed")
                if keep_task and r.read32(SYMBOLS["current_task"]) != task:
                    bad.append("current_task changed")
            RES.add(name, not bad, "; ".join(bad))
            rewind()

        regs, error, top, end = enter(SYMBOLS["isr80h_wrapper"], 0, False, eax=0x3FF)
        if run_to(r, None, 10) == syscall_handler:
            check_return("syscall_return", regs, end, 0, None, False)
        else:
            RES.add("syscall_return", False, "isr80h_handler not reached")
            rewind()

        unused = 0x81
        regs, error, top, end = enter(stub(unused), unused, False)
        if run_to(r, None, 10) == handler:
            check_return("unused_vector_return", regs, end, None, None, False)
        else:
            RES.add("unused_vector_return", False, "interrupt_handler not reached")
            rewind()

        cr3 = r.control_registers()["cr3"]
        regs, error, top, end = enter(stub(2), 2, False, segments=user_ds)
        if run_to(r, None, 10) == handler:
            check_return("kernel_nmi_return", regs, end, None, user_ds, True)
        else:
            RES.add("kernel_nmi_return", False, "interrupt_handler not reached")
            rewind()
        RES.add("kernel_nmi_cr3", r.control_registers()["cr3"] == cr3, "cr3 changed")

        if r.regs() != entry or r.read(base, 1280) != stack or r.read(task, 76) != saved_task:
            raise RspError("could not put task_return's entry state back")
        r.detach()
    except (RspError, OSError) as e:
        fail_session(e)


# --- early -------------------------------------------------------------------------------------

def early_mode(sock, map_path, result_path, nested=None):
    setup(map_path, result_path)
    kernel_cs = CONFIG["KERNEL_CODE_SELECTOR"]
    panic = SYMBOLS["PANIC"]
    pause = SYMBOLS["PAUSE"]
    try:
        r = connect(sock)
        r.add_breakpoint(SYMBOLS["idt_load"])
        r.cont(60)
        r.remove_breakpoint(SYMBOLS["idt_load"])
        ret = r.read32(r.reg("esp"))
        r.add_breakpoint(ret)
        if r.cont(20) != ret:
            raise RspError("idt_load did not return")
        r.remove_breakpoint(ret)
        cr = r.control_registers()
        RES.add("after_idt_load", r.reg("cs") == kernel_cs and not cr["cr0"] & 0x80000000 and
                not r.reg("eflags") & EFLAGS_IF and r.read32(SYMBOLS["current_task"]) == 0 and
                cr["idt_base"] == SYMBOLS["idt_descriptors"],
                "cs %#x cr0 %#x eflags %#x current_task %#x idt %#x" %
                (r.reg("cs"), cr["cr0"], r.reg("eflags"), r.read32(SYMBOLS["current_task"]), cr["idt_base"]))
        r.write(ret, b"\x0f\x0b")       # ud2
        for addr in (stub(6), SYMBOLS["interrupt_handler"], panic, pause):
            r.add_breakpoint(addr)

        eip = run_to(r, None, 10)
        frame = struct.unpack("<3I", r.read(r.reg("esp"), 12))
        RES.add("ud2_stub", eip == stub(6) and frame[0] == ret and frame[1] == kernel_cs,
                "stopped at %s, frame %s" % (where(eip), ["%#x" % w for w in frame]))
        if eip != stub(6):
            bail()
        esp = r.reg("esp")
        eip = run_to(r, None, 10)
        if eip == SYMBOLS["interrupt_handler"]:
            vector, f, _, words = handler_frame(r)
            RES.add("ud2_handler_frame", vector == 6 and f == esp - 36 and words[8] == 0 and words[9] == ret,
                    "vector %d frame %#x (want %#x) error %#x ip %#x" % (vector, f, esp - 36, words[8], words[9]))
            eip = run_to(r, None, 10)
        else:
            RES.add("ud2_handler_frame", False, "interrupt_handler not reached, stopped at %s" % where(eip))
        msg = read_string(r, r.read32(r.reg("esp") + 4)) if eip == panic else None
        RES.add("ud2_panic", eip == panic and msg == "Kernel exception", "stopped at %s, message %r" % (where(eip), msg))
        if eip != panic:
            bail()

        if nested:
            r.write(panic, b"\x0f\x0b")
            eip = run_to(r, None, 10)
            frame = struct.unpack("<3I", r.read(r.reg("esp"), 12))
            RES.add("nested_stub", eip == stub(6) and frame[0] == panic and frame[1] == kernel_cs,
                    "stopped at %s, frame %s" % (where(eip), ["%#x" % w for w in frame]))
            r.remove_breakpoint(SYMBOLS["interrupt_handler"])
            eip = run_to(r, None, 10)
            RES.add("nested_straight_to_pause", eip == pause,
                    "stopped at %s" % ("PANIC again" if eip == panic else "int6 again" if eip == stub(6) else where(eip)))
        else:
            eip = run_to(r, None, 10)
            RES.add("halted_in_pause", eip == pause, "stopped at %s" % where(eip))
        cr3 = r.control_registers()["cr3"]
        RES.add("cr3_untouched", cr3 == cr["cr3"], "cr3 %#x, was %#x" % (cr3, cr["cr3"]))
        r.detach()
    except (RspError, OSError) as e:
        fail_session(e)


# --- nmi ---------------------------------------------------------------------------------------

def nmi_mode(sock, map_path, result_path):
    setup(map_path, result_path)
    pause = SYMBOLS["PAUSE"]
    try:
        r = connect(sock)
        eip = r.reg("eip")
        RES.add("halted_before_nmi", pause <= eip < pause + 8, "eip %#x, PAUSE at %#x" % (eip, pause))
        others = [stub(v) for v in range(32) if v != 2]
        for addr in [stub(2), pause, SYMBOLS["PANIC"]] + others:
            r.add_breakpoint(addr)
        r.monitor("nmi")
        eip = run_to(r, None, 10)
        frame = struct.unpack("<3I", r.read(r.reg("esp"), 12))
        RES.add("nmi_stub", eip == stub(2) and pause <= frame[0] < pause + 8 and
                frame[1] == CONFIG["KERNEL_CODE_SELECTOR"],
                "stopped at %s, frame %s" % (where(eip), ["%#x" % w for w in frame]))
        if eip != stub(2):
            bail()
        eip = run_to(r, None, 10)
        RES.add("nmi_straight_to_pause", eip == pause, "stopped at %s" % where(eip))
        r.detach()
    except (RspError, OSError) as e:
        fail_session(e)


if __name__ == "__main__":
    mode, args = sys.argv[1], sys.argv[2:]
    {"catch": catch_mode, "list": list_mode, "layout": layout_mode, "early": early_mode, "nmi": nmi_mode}[mode](*args)
