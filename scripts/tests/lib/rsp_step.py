"""Rsp.cont() against a fake gdbstub: stepping off a breakpoint must really leave it.

rsp_step.py <results file>

QEMU can answer a single step with a SIGTRAP stop before the instruction ran, so eip is unchanged.
Rsp.cont() then continued from the breakpoint it had just put back, and the same breakpoint was
reported a second time (a heap copy request seen twice). The fake gdbstub here runs on a unix
socket in a thread. Its "CPU" only has eip: a step adds 1 unless it is told to stall or eip is on
an installed breakpoint, and "c" stops at eip if a breakpoint is installed there, else at the next
breakpoint above eip. Breakpoints at 0x1000 and 0x2000; each case continues twice from eip 0.
  normal  steps always move: stops at 0x1000 then 0x2000, one step.
  stall   the first step stalls: stops at 0x1000 then 0x2000 (never 0x1000 twice), two steps, and
          both breakpoints are installed again when the guest continues.
  stuck   no step moves: cont() raises RspError naming eip 0x1000 after at most 8 steps, and does
          not continue the guest.
"""

import os
import socket
import sys
import tempfile
import threading

from rsp import Results, Rsp, RspError

MAX_STEPS = 50      # the fake closes the connection then, so an endless retry can't hang the test


class FakeStub:
    def __init__(self, path, stalls):
        self.eip = 0
        self.breakpoints = set()
        self.stalls = stalls        # how many of the coming steps don't move
        self.steps = 0
        self.stops = []             # (eip, breakpoints installed) for every "c"
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
                while b"#" not in buf or len(buf) < buf.index(b"#") + 3:
                    data = conn.recv(4096)
                    if not data:
                        return
                    buf += data
                start = buf.index(b"$")
                end = buf.index(b"#")
                payload = buf[start + 1:end].decode("latin-1")
                buf = buf[end + 3:]
                conn.sendall(b"+")
                reply = self.handle(payload)
                if reply is None:
                    return
                body = reply.encode("latin-1")
                conn.sendall(b"$" + body + b"#" + ("%02x" % (sum(body) & 0xFF)).encode())
        except OSError:
            return
        finally:
            conn.close()

    def handle(self, p):
        if p == "?":
            return "S05"
        if p == "p8":
            return self.eip.to_bytes(4, "little").hex()
        if p[:3] in ("Z1,", "z1,"):
            addr = int(p.split(",")[1], 16)
            (self.breakpoints.add if p[0] == "Z" else self.breakpoints.discard)(addr)
            return "OK"
        if p == "s":
            self.steps += 1
            if self.steps > MAX_STEPS:
                return None
            if self.stalls > 0:
                self.stalls -= 1
            elif self.eip not in self.breakpoints:
                self.eip += 1
            return "S05"
        if p == "c":
            if self.eip not in self.breakpoints:
                above = sorted(a for a in self.breakpoints if a > self.eip)
                if not above:
                    return None
                self.eip = above[0]
            self.stops.append((self.eip, frozenset(self.breakpoints)))
            return "T05thread:01;"
        if p == "D":
            return "OK"
        return ""


def run(case, stalls):
    tmp = tempfile.mkdtemp(prefix="rsp-step.")
    path = os.path.join(tmp, "gdb")
    fake = FakeStub(path, 0)
    stops = []
    error = None
    r = None
    try:
        r = Rsp(path, timeout=5)
        r.op_timeout = 5
        r.add_breakpoint(0x1000)
        r.add_breakpoint(0x2000)
        stops.append(r.cont(5))
        fake.stalls = stalls
        stops.append(r.cont(5))
    except RspError as e:
        error = str(e)
    finally:
        if r is not None:
            r.sock.close()
        fake.server.close()
        os.unlink(path)
        os.rmdir(tmp)
    seen = [s[0] for s in fake.stops]
    print("  %s: cont() returned %s, fake stops %s, %d steps, error %r" %
          (case, [hex(s) for s in stops], [hex(s) for s in seen], fake.steps, error), flush=True)
    return stops, seen, fake, error


def main():
    res = Results(sys.argv[1])
    both = frozenset((0x1000, 0x2000))

    stops, seen, fake, error = run("normal", 0)
    res.add("normal", error is None and stops == [0x1000, 0x2000] and seen == [0x1000, 0x2000] and fake.steps == 1,
            "stops %s, %d steps, error %r" % ([hex(s) for s in seen], fake.steps, error))

    stops, seen, fake, error = run("stall", 1)
    res.add("stall_one_delivery", error is None and stops == [0x1000, 0x2000] and seen == [0x1000, 0x2000],
            "cont() returned %s, fake stops %s, error %r" % ([hex(s) for s in stops], [hex(s) for s in seen], error))
    res.add("stall_steps", fake.steps == 2, "%d steps, want 2" % fake.steps)
    res.add("stall_breakpoints_back", len(fake.stops) == 2 and fake.stops[1][1] == both,
            "breakpoints at the second continue: %s" % [sorted(hex(a) for a in s[1]) for s in fake.stops])

    stops, seen, fake, error = run("stuck", MAX_STEPS + 10)
    res.add("stuck_fails", error is not None and "0x1000" in error and "stepping off" in error,
            "error %r" % error)
    res.add("stuck_bounded", 2 <= fake.steps <= 8, "%d steps, want 2..8" % fake.steps)
    res.add("stuck_not_continued", seen == [0x1000], "fake stops %s" % [hex(s) for s in seen])
    return 0 if all(line.startswith("PASS") for line in res.lines) else 1


if __name__ == "__main__":
    sys.exit(main())
