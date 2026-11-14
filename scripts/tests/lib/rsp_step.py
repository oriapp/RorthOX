"""The gdbstub client (rsp.py) against a fake gdbstub, without QEMU.

rsp_step.py <results file>

The fake gdbstub runs on a unix socket in a thread. Its "CPU" has the 16 registers of QEMU's i386
target, a CR3, code bytes below 0x10000 (nop unless a case puts something there) and one 4 KiB page
at 0x400000 per CR3. A step adds 1 to eip unless it is told to stall or eip is on an installed
breakpoint; "c" stops at eip if a breakpoint is installed there, else at the next breakpoint above
eip, and the guest's own work on the way adds 1 to ebx and switches CR3 between two directories.
With no breakpoint above eip the guest runs until "\\x03" stops it at 0x3000.

Stepping off a breakpoint (QEMU can answer a single step with a SIGTRAP stop before the instruction
ran, so eip is unchanged; Rsp.cont() then continued from the breakpoint it had just put back, and
the same breakpoint was reported a second time). Breakpoints at 0x1000 and 0x2000; each case
continues twice from eip 0.
  normal  steps always move: stops at 0x1000 then 0x2000, one step.
  stall   the first step stalls: stops at 0x1000 then 0x2000 (never 0x1000 twice), two steps, and
          both breakpoints are installed again when the guest continues.
  stuck   no step moves: cont() raises RspError naming eip 0x1000 after at most 8 steps, and does
          not continue the guest.

What KVM changes, with the fake acting like QEMU 8.2 with KVM: "p" reads a copy of the registers
that only "g" refreshes (stale after every stop until then), a "P" before that "g" is lost, "m" and
"M" use the copied CR3, a fifth hardware breakpoint is refused, and a step after the debugger moved
eip runs freely to the next breakpoint.
  kvm_stale_read   both stops of cont() are read right (0x1000, then 0x2000).
  kvm_lost_write   ebx written at the first stop reaches the guest, and the next stop reads the
                   guest's ebx (sentinel + 1), not the sentinel.
  kvm_memory_cr3   at each stop, memory reads and writes at 0x400000 go to the page of the CR3 the
                   guest has at that stop.
  kvm_raw_stop     a test's own "c" + stop reply, and "c" + "\\x03" (guest_state.run_for), are
                   followed by a sync before the registers are read.
  kvm_monitor      a monitor command after a stop is no sync, except "info registers".
  kvm_monitor_first_step
                   "info registers" as the first access after a stop, then eip set to the same
                   value: a step is still allowed (the sync records the stop's eip).
  bp_limit         (fake as TCG: no limit) the fifth breakpoint raises without reaching the stub;
                   after removing one another fits, and cont() steps off a breakpoint with 4 set.
  bp_duplicate     a second breakpoint at an installed address raises without a Z1 (TCG would
                   keep two, KVM refuses it).
  kvm_bp_limit     the KVM fake refuses a fifth Z1 itself (what bp_limit protects against).
  moved_eip        a step after the debugger moved eip raises and sends no step; cont() from a
                   moved eip that is not a breakpoint runs; moving eip back allows a step again.
  if_step          stopped at a breakpoint with IF set: the step off runs with IF clear and IF is
                   set again before the guest continues; on every instruction that uses IF or
                   delays interrupts (pushf popf iretd sti cli int3 int n into icebp hlt pop ss,
                   also behind a prefix, and mov ss) cont() raises without stepping; mov ds steps
                   as usual; with IF clear a pushf steps as usual. A step that ends in another CS
                   (an exception handler, as TCG reports it) raises; a step off that fails puts IF
                   back. A one-byte instruction on the last mapped byte steps off (only mov sreg
                   needs the byte after the opcode).
"""

import os
import socket
import struct
import sys
import tempfile
import threading

from rsp import EFLAGS_IF, REGS, Results, Rsp, RspError

MAX_STEPS = 50      # the fake closes the connection then, so an endless retry can't hang the test
STALE = 0xDEAD0000  # what the KVM fake's register copy holds before the first "g"
EIP, EFLAGS, CS = REGS.index("eip"), REGS.index("eflags"), REGS.index("cs")


