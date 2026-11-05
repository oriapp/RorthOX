"""sleep.sh, debugger side: the peer runs while the sleeper sleeps, seen from the host.

sleep_peer.py <gdb socket> <libtest.elf> <results file> <samples file> <ready file> name=hex...

The names are the kernel symbols and offsets from kernel-symbols.sh: current_task, task_head,
jiffies, state, wake, next, process, directory (struct task) and filename (struct process).

Attached while "libtest.elf sleep" waits for its key. Breaks at libtest's lt_peer_sample,
lt_peer_done and lt_sleeper_woke, lets the guest run and writes the ready file (sleep.sh sends the
key then). At every stop, with the VM stopped, it walks the task list from task_head (at most 12
tasks) and finds the task that runs, current_task. Programs share virtual addresses, so only a
stop in a libtest.elf task counts; a stop in any other task fails the session at once if it comes
before the first libtest.elf stop, or if there are more than MAX_OTHER_STOPS of them.
  - The first libtest.elf stop needs exactly two libtest.elf tasks in the list: the sleeper, then
    the peer it started (tasks are added at the tail). Their process and page directory are kept;
    every later stop must find the same ones (a sleeper task that no longer matches is "gone").
  - Every stop runs in ring 3 with CR3 = the page directory of the task that runs.
  - lt_peer_sample and lt_peer_done must stop in the peer. At lt_peer_sample it reads the sample
    number and the tick the peer saw from the stack, the kernel's jiffies, and the sleeper's state
    and wake_tick, writes "sample <n> <tick> <jiffies> <state> <wake>" to the samples file (state
    "gone" once the sleeper has ended), and acknowledges the sample by writing n into the peer's
    lt_peer_ack.
  - lt_sleeper_woke must stop in the sleeper, once, after its sleep(1000): its state runnable (0)
    and jiffies at or past its wake_tick.
At lt_peer_done the samples must be 1..n with no gap, n the count the peer passes. Then it
detaches and the guest runs on. sleep.sh compares the samples with the sleeper's own window.
"""

import struct
import sys

from rsp import Results, Rsp, RspError, read_elf_symbols

MAX_TASKS = 12
MAX_OTHER_STOPS = 5
TIMEOUT = 60
TASK_RUNNABLE = 0


def walk(r, k):
    """[(task, process, state, wake, filename, page directory chunk)] from task_head, in list order."""
    tasks = []
    task = r.read32(k["task_head"])
    while task:
        if len(tasks) == MAX_TASKS:
            raise RspError("more than %d tasks in the list (a cycle?)" % MAX_TASKS)
        process = r.read32(task + k["process"])
        name = r.read(process + k["filename"], 64).split(b"\0")[0].decode("latin-1") if process else ""
        tasks.append((task, process, r.read32(task + k["state"]), r.read32(task + k["wake"]), name,
                      r.read32(task + k["directory"])))
        task = r.read32(task + k["next"])
    return tasks


def is_libtest(entry):
    return entry[4].endswith("/libtest.elf")


