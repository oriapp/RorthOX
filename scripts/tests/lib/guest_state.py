"""Guest state at a known point: which tasks and processes are left, which one is the shell and
which process owns the keyboard. The screen shows what a program printed, not that it has ended.

guest_state.py <gdb socket> <results file> <tag> name=hex... option=value...

The names are the kernel symbols and offsets from kernel-symbols.sh: isr80h_handler, current_task,
task_head, task_tail, current_process, processes, max (process slots), next, prev, tid (the task
id), process, directory (struct task), id, task, filename (struct process) and cs (struct interrupt_frame). The
getkey syscall number and the ring-3 code selector come from build/config/rorthox-config.h.

The rendezvous: QEMU's gdbstub stops the guest at isr80h_handler for a getkey syscall made by the
task named by at= (the first task in the list, the shell, or the last one), from ring 3 (the
frame's cs) with that task's page directory in CR3. A task that asks for a key has finished
everything it did before, and the kernel runs with interrupts off, so no other task is halfway
through a syscall (an exit, a print) either. If the task list doesn't have tasks= entries yet (a
program is still ending), the guest runs on for a moment without breakpoints and is stopped
again, until timeout=. The same happens after a stop in another task: with the guest stopped at
every syscall, the virtual clock hardly moves and the task in the way would keep its time slice.
At the rendezvous, with the guest stopped, it checks:
  <tag>_rendezvous      the stop above (getkey, from ring 3, the task's own CR3)
  <tag>_list_consistent next/prev links, task_tail, every task's process points back to the task
                        and sits in its slot, the slots hold exactly the listed processes,
                        current_task is in the list
  <tag>_task_count      tasks= tasks in the list
  <tag>_keyboard_owner  owner=first|last: current_process is that task's process
  <tag>_same_shell      shell=<file>: the first task, its process and its task id are the ones
                        saved there (task ids only go up, so a shell started again has a new one)
Options: save=<file> writes the first task, its process and its task id; screen=<file> writes the VGA text
screen as it is at the stop (the screen can't be halfway through a scroll there).
Then it detaches and the guest runs on. Run it only while no other debugger is attached.
"""

import struct
import sys
import time

from rsp import Results, Rsp, RspError, read_config

RUN_FOR = 0.05     # seconds the guest runs on between two tries


def walk(r, k):
    """[(task, process, page directory chunk, task id)] from task_head, and the problems found on the way."""
    tasks, bad = [], []
    prev, task = 0, r.read32(k["task_head"])
    while task:
        if len(tasks) == k["max"] + 1:
            bad.append("more than %d tasks in the list (a cycle?)" % k["max"])
            break
        process = r.read32(task + k["process"])
        if r.read32(task + k["prev"]) != prev:
            bad.append("task %#x prev %#x, expected %#x" % (task, r.read32(task + k["prev"]), prev))
        if not process or r.read32(process + k["task"]) != task:
            bad.append("task %#x: process %#x does not point back to it" % (task, process))
        tasks.append((task, process, r.read32(task + k["directory"]), r.read32(task + k["tid"])))
        prev, task = task, r.read32(task + k["next"])
    return tasks, bad


def name(r, k, process):
    return r.read(process + k["filename"], 64).split(b"\0")[0].decode("latin-1") if process else "-"


def run_for(r, seconds):
    """Let the guest run without breakpoints for a moment, then stop it again."""
    for addr in list(r.breakpoints):
        r.remove_breakpoint(addr)
    r._send_packet("c")
    time.sleep(seconds)
    r._send_raw(b"\x03")
    reply = r._read_packet(10)
    if not reply[:1] in ("S", "T"):
        raise RspError("unexpected stop reply %r" % reply)


def vga_text(data):
    rows = []
    for i in range(0, 4000, 160):
        rows.append("".join(chr(c) if 32 <= c < 127 else " " for c in data[i:i + 160:2]).rstrip())
    return "\n".join(rows) + "\n"