class FakeStub:
    def __init__(self, path, stalls=0, kvm=False):
        self.kvm = kvm
        self.live = [0x100 * (i + 1) for i in range(len(REGS))]
        self.live[EIP] = 0
        self.live[EFLAGS] = 0x2
        self.cr3 = 0xA000
        self.copy = [STALE] * len(REGS)     # KVM: QEMU's copy of the CPU state
        self.copy_cr3 = 0xBAD000
        self.synced = False
        self.stop_eip = 0
        self.pages = {0xA000: bytearray(4096), 0xB000: bytearray(4096), 0xBAD000: bytearray(4096)}
        for cr3, page in self.pages.items():
            page[0:4] = struct.pack("<I", cr3)
        self.code = {}
        self.breakpoints = set()
        self.max_breakpoints = 4 if kvm else None
        self.stalls = stalls        # how many of the coming steps don't move
        self.step_cs = None         # a step sets cs to this (an exception handler, as TCG shows it)
        self.steps = 0
        self.step_flags = []        # eflags at every step
        self.z1 = 0                 # Z1 packets seen
        self.stops = []             # (eip, breakpoints installed, eflags, ebx, cr3) for every "c"
        self.running = False
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(path)
        self.server.listen(1)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        conn, _ = self.server.accept()
        conn.settimeout(30)
        buf = b""
        try:
            while True:
                while buf[:1] in (b"+", b"-"):
                    buf = buf[1:]
                if buf[:1] == b"\x03":
                    buf = buf[1:]
                    reply = self.interrupt()
                elif b"$" in buf and b"#" in buf and len(buf) >= buf.index(b"#") + 3:
                    start = buf.index(b"$")
                    end = buf.index(b"#")
                    payload = buf[start + 1:end].decode("latin-1")
                    buf = buf[end + 3:]
                    conn.sendall(b"+")
                    reply = self.handle(payload)
                    if reply is None:
                        return
                else:
                    data = conn.recv(4096)
                    if not data:
                        return
                    buf += data
                    continue
                if reply is False:      # the guest runs on: no reply yet
                    continue
                for body in ([reply] if isinstance(reply, str) else reply):
                    body = body.encode("latin-1")
                    conn.sendall(b"$" + body + b"#" + ("%02x" % (sum(body) & 0xFF)).encode())
        except OSError:
            return
        finally:
            conn.close()

    # --- what the debugger sees --------------------------------------------------------------

    def regs_seen(self):
        return self.copy if self.kvm else self.live

    def cr3_seen(self):
        return self.copy_cr3 if self.kvm else self.cr3

    def stopped(self):
        self.synced = False
        self.stop_eip = self.live[EIP]

    def memory(self, addr, length):
        if addr + length <= 0x10000:
            return b"".join(self.code.get(a, b"\x90") for a in range(addr, addr + length))
        if 0x400000 <= addr and addr + length <= 0x401000:
            return self.pages[self.cr3_seen()][addr - 0x400000:addr - 0x400000 + length]
        return None

    # --- the guest ---------------------------------------------------------------------------

    def run_to_breakpoint(self):
        if self.live[EIP] not in self.breakpoints:
            above = sorted(a for a in self.breakpoints if a > self.live[EIP])
            if not above:
                self.running = True
                return False
            self.live[EIP] = above[0]
        self.guest_work()
        self.stops.append((self.live[EIP], frozenset(self.breakpoints), self.live[EFLAGS],
                           self.live[REGS.index("ebx")], self.cr3))
        self.stopped()
        return "T05thread:01;"

    def guest_work(self):
        self.live[REGS.index("ebx")] = (self.live[REGS.index("ebx")] + 1) & 0xFFFFFFFF
        self.cr3 = 0xB000 if self.cr3 == 0xA000 else 0xA000

    def interrupt(self):
        if not self.running:
            return "S05"
        self.running = False
        self.guest_work()
        self.live[EIP] = 0x3000
        self.stopped()
        return "T02thread:01;"

    def handle(self, p):
        if p == "?":
            self.stopped()
            return "S05"
        if p == "g":
            self.synced = True
            self.copy = list(self.live)
            self.copy_cr3 = self.cr3
            return "".join(struct.pack("<I", v).hex() for v in self.live) + "00" * 200
        if p[:1] == "p":
            return struct.pack("<I", self.regs_seen()[int(p[1:], 16)]).hex()
        if p[:1] == "P":
            n, value = p[1:].split("=")
            value = struct.unpack("<I", bytes.fromhex(value))[0]
            if self.kvm and not self.synced:
                return "OK"     # QEMU writes its copy; the next "g" overwrites it
            self.live[int(n, 16)] = value
            self.copy[int(n, 16)] = value
            return "OK"
        if p[:1] == "m":
            addr, length = (int(x, 16) for x in p[1:].split(","))
            data = self.memory(addr, length)
            return "E14" if data is None else data.hex()
        if p[:1] == "M":
            where, data = p[1:].split(":")
            addr, length = (int(x, 16) for x in where.split(","))
            if self.memory(addr, length) is None or addr < 0x400000:
                return "E14"
            self.pages[self.cr3_seen()][addr - 0x400000:addr - 0x400000 + length] = bytes.fromhex(data)
            return "OK"
        if p[:3] in ("Z1,", "z1,"):
            addr = int(p.split(",")[1], 16)
            if p[0] == "Z":
                self.z1 += 1
                if self.max_breakpoints is not None and (len(self.breakpoints) >= self.max_breakpoints or
                                                         addr in self.breakpoints):
                    return "E22"
                self.breakpoints.add(addr)
            else:
                self.breakpoints.discard(addr)
            return "OK"
        if p == "s":
            self.steps += 1
            if self.steps > MAX_STEPS:
                return None
            self.step_flags.append(self.live[EFLAGS])
            if self.kvm and self.live[EIP] != self.stop_eip:
                # KVM armed the step for the old eip: the guest runs freely.
                return self.run_to_breakpoint()
            if self.stalls > 0:
                self.stalls -= 1
            elif self.live[EIP] not in self.breakpoints:
                self.live[EIP] += 1
            if self.step_cs is not None:
                self.live[CS] = self.step_cs
            self.stopped()
            return "S05"
        if p == "c":
            return self.run_to_breakpoint()
        if p.startswith("qRcmd,"):
            command = bytes.fromhex(p[6:]).decode()
            if command == "info registers":     # QEMU reads the CPU state for this one
                self.handle("g")
            return ["O" + ("%s done\n" % command).encode().hex(), "OK"]
        if p == "D":
            return "OK"
        return ""