def main():
    sock, elf, result_path, samples_path, ready_path = sys.argv[1:6]
    k = {name: int(value, 16) for name, value in (a.split("=", 1) for a in sys.argv[6:])}
    res = Results(result_path)
    symbols = read_elf_symbols(elf)
    sample_at, done_at, woke_at = symbols["lt_peer_sample"], symbols["lt_peer_done"], symbols["lt_sleeper_woke"]
    ack_at = symbols["lt_peer_ack"]
    print("  lt_peer_sample %#x, lt_peer_done %#x, lt_sleeper_woke %#x, lt_peer_ack %#x" %
          (sample_at, done_at, woke_at, ack_at), flush=True)

    r = Rsp(sock)
    for addr in (sample_at, done_at, woke_at):
        r.add_breakpoint(addr)
    res.add("attached", True)

    def ready():
        with open(ready_path, "w") as f:
            f.write("running\n")

    sleeper = peer = None      # (task, process, chunk) fixed at the first libtest.elf stop
    numbers = []
    other_stops = 0
    failed = set()
    done = woke = None
    with open(samples_path, "w") as samples:
        try:
            first = True
            while True:
                eip = r.cont(TIMEOUT, ready if first else None)
                first = False
                current = r.read32(k["current_task"])
                tasks = walk(r, k)
                running = [t for t in tasks if t[0] == current]
                if not running or not is_libtest(running[0]):
                    other_stops += 1
                    if peer is None or other_stops > MAX_OTHER_STOPS:
                        res.add("other_stops", False, "stop at %#x in task %#x (%s), %d such stops%s" %
                                (eip, current, running[0][4] if running else "not in the list", other_stops,
                                 "" if peer else ", before the first libtest.elf stop"))
                        failed.add("other_stops")
                        break
                    continue
                if peer is None:
                    libtest = [(t[0], t[1], t[5]) for t in tasks if is_libtest(t)]
                    if len(libtest) != 2:
                        res.add("identity", False, "libtest.elf tasks %s, want 2" % ["%#x" % t[0] for t in libtest])
                        failed.add("identity")
                        break
                    sleeper, peer = libtest
                    print("  sleeper task %#x, peer task %#x" % (sleeper[0], peer[0]), flush=True)
                me = (running[0][0], running[0][1], running[0][5])
                want = sleeper if eip == woke_at else peer
                if me != want:
                    res.add("identity", False, "stop at %#x in task %#x (process %#x, directory %#x), want %#x "
                            "(process %#x, directory %#x)" % ((eip,) + me + want))
                    failed.add("identity")
                    break
                cs = r.reg("cs")
                cr3 = r.control_registers()["cr3"]
                directory = r.read32(me[2])
                if cs & 3 != 3 or cr3 & 0xFFFFF000 != directory:
                    res.add("task_context", False, "stop at %#x: cs %#x cr3 %#x, task directory %#x" %
                            (eip, cs, cr3, directory))
                    failed.add("task_context")
                    break
                jiffies = r.read32(k["jiffies"])
                if eip == woke_at:
                    state, wake = running[0][2], running[0][3]
                    print("  sleeper woke: state %d, wake_tick %d, jiffies %d" % (state, wake, jiffies), flush=True)
                    if woke is not None or state != TASK_RUNNABLE or (jiffies - wake) & 0xFFFFFFFF >= 0x80000000:
                        res.add("sleeper_woke", False, "stop %s: state %d, wake_tick %d, jiffies %d" %
                                ("again" if woke is not None else "once", state, wake, jiffies))
                        failed.add("sleeper_woke")
                        break
                    woke = jiffies
                    continue
                if eip == done_at:
                    done = struct.unpack("<I", r.read(r.reg("esp") + 4, 4))[0]
                    break
                number, tick = struct.unpack("<II", r.read(r.reg("esp") + 4, 8))
                found = [t for t in tasks if (t[0], t[1], t[5]) == sleeper]
                state = "%d %d" % (found[0][2], found[0][3]) if found else "gone 0"
                samples.write("sample %d %d %d %s\n" % (number, tick, jiffies, state))
                samples.flush()
                numbers.append(number)
                r.write32(ack_at, number)
        except RspError as e:
            res.add("session", False, str(e))
        finally:
            try:
                r.detach()
            except (RspError, OSError):
                pass

    print("  %d samples, %d stops in other tasks, lt_peer_done(%s)" % (len(numbers), other_stops, done), flush=True)
    for ident in ("other_stops", "identity", "task_context"):
        if ident not in failed:
            res.add(ident, peer is not None, "no stop in a libtest.elf task")
    if "sleeper_woke" not in failed:
        res.add("sleeper_woke", woke is not None, "no stop at lt_sleeper_woke")
    res.add("every_sample", done is not None and done > 0 and numbers == list(range(1, done + 1)),
            "lt_peer_done(%s), sample numbers %s" % (done, numbers[:5] + ["..."] + numbers[-5:]))
    return 0 if all(line.startswith("PASS") for line in res.lines) else 1


if __name__ == "__main__":
    sys.exit(main())
