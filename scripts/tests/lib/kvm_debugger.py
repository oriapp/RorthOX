"""Debugger side of kvm-debugger.sh: what rsp.py relies on, checked on the real gdbstub.

kvm_debugger.py <socket> <kernel.map> <kernel disassembly> <shell elf> <debugcon log> <results> tcg|kvm

The shell is idle, polling getkey. All checks run in one debugger session.
  sync      at task_return: "p eip" before and after the first "g" is recorded (KVM may read a
            stale copy before it; not checked). ebx set there is dead: at the "and" the guest's own
            ebx (the registers pointer) is read. eax set at the "and" must reach the CPU: at the
            "push" it is (value & mask) | 0x202. Those two stops come from the test's own "c", so a
            register write and a memory read are the first accesses after them. The client sends
            "g" after every stop before any "p", "P", "m" or "M" (packet log of the whole session).
  moved     a single step after eip was set to a new value is refused without sending "s".
  capacity  4 breakpoints are accepted, the client refuses a fifth without sending it; the stub's
            own answer to a fifth is recorded (KVM must refuse it).
  steps     from the timer's stub (a tick in ring 3) through task_next to the iretd in task_return
            (ring 0, IF clear), every step advances by one instruction of the kernel's disassembly
            (a step that ends before its instruction ran, see rsp.py, is tried again; at most 5).
  stepoff   from here QEMU's single steps let interrupts in (Qqemu.sstep=1: TCG then behaves like
            KVM without KVM_GUESTDBG_BLOCKIRQ; by default it masks them during a step).
            120 stops at the shell's "mov eax, SYSCALL_GETKEY" (ring 3, IF set); in each, a key
            byte is put into the keyboard controller (IRQ 1 pending), and continuing steps off to
            the next instruction, the guest takes the key, and no #DB reaches the guest.
  fault     ud2 at that breakpoint, a breakpoint at the #UD stub: under TCG the step stops at the
            stub and cont() raises; under KVM the step runs freely until that breakpoint (or on).
            Either way cont() must raise, not continue, within its timeout; what happened is printed.
"""

import re
import sys
import time

from rsp import EFLAGS_IF, Results, Rsp, RspError, read_elf_symbols, read_map

SENTINEL_EBX = 0x5EB15EB1
SENTINEL_EAX = 0x5EB10203   # (& 0x00250dd5) | 0x202 = 0x00210203: RF, ID and CF
FLAGS_MASK, FLAGS_SET = 0x00250DD5, 0x202
STOPOFFS = 120


class LoggedRsp(Rsp):
    """Rsp that keeps every packet it sends and where each step off a breakpoint landed."""

    def __init__(self, *args):
        self.log = []
        self.landed = []
        super().__init__(*args)

    def _send_packet(self, payload):
        self.log.append(payload)
        super()._send_packet(payload)

    def _send_raw(self, data):
        if data == b"\x03":
            self.log.append("\x03")
        super()._send_raw(data)

    def _step_off(self, eip):
        super()._step_off(eip)
        self.landed.append((eip, self.reg("eip")))


def unsynced_accesses(log):
    """Register or memory packets sent after a stop before any "g" (or "info registers")."""
    bad, stopped = [], True
    for p in log:
        if p[:1] in ("c", "s", "?", "D", "\x03"):
            stopped = True
        elif p == "g" or p == "qRcmd," + b"info registers".hex():
            stopped = False
        elif p[:1] in ("p", "P", "m", "M") and stopped:
            bad.append(p[:20])
    return bad


def disassembly(path):
    """{addr: (next addr, mnemonic, operands)} from objdump -D (a long instruction's second line
    of bytes has no mnemonic and is skipped)."""
    rows = []
    for line in open(path):
        m = re.match(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)\s*(.*)$", line)
        if m:
            rows.append((int(m.group(1), 16), m.group(3), m.group(4)))
    out = {}
    for (a, mn, ops), nxt in zip(rows, rows[1:] + [(0, "", "")]):
        out[a] = (nxt[0], mn, ops)
    return out


def step_targets(r, dis, eip):
    """Where one step from eip may end, or None if any instruction start is right (indirect)."""
    nxt, mn, ops = dis[eip]
    direct = re.match(r"^([0-9a-f]+) <", ops)
    if mn.startswith("ret"):
        return {r.read32(r.reg("esp"))}
    if mn.startswith("call") or mn == "jmp":
        return {int(direct.group(1), 16)} if direct else None
    if mn.startswith("j") or mn.startswith("loop"):
        return {int(direct.group(1), 16), nxt}
    if mn.startswith("rep"):
        return {eip, nxt}
    return {nxt}