def session(body, kvm=False, stalls=0, eflags=None, code=b""):
    """body(r, fake) against a fresh fake stub with breakpoints at 0x1000 and 0x2000:
    (body's value, RspError text or None, fake). eflags/code: the guest's EFLAGS and code at 0x1000."""
    tmp = tempfile.mkdtemp(prefix="rsp-step.")
    path = os.path.join(tmp, "gdb")
    fake = FakeStub(path, stalls, kvm)
    if eflags is not None:
        fake.live[EFLAGS] = eflags
    for i, b in enumerate(code):
        fake.code[0x1000 + i] = bytes([b])
    r = None
    try:
        r = Rsp(path, timeout=5)
        r.op_timeout = 5
        r.add_breakpoint(0x1000)
        r.add_breakpoint(0x2000)
        return body(r, fake), None, fake
    except RspError as e:
        return None, str(e), fake
    finally:
        if r is not None:
            r.sock.close()
        fake.server.close()
        os.unlink(path)
        os.rmdir(tmp)


def attempt(fn):
    """fn()'s RspError text, or None."""
    try:
        fn()
        return None
    except RspError as e:
        return str(e)


def raw_stop(r):
    """Continue and wait for the stop the way exceptions.py catch does, without cont()."""
    r._send_packet("c")
    r._check_stop(r._read_packet(5))


def hexes(values):
    return [hex(v) for v in values] if values is not None else None


def report(res, name, ok, detail):
    print("  %s: %s" % (name, detail), flush=True)
    res.add(name, ok, detail)


def two_conts(r, fake, stalls=None):
    stops = [r.cont(5)]
    if stalls is not None:
        fake.stalls = stalls
    return stops + [r.cont(5)]