def main():
    sock, result_path, tag = sys.argv[1:4]
    args = dict(a.split("=", 1) for a in sys.argv[4:])
    k = {n: int(args.pop(n), 16) for n in ("isr80h_handler", "current_task", "task_head", "task_tail",
                                            "current_process", "processes", "max", "next", "prev", "tid", "process",
                                            "directory", "id", "task", "filename", "cs")}
    config = read_config("build/config/rorthox-config.h")
    k["getkey"], k["user_cs"] = config["SYSCALL_GETKEY"], config["USER_CODE_SEGMENT"]
    at = args.get("at", "first")
    want = int(args.get("tasks", "1"))
    timeout = float(args.get("timeout", "20"))
    res = Results(result_path)
    r = None
    deadline = time.monotonic() + timeout
    seen = "no getkey stop yet"
    try:
        r = Rsp(sock)
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise RspError("no rendezvous within %d s: %s" % (timeout, seen))
            if k["isr80h_handler"] not in r.breakpoints:
                r.add_breakpoint(k["isr80h_handler"])
            eip = r.cont(left)
            if eip != k["isr80h_handler"]:
                # A breakpoint another debugger left behind: the stack is not the handler's.
                raise RspError("stopped at %#x, not at isr80h_handler" % eip)
            esp = r.reg("esp")
            command, frame = struct.unpack("<II", r.read(esp + 4, 8))
            current = r.read32(k["current_task"])
            tasks, bad = walk(r, k)
            target = (tasks[0] if at == "first" else tasks[-1]) if tasks else None
            if command == k["getkey"] and target and current == target[0]:
                if len(tasks) == want:
                    break
                seen = "%d tasks %s, expected %d" % (len(tasks), [name(r, k, t[1]) for t in tasks], want)
            run_for(r, RUN_FOR)

        cs = r.read32(frame + k["cs"])
        cr3 = r.control_registers()["cr3"]
        directory = r.read32(target[2])
        res.add("%s_rendezvous" % tag, cs == k["user_cs"] and cr3 & 0xFFFFF000 == directory,
                "getkey of task %#x: frame cs %#x, cr3 %#x, task directory %#x" % (current, cs, cr3, directory))

        owner = r.read32(k["current_process"])
        slots = struct.unpack("<%dI" % k["max"], r.read(k["processes"], 4 * k["max"]))
        tail = r.read32(k["task_tail"])
        if tail != tasks[-1][0]:
            bad.append("task_tail %#x, last task %#x" % (tail, tasks[-1][0]))
        for t, p, _, _ in tasks:
            pid = struct.unpack("<H", r.read(p + k["id"], 2))[0] if p else -1
            if not (0 <= pid < k["max"]) or slots[pid] != p:
                bad.append("process %#x (id %d) not in its slot" % (p, pid))
        live = sorted(s for s in slots if s)
        if live != sorted(t[1] for t in tasks):
            bad.append("slots %s, processes in the list %s" %
                       (["%#x" % s for s in live], ["%#x" % t[1] for t in tasks]))
        print("  %s: tasks %s, current %#x, keyboard owner %#x" %
              (tag, ["%#x/%#x id %d %s" % (t, p, i, name(r, k, p)) for t, p, _, i in tasks], current, owner),
              flush=True)
        res.add("%s_list_consistent" % tag, not bad, "; ".join(bad))
        res.add("%s_task_count" % tag, len(tasks) == want, "%d tasks, expected %d" % (len(tasks), want))
        if "owner" in args:
            expected = tasks[0][1] if args["owner"] == "first" else tasks[-1][1]
            res.add("%s_keyboard_owner" % tag, owner == expected,
                    "current_process %#x, expected %#x" % (owner, expected))
        if "save" in args:
            with open(args["save"], "w") as f:
                f.write("%#x %#x %#x\n" % (tasks[0][0], tasks[0][1], tasks[0][3]))
        if "shell" in args:
            with open(args["shell"]) as f:
                shell = tuple(int(x, 16) for x in f.read().split())
            first = (tasks[0][0], tasks[0][1], tasks[0][3])
            res.add("%s_same_shell" % tag, first == shell,
                    "first task %#x/%#x id %d, the shell was %#x/%#x id %d" % (first + shell))
        if "screen" in args:
            with open(args["screen"], "w") as f:
                f.write(vga_text(r.read(0xB8000, 4000)))
        r.detach()
    except (RspError, OSError) as e:
        res.add("%s_session" % tag, False, str(e))
        try:
            if r:
                r.detach()
        except Exception:
            pass
    return 0 if res.lines and all(line.startswith("PASS") for line in res.lines) else 1


if __name__ == "__main__":
    sys.exit(main())