def status(r):
    """The keyboard controller's status byte (bit 0: a byte waits in the output buffer)."""
    m = re.search(r"= (0x[0-9a-f]+)", r.monitor("i 0x64"))
    if not m:
        raise RspError("no port value from the monitor")
    return int(m.group(1), 16)


def raw_cont(r):
    r._send_packet("c")
    r._check_stop(r._read_packet(10))


def main():
    sock, kmap, dis_path, shell, debugcon, results, accel = sys.argv[1:8]
    res = Results(results)
    syms = read_map(kmap)[0]
    dis = disassembly(dis_path)
    getkey = read_elf_symbols(shell)["rorthox_getkey"] + 3
    task_return, task_next = syms["task_return"], syms["task_next"]
    tr = sorted(a for a in dis if task_return <= a < task_return + 0x60)
    and_eax = next(a for a in tr if dis[a][1] == "and" and dis[a][2].endswith(",%eax"))
    push_eax = next(a for a in tr if dis[a][1] == "push" and dis[a][2] == "%eax")
    iretd = next(a for a in tr if dis[a][1].startswith("iret"))
    r = LoggedRsp(sock)
    try:
        # sync
        r.add_breakpoint(task_return)
        r.cont(30)
        r.log.append("g")   # the raw "p" below comes before the sync on purpose: not counted
        before = r.request("p8")
        after = r.reg("eip")
        print("  sync: p eip before g %s, after g %#x" % (before, after))
        res.add("sync_stop", after == task_return, "stopped at %#x" % after)
        regs_ptr = r.read32(r.reg("esp") + 4)
        r.set_reg("ebx", SENTINEL_EBX)
        r.remove_breakpoint(task_return)
        r.add_breakpoint(and_eax)
        r.add_breakpoint(push_eax)
        # The test's own continues (as guest_state.py and exceptions.py do): cont() would read eip
        # first, here a register write and a memory read are the first accesses after the stop.
        raw_cont(r)
        r.set_reg("eax", SENTINEL_EAX)
        stop, ebx = r.reg("eip"), r.reg("ebx")
        res.add("sync_guest_ebx", stop == and_eax and ebx == regs_ptr and ebx != SENTINEL_EBX,
                "stop %#x ebx %#x, guest's %#x" % (stop, ebx, regs_ptr))
        r.remove_breakpoint(and_eax)
        raw_cont(r)
        guest_flags = (r.read32(regs_ptr + 36) & FLAGS_MASK) | FLAGS_SET
        stop, eax = r.reg("eip"), r.reg("eax")
        want = (SENTINEL_EAX & FLAGS_MASK) | FLAGS_SET
        res.add("sync_write_reached", stop == push_eax and eax == want, "stop %#x eax %#x, want %#x" % (stop, eax, want))
        r.set_reg("eax", guest_flags)

        # moved
        sent = len(r.log)
        r.set_reg("eip", push_eax + 1)
        try:
            r.step()
            refused = False
        except RspError:
            refused = True
        res.add("moved_eip_step_refused", refused and "s" not in r.log[sent:], "a step was allowed or sent")
        r.set_reg("eip", push_eax)

        # capacity
        for a in list(r.breakpoints):
            r.remove_breakpoint(a)
        five = [task_return, and_eax, task_next, iretd, push_eax]
        for a in five[:4]:
            r.add_breakpoint(a)
        sent = len(r.log)
        try:
            r.add_breakpoint(five[4])
            refused = False
        except RspError:
            refused = True
        res.add("z1_fifth_refused_by_client", refused and not any(p.startswith("Z1") for p in r.log[sent:]),
                "the client sent or accepted a fifth Z1")
        raw = r.request("Z1,%x,1" % five[4])
        print("  capacity: the stub answered a fifth Z1 with %r" % raw)
        if raw == "OK":
            r.expect_ok("z1,%x,1" % five[4])
        res.add("z1_fifth_stub", raw != "OK" if accel == "kvm" else True, "KVM accepted a fifth Z1")
        for a in five[:4]:
            r.remove_breakpoint(a)

        # steps
        tick = syms["int32"]     # the timer's interrupt stub; from ring 3 it switches tasks
        r.add_breakpoint(tick)
        for _ in range(20):
            if r.cont(10) == tick and r.read32(r.reg("esp") + 4) & 3 == 3:
                break
        r.remove_breakpoint(tick)
        eip, steps, stalls, wrong, seen_tr = r.reg("eip"), 0, 0, [], False
        while eip != iretd and steps < 3000 and len(wrong) < 5:
            if eip not in dis or dis[eip][1] in ("sti", "hlt") or dis[eip][1].startswith(("int", "iret")):
                wrong.append("at %#x %s" % (eip, dis.get(eip, ("", "?"))[1]))
                break
            want = step_targets(r, dis, eip)
            r.step()
            steps += 1
            now = r.reg("eip")
            if now == eip and (want is None or eip not in want):
                stalls += 1
                print("  steps: the step at %#x (%s) ended before it ran" % (eip, dis[eip][1]))
                continue
            if (want is not None and now not in want) or (want is None and now not in dis):
                wrong.append("%#x %s -> %#x" % (eip, dis[eip][1], now))
            if r.reg("cs") != 0x08 or r.reg("eflags") & EFLAGS_IF:
                wrong.append("%#x: cs %#x eflags %#x" % (now, r.reg("cs"), r.reg("eflags")))
            seen_tr = seen_tr or now == task_return
            eip = now
        print("  steps: %d to the iretd, %d stalled" % (steps, stalls))
        res.add("steps_one_insn", eip == iretd and not wrong and seen_tr and steps >= 200 and stalls <= 5,
                "%d steps, %d stalls, ended %#x, through task_return %s: %s" % (steps, stalls, eip, seen_tr, wrong[:5]))

        # stepoff: from here single steps let interrupts in
        res.add("sstep_irqs", r.request("Qqemu.sstep=0x1") == "OK", "Qqemu.sstep refused")
        r.add_breakpoint(getkey)
        r.add_breakpoint(syms["int1"])
        pending = taken = 0
        bad = []
        r.landed = []
        for i in range(STOPOFFS + 1):    # the first stop comes from elsewhere, then one step-off each
            try:
                stop = r.cont(10)
            except RspError as e:
                bad.append("stop %d: %s" % (i, e))
                break
            if stop != getkey or r.reg("cs") & 3 != 3 or not r.reg("eflags") & EFLAGS_IF:
                bad.append("stop %d at %#x cs %#x" % (i, stop, r.reg("cs")))
                break
            if i == 0:
                res.add("stepoff_insn", r.read(getkey, 1) == b"\xb8", "no mov eax, imm32 at %#x" % getkey)
            taken += i > 0 and not status(r) & 1     # the last stop's key was read
            r.monitor("o 0x64 0xd2")    # write the keyboard output buffer: IRQ 1 as for a key
            r.monitor("o 0x60 0xaa")    # left shift released
            pending += status(r) & 1
        last = r.landed
        off = [l for l in last if l != (getkey, getkey + 5)]
        res.add("stepoff_lands_next", not bad and len(last) == STOPOFFS and not off,
                "%s; step-offs elsewhere: %s" % (bad, ["%#x->%#x" % l for l in off[:5]]))
        res.add("stepoff_keys", pending == STOPOFFS + 1 and taken == STOPOFFS,
                "%d keys pending, %d taken" % (pending, taken))
        text = open(debugcon, "rb").read().decode("latin-1")
        res.add("stepoff_no_guest_db", "exception:" not in text and "PANIC" not in text, "debugcon: %r" % text[-200:])

        # fault
        r.remove_breakpoint(syms["int1"])
        r.add_breakpoint(syms["int6"])
        r.write(getkey, b"\x0f\x0b")
        r.op_timeout = 15.0
        t = time.monotonic()
        try:
            r.cont(15)
            outcome = "cont() returned"
        except (RspError, TimeoutError) as e:
            outcome = "%s: %s" % (type(e).__name__, e)
        took = time.monotonic() - t
        try:
            where = "eip %#x cs %#x" % (r.reg("eip"), r.reg("cs"))
        except (RspError, TimeoutError, OSError):
            where = "guest not stopped"
        print("  fault: %s, %s, %.1f s" % (outcome, where, took))
        ok = not outcome.startswith("cont() returned") and took < 40
        if accel == "tcg":
            ok = ok and "exception handler" in outcome and where == "eip %#x cs 0x8" % syms["int6"]
        res.add("fault_step_ends", ok, "%s, %s" % (outcome, where))
    finally:
        # Also after an error: a client that skips the sync usually fails later on.
        bad = unsynced_accesses(r.log)
        res.add("sync_g_first", not bad and r.log.count("g") >= STOPOFFS, "unsynced: %s" % bad[:5])
        r.sock.close()


if __name__ == "__main__":
    main()