def step_off_cases(res):
    both = frozenset((0x1000, 0x2000))
    for case, stalls in (("normal", 0), ("stall", 1), ("stuck", MAX_STEPS + 10)):
        stops, error, fake = session(lambda r, f: two_conts(r, f, stalls))
        seen = [st[0] for st in fake.stops]
        print("  %s: cont() returned %s, fake stops %s, %d steps, error %r" %
              (case, hexes(stops), hexes(seen), fake.steps, error), flush=True)
        if case == "normal":
            res.add("normal", error is None and stops == [0x1000, 0x2000] and seen == [0x1000, 0x2000] and
                    fake.steps == 1, "stops %s, %d steps, error %r" % (hexes(seen), fake.steps, error))
        elif case == "stall":
            res.add("stall_one_delivery", error is None and stops == [0x1000, 0x2000] and seen == [0x1000, 0x2000],
                    "cont() returned %s, fake stops %s, error %r" % (hexes(stops), hexes(seen), error))
            res.add("stall_steps", fake.steps == 2, "%d steps, want 2" % fake.steps)
            res.add("stall_breakpoints_back", len(fake.stops) == 2 and fake.stops[1][1] == both,
                    "breakpoints at the second continue: %s" % [sorted(hexes(s[1])) for s in fake.stops])
        else:
            res.add("stuck_fails", error is not None and "0x1000" in error and "stepping off" in error,
                    "error %r" % error)
            res.add("stuck_bounded", 2 <= fake.steps <= 8, "%d steps, want 2..8" % fake.steps)
            res.add("stuck_not_continued", seen == [0x1000], "fake stops %s" % hexes(seen))


def kvm_cases(res):
    # Stale "p": cont() must return the eip of each stop.
    stops, error, _ = session(two_conts, kvm=True)
    report(res, "kvm_stale_read", error is None and stops == [0x1000, 0x2000],
           "cont() returned %s, error %r" % (hexes(stops), error))

    # Lost "P": a register write as the first access after a stop must reach the guest, and the next
    # stop must show the guest's value (sentinel + 1), not the sentinel.
    def lost_write(r, fake):
        raw_stop(r)
        r.set_reg("ebx", 0x5E5E0000)
        r.remove_breakpoint(0x1000)
        r.cont(5)
        return r.reg("ebx")
    got, error, fake = session(lost_write, kvm=True)
    guest = [st[3] for st in fake.stops]
    report(res, "kvm_lost_write", error is None and guest[1:] == [0x5E5E0001] and got == 0x5E5E0001,
           "guest ebx at the stops %s, read %s (want 0x5e5e0001 for both), error %r" %
           (hexes(guest), None if got is None else hex(got), error))

    # "m"/"M" through the CR3 of each stop: after cont() (which reads eip), then after a test's own
    # continue with a write first, then with a read first.
    def memory(r, fake):
        bad = []
        r.add_breakpoint(0x3000)
        for k in range(3):
            if k == 0:
                r.cont(5)
            else:
                r.remove_breakpoint(0x1000 * k)
                raw_stop(r)
            cr3 = fake.cr3
            if k == 1:
                r.write32(0x400004, 0xAC0 + k)
            seen = r.read32(0x400000)
            if k != 1:
                r.write32(0x400004, 0xAC0 + k)
            if seen != cr3:
                bad.append("stop %d: read the page of cr3 %#x, guest cr3 %#x" % (k, seen, cr3))
            if struct.unpack("<I", fake.pages[cr3][4:8])[0] != 0xAC0 + k:
                bad.append("stop %d: the write missed the page of cr3 %#x" % (k, cr3))
        return bad
    bad, error, _ = session(memory, kvm=True)
    report(res, "kvm_memory_cr3", error is None and not bad, "%s, error %r" % (bad or "all stops right", error))

    # A test's own "c" (exceptions.py catch) and "c" + "\x03" (guest_state.run_for).
    def raw(r, fake):
        raw_stop(r)
        eips = [r.reg("eip")]
        r.remove_breakpoint(0x1000)
        r.remove_breakpoint(0x2000)
        r._send_packet("c")
        r._send_raw(b"\x03")
        r._read_packet(5)
        return eips + [r.reg("eip")]
    eips, error, _ = session(raw, kvm=True)
    report(res, "kvm_raw_stop", error is None and eips == [0x1000, 0x3000],
           "eip read after the stops %s (want 0x1000, 0x3000), error %r" % (hexes(eips), error))

    # A monitor command after a stop: only "info registers" counts as a sync.
    def monitor(r, fake):
        eips = []
        for k, command in enumerate(("xp /1wx 0", "info registers")):
            if k:
                r.remove_breakpoint(0x1000)
            raw_stop(r)
            r.monitor(command)
            eips.append(r.reg("eip"))
        return eips
    eips, error, _ = session(monitor, kvm=True)
    report(res, "kvm_monitor", error is None and eips == [0x1000, 0x2000],
           "eip read after xp, after info registers %s (want 0x1000, 0x2000), error %r" % (hexes(eips), error))

    # "info registers" as the first access after a stop, then eip set to what it is: no move.
    def first_step(r, fake):
        raw_stop(r)
        r.monitor("info registers")
        r.set_reg("eip", r.reg("eip"))
        r.remove_breakpoint(0x1000)
        try:
            r.step()
        except Exception as e:
            return "%s: %s" % (type(e).__name__, e)
        return r.reg("eip")
    got, error, _ = session(first_step, kvm=True)
    report(res, "kvm_monitor_first_step", error is None and got == 0x1001,
           "after the step %s (want eip 0x1001), error %r" % (hex(got) if isinstance(got, int) else got, error))


def breakpoint_cases(res):
    def limit(r, fake):
        r.add_breakpoint(0x3000)
        r.add_breakpoint(0x4000)
        fifth = attempt(lambda: r.add_breakpoint(0x5000))
        z1 = fake.z1
        r.remove_breakpoint(0x4000)
        again = attempt(lambda: r.add_breakpoint(0x5000))
        return fifth, z1, again, two_conts(r, fake)
    out, error, fake = session(limit)
    fifth, z1, again, stops = out or (None, None, None, None)
    report(res, "bp_limit_fifth_refused", fifth is not None and "0x5000" in fifth and z1 == 4,
           "fifth add: error %r, %s Z1 packets sent (want 4), error %r" % (fifth, z1, error))
    res.add("bp_limit_slot_reused", error is None and again is None and stops == [0x1000, 0x2000] and
            len(fake.stops) == 2 and len(fake.stops[1][1]) == 4,
            "add after a removal: %r, stops %s, error %r" % (again, hexes(stops), error))

    def stub_limit(r, fake):
        r.add_breakpoint(0x3000)
        r.add_breakpoint(0x4000)
        r.breakpoints.clear()        # bypass the client's own count: only the stub decides
        return attempt(lambda: r.add_breakpoint(0x5000))
    refused, error, _ = session(stub_limit, kvm=True)
    report(res, "kvm_bp_limit", refused is not None and "refused" in refused, "error %r %r" % (refused, error))

    def duplicate(r, fake):
        return attempt(lambda: r.add_breakpoint(0x1000)), fake.z1, two_conts(r, fake)
    out, error, fake = session(duplicate)
    dup, z1, stops = out or (None, None, None)
    report(res, "bp_duplicate_refused", dup is not None and "already" in dup and z1 == 2 and
           stops == [0x1000, 0x2000],
           "second add at 0x1000: error %r, %s Z1 packets sent (want 2), stops %s, error %r" %
           (dup, z1, hexes(stops), error))


def moved_eip_case(res):
    def moved(r, fake):
        r.cont(5)
        r.set_reg("eip", 0x1800)
        refused = attempt(r.step)
        steps = fake.steps
        after = r.cont(5)               # 0x1800 is no breakpoint: no step needed
        r.set_reg("eip", 0x1900)
        r.set_reg("eip", 0x2000)        # back where it stopped
        r.remove_breakpoint(0x2000)
        back = attempt(r.step) is None and r.reg("eip") == 0x2001
        return refused, steps, after, back
    out, error, _ = session(moved, kvm=True)
    refused, steps, after, back = out or (None, None, None, None)
    print("  moved_eip: step error %r, %s steps sent, cont() then %s, step after moving back %s, error %r" %
          (refused, steps, None if after is None else hex(after), back, error), flush=True)
    res.add("moved_eip_step_refused", refused is not None and "moved eip" in refused and steps == 0,
            "step error %r, %s steps sent, error %r" % (refused, steps, error))
    res.add("moved_eip_cont_runs", after == 0x2000, "cont() returned %s" % (None if after is None else hex(after)))
    res.add("moved_back_steps", back is True, "step after moving eip back: %s" % back)


def if_cases(res):
    # Stopped at 0x1000 (a nop) with IF set.
    stops, error, fake = session(two_conts, eflags=0x246)
    flags_c = [st[2] for st in fake.stops]
    print("  if_step: stops %s, eflags at the steps %s, at the continues %s, error %r" %
          (hexes(stops), hexes(fake.step_flags), hexes(flags_c), error), flush=True)
    res.add("if_masked_in_step", error is None and fake.step_flags and stops == [0x1000, 0x2000] and
            all(not f & EFLAGS_IF for f in fake.step_flags),
            "eflags at the steps %s, stops %s, error %r" % (hexes(fake.step_flags), hexes(stops), error))
    res.add("if_restored", len(flags_c) == 2 and flags_c[1] == 0x246,
            "eflags when the guest continued %s, want 0x246" % hexes(flags_c))

    bad = []
    for name, code in (("pushf", b"\x9c"), ("popf", b"\x9d"), ("iretd", b"\xcf"), ("sti", b"\xfb"),
                       ("cli", b"\xfa"), ("int3", b"\xcc"), ("int 0x80", b"\xcd\x80"), ("into", b"\xce"),
                       ("icebp", b"\xf1"), ("hlt", b"\xf4"), ("pop ss", b"\x17"), ("mov ss, ax", b"\x8e\xd0"),
                       ("o16 popf", b"\x66\x9d")):
        refused, error, fake = session(lambda r, f: (r.cont(5), attempt(lambda: r.cont(5)))[1],
                                       eflags=0x202, code=code)
        if refused is None or "IF set" not in refused or fake.steps or len(fake.stops) != 1:
            bad.append("%s: %r %r, %d steps, %d continues" % (name, refused, error, fake.steps, len(fake.stops)))
    report(res, "if_opcodes_refused", not bad,
           "; ".join(bad) or "pushf popf iretd sti cli int3 int into icebp hlt pop ss, mov ss, o16 popf refused")

    stops, error, fake = session(two_conts, eflags=0x202, code=b"\x8e\xd8")    # mov ds, ax
    report(res, "if_mov_ds_steps", error is None and stops == [0x1000, 0x2000] and fake.steps == 1 and
           all(not f & EFLAGS_IF for f in fake.step_flags),
           "stops %s, %d steps, eflags at the steps %s, error %r" %
           (hexes(stops), fake.steps, hexes(fake.step_flags), error))

    # The step lands in another CS (TCG: an exception handler's first instruction).
    def cs_change(r, fake):
        r.cont(5)
        fake.step_cs = 0x08
        return attempt(lambda: r.cont(5))
    refused, error, fake = session(cs_change, eflags=0x202)
    report(res, "if_cs_change_refused", refused is not None and "exception handler" in refused and
           len(fake.stops) == 1 and not fake.live[EFLAGS] & EFLAGS_IF,
           "error %r, %d continues, eflags %#x (the handler's, IF clear), error %r" %
           (refused, len(fake.stops), fake.live[EFLAGS], error))

    # A step off that never moves fails; the guest gets IF back, unless the step ended in another
    # CS (then the flags are a handler's).
    def stuck(r, fake, cs=None):
        r.cont(5)
        fake.stalls = MAX_STEPS
        fake.step_cs = cs
        return attempt(lambda: r.cont(5))
    refused, error, fake = session(stuck, eflags=0x246)
    refused2, error2, fake2 = session(lambda r, f: stuck(r, f, 0x08), eflags=0x246)
    report(res, "if_restored_after_failure", refused is not None and "stepping off" in refused and
           fake.live[EFLAGS] == 0x246 and refused2 is not None and fake2.live[EFLAGS] == 0x46,
           "error %r, eflags after it %#x (want 0x246); in another CS: error %r, eflags %#x (want 0x46), "
           "errors %r %r" % (refused, fake.live[EFLAGS], refused2, fake2.live[EFLAGS], error, error2))

    # A nop on the last mapped byte (0xffff): nothing is mapped after it.
    def last_byte(r, fake):
        r.remove_breakpoint(0x1000)
        r.remove_breakpoint(0x2000)
        r.add_breakpoint(0xFFFF)
        r.add_breakpoint(0x20000)
        return [r.cont(5), r.cont(5)]
    stops, error, fake = session(last_byte, eflags=0x246)
    report(res, "if_last_byte_steps", error is None and stops == [0xFFFF, 0x20000] and fake.steps == 1,
           "stops %s, %d steps, error %r" % (hexes(stops), fake.steps, error))

    stops, error, fake = session(two_conts, code=b"\x9c")      # pushf with IF clear: an ordinary step
    report(res, "if_clear_steps", error is None and stops == [0x1000, 0x2000] and fake.steps == 1,
           "stops %s, %d steps, error %r" % (hexes(stops), fake.steps, error))


def main():
    res = Results(sys.argv[1])
    for cases in (step_off_cases, kvm_cases, breakpoint_cases, moved_eip_case, if_cases):
        try:
            cases(res)
        except Exception as e:      # a broken case must not hide the others
            res.add(cases.__name__, False, "%s: %s" % (type(e).__name__, e))
    return 0 if all(line.startswith("PASS") for line in res.lines) else 1


if __name__ == "__main__":
    sys.exit(main())
