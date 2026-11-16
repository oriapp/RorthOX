"""Heap table copies for the out-of-memory test programs (programs/heapsnapshot/heap_snapshot.h).

heap_snapshot.py <gdb socket> <program.elf> <results file> <ready file> <syscall checks> name=hex...

The names are the kernel symbols and offsets from kernel-symbols.sh (current_task, task_head,
next, process, directory, filename) and the heap table: table (its address) and blocks (its size).

Attached while the program waits for its first key. Breaks at heap_snapshot_wait and
heap_snapshot_end in the program, lets the guest run and writes the ready file (the test script
sends the key then). At every stop, with the VM stopped, it walks the task list from task_head (at
most 12 tasks) and finds the task that runs, current_task. Programs share virtual addresses, so a
stop in another task (the shell) is skipped; more than MAX_OTHER_STOPS of them fail the session.
  - The first stop in the program needs exactly one task of that program; its task, process and
    page directory are kept, and every later stop must be in the same one.
  - Every stop runs in ring 3 with CR3 = the page directory of that task.
  - heap_snapshot_wait(buffer, ack, sequence, size, wrapper): buffer and ack must be the
    program's heap_copy and heap_ack, size the table size and sequence one more than the last
    request. The ELF symbol sizes must match too (heap_copy the table size, heap_ack 4 bytes):
    debugger writes ignore R/W and U/S, so nothing else stops a write past a smaller buffer. It reads the heap table, writes it into the buffer through the task's own mapping
    (the gdbstub translates through the current CR3), then writes the sequence number into ack.
    A nonzero wrapper must be rorthox_system, print or printc: the next stop in the program must
    be there, and the heap table must then be exactly the copy just given (nothing was allocated
    or freed in between). Then that breakpoint is removed again. This covers the wrapper's entry,
    not its instructions up to int 0x80; the program's later checks catch a lasting change there.
  - heap_snapshot_end(requests): requests must be the number of copies given, and the number of
    wrapper checks the count passed on the command line. Then it detaches.
With kernel=<relinked kernel ELF> and sites=<file> (and enomem=<hex>, the ENOMEM value), it also
records where the kernel heap runs out: from each wrapper check to the program's next stop, a
breakpoint sits on the only "mov eax, -ENOMEM" of heap_get_start_block (reached only when no free
run is big enough). At each hit the stack is walked for the callers, and the first caller outside
the heap's own allocation path (ALLOCATOR) is the site. The file gets "failures=<n>", one
"failure <i> site=<function> callers=<a<b<...>" line per failure and a "failure <i> caller <name>"
line for each function in that chain.
With from=<kernel function> as well, the breakpoint on the failure path is only set once that
function is entered (after the wrapper check, before the program's next stop); the function must
lie on another 4 KiB page. Under TCG a breakpoint makes QEMU run every instruction of its page on
its own, and the heap's search loop shares a page with the failure path: watched from the wrapper
on, a syscall that builds a page directory (1024 allocations) runs slow enough to miss the test's
deadlines on a busy host. The file then also gets "from <function> reached=<n>".
All reads (task list, heap table) go through the CR3 of the stopped task, so the kernel must stay
mapped (supervisor-only is enough) in task page directories; if it isn't, the session fails.
"""

import bisect
import struct
import sys

from rsp import Results, Rsp, RspError, read_elf_symbols

MAX_TASKS = 12
MAX_OTHER_STOPS = 20
TIMEOUT = 90
WRAPPERS = ("rorthox_system", "print", "printc")
# kernel=<relinked kernel ELF> and sites=<file>: record where the kernel heap runs out (see sites()).
PATH_ARGS = ("kernel", "sites", "from")
# The kernel heap's own allocation path: the failing allocation site is the first caller outside it.
ALLOCATOR = ("heap_get_start_block", "heap_malloc_blocks", "heap_malloc", "kmalloc", "kzalloc")
MAX_FRAMES = 8


class Kernel:
    """Functions of the relinked kernel ELF, to name the callers at a stop in the heap code."""

    def __init__(self, path):
        self.symbols = read_elf_symbols(path, sizes=True)
        self.functions = sorted((value, size, name) for name, (value, size) in self.symbols.items() if size)

    def function_at(self, addr):
        """(start, name) of the sized symbol holding addr, or None."""
        i = bisect.bisect_right(self.functions, (addr, 0xFFFFFFFF, "")) - 1
        if i >= 0 and self.functions[i][0] <= addr < self.functions[i][0] + self.functions[i][1]:
            return self.functions[i][0], self.functions[i][2]
        return None

    def failure_address(self, r, enomem):
        """The one "mov eax, -ENOMEM" in heap_get_start_block: only reached when no free run is big
        enough, so a breakpoint there stops exactly when a kernel heap allocation fails."""
        start, size = self.symbols["heap_get_start_block"]
        code = r.read(start, size)
        insn = b"\xb8" + struct.pack("<i", -enomem)
        found = [i for i in range(len(code)) if code.startswith(insn, i)]
        if len(found) != 1:
            raise RspError("heap_get_start_block has %d \"mov eax, -ENOMEM\", want 1" % len(found))
        return start + found[0]

    def callers(self, r):
        """Function names from heap_get_start_block outwards. The kernel has no frame pointers, so
        walk the stack: a return address is a value just after a direct call (E8 rel32) to the
        function found last, and it lies in its caller."""
        chain = ["heap_get_start_block"]
        target = self.symbols["heap_get_start_block"][0]
        esp = r.reg("esp")
        stack = r.read(esp, 1024)
        for off in range(0, len(stack) - 3, 4):
            value, = struct.unpack_from("<I", stack, off)
            caller = self.function_at(value)
            if caller is None or value - 5 < caller[0]:
                continue
            call = r.read(value - 5, 5)
            if call[0] == 0xE8 and (value + struct.unpack_from("<i", call, 1)[0]) & 0xFFFFFFFF == target:
                chain.append(caller[1])
                target = caller[0]
                if len(chain) == MAX_FRAMES:
                    break
        return chain


def walk(r, k):
    """[(task, process, filename, page directory chunk)] from task_head, in list order."""
    tasks = []
    task = r.read32(k["task_head"])
    while task:
        if len(tasks) == MAX_TASKS:
            raise RspError("more than %d tasks in the list (a cycle?)" % MAX_TASKS)
        process = r.read32(task + k["process"])
        name = r.read(process + k["filename"], 64).split(b"\0")[0].decode("latin-1") if process else ""
        tasks.append((task, process, name, r.read32(task + k["directory"])))
        task = r.read32(task + k["next"])
    return tasks


def main():
    sock, elf, result_path, ready_path, want_checks = sys.argv[1:6]
    want_checks = int(want_checks)
    paths = dict(a.split("=", 1) for a in sys.argv[6:] if a.split("=", 1)[0] in PATH_ARGS)
    k = {name: int(value, 16) for name, value in (a.split("=", 1) for a in sys.argv[6:]) if name not in PATH_ARGS}
    program = "/" + elf.rsplit("/", 1)[-1]
    res = Results(result_path)
    sized = read_elf_symbols(elf, sizes=True)
    symbols = {name: value for name, (value, _) in sized.items()}
    wait_at, end_at = symbols["heap_snapshot_wait"], symbols["heap_snapshot_end"]
    (copy_at, copy_size), (ack_at, ack_size) = sized["heap_copy"], sized["heap_ack"]
    wrappers = {symbols[name]: name for name in WRAPPERS if name in symbols}
    table, blocks = k["table"], k["blocks"]
    print("  heap_snapshot_wait %#x, heap_snapshot_end %#x, heap_copy %#x + %d, heap_ack %#x + %d, table %#x + %d" %
          (wait_at, end_at, copy_at, copy_size, ack_at, ack_size, table, blocks), flush=True)

    r = Rsp(sock)
    for addr in (wait_at, end_at):
        r.add_breakpoint(addr)
    res.add("attached", True)
    kernel = Kernel(paths["kernel"]) if "kernel" in paths else None
    fail_at = None      # the heap's failure path, watched from a wrapper's call to the program's next stop
    armed = False
    from_at = None      # with from=: the failure path is watched only from this kernel function on
    arming = False      # the breakpoint on from_at is set
    reached = 0         # stops at from_at
    failures = []       # (site, callers) of every kernel heap allocation that failed while watched

    def ready():
        with open(ready_path, "w") as f:
            f.write("running\n")

    me = None           # (task, process, chunk) fixed at the first stop in the program
    given = 0           # copies written and acknowledged
    last = None         # the last copy given
    pending = None      # wrapper address armed by the last request
    checks = 0
    other_stops = 0
    ended = None
    failed = None
    try:
        if kernel:
            fail_at = kernel.failure_address(r, k["enomem"])
            print("  kernel heap allocations fail at %#x" % fail_at, flush=True)
            if "from" in paths:
                if paths["from"] not in kernel.symbols:
                    raise RspError("no kernel function %s" % paths["from"])
                from_at = kernel.symbols[paths["from"]][0]
                if from_at >> 12 == fail_at >> 12:
                    raise RspError("%s (%#x) shares a page with the failure path" % (paths["from"], from_at))
                print("  watched from %s at %#x" % (paths["from"], from_at), flush=True)
        first = True
        while True:
            eip = r.cont(TIMEOUT, ready if first else None)
            first = False
            if armed and eip == fail_at:
                chain = kernel.callers(r)
                site = next((name for name in chain if name not in ALLOCATOR), "unknown")
                failures.append((site, chain))
                print("  a kernel heap allocation failed in %s (%s)" % (site, " < ".join(chain)), flush=True)
                continue
            if arming and eip == from_at:
                r.remove_breakpoint(from_at)
                arming = False
                reached += 1
                r.add_breakpoint(fail_at)
                armed = True
                continue
            current = r.read32(k["current_task"])
            tasks = walk(r, k)
            running = [t for t in tasks if t[0] == current]
            if not running or not running[0][2].endswith(program):
                other_stops += 1
                if other_stops > MAX_OTHER_STOPS:
                    failed = ("other_stops", "%d stops in other tasks, the last at %#x in %s" %
                              (other_stops, eip, running[0][2] if running else "a task not in the list"))
                    break
                continue
            if me is None:
                mine = [(t[0], t[1], t[3]) for t in tasks if t[2].endswith(program)]
                if len(mine) != 1:
                    failed = ("identity", "%d %s tasks, want 1" % (len(mine), program))
                    break
                me = mine[0]
                print("  %s task %#x, process %#x" % (program, me[0], me[1]), flush=True)
            this = (running[0][0], running[0][1], running[0][3])
            if this != me:
                failed = ("identity", "stop at %#x in task %#x (process %#x, directory %#x), want %#x "
                          "(process %#x, directory %#x)" % ((eip,) + this + me))
                break
            cs = r.reg("cs")
            cr3 = r.control_registers()["cr3"]
            directory = r.read32(me[2])
            if cs & 3 != 3 or cr3 & 0xFFFFF000 != directory:
                failed = ("task_context", "stop at %#x: cs %#x cr3 %#x, task directory %#x" % (eip, cs, cr3, directory))
                break
            if pending is not None:
                # The first stop after a request with a wrapper must be that wrapper.
                if eip != pending:
                    failed = ("unchanged", "stop at %#x before %s was called" % (eip, wrappers[pending]))
                    break
                now = r.read(table, blocks)
                changed = sum(1 for a, b in zip(now, last) if a != b)
                print("  %s called: %d heap table entries changed since copy %d" % (wrappers[pending], changed, given),
                      flush=True)
                if changed:
                    failed = ("unchanged", "%d heap table entries changed between copy %d and %s" %
                              (changed, given, wrappers[pending]))
                    break
                r.remove_breakpoint(pending)
                pending = None
                checks += 1
                if from_at is not None:
                    r.add_breakpoint(from_at)
                    arming = True
                elif fail_at is not None:
                    # Until the program's next stop (the syscall has returned by then).
                    r.add_breakpoint(fail_at)
                    armed = True
                continue
            if arming:
                r.remove_breakpoint(from_at)
                arming = False
            if armed:
                r.remove_breakpoint(fail_at)
                armed = False
            if eip == end_at:
                ended = r.read32(r.reg("esp") + 4)
                break
            if eip != wait_at:
                failed = ("requests", "unexpected stop at %#x" % eip)
                break
            buffer, ack, sequence, size, wrapper = struct.unpack("<5I", r.read(r.reg("esp") + 4, 20))
            if (buffer, ack, sequence, size) != (copy_at, ack_at, given + 1, blocks) or \
                    (wrapper and wrapper not in wrappers):
                failed = ("requests", "request %#x %#x %d %d %#x, want %#x %#x %d %d and a known wrapper" %
                          (buffer, ack, sequence, size, wrapper, copy_at, ack_at, given + 1, blocks))
                break
            if (copy_size, ack_size) != (blocks, 4):
                failed = ("requests", "heap_copy is %d bytes, heap_ack %d, want %d and 4" %
                          (copy_size, ack_size, blocks))
                break
            last = r.read(table, blocks)
            r.write(buffer, last)
            r.write32(ack, sequence)
            given += 1
            if wrapper:
                r.add_breakpoint(wrapper)
                pending = wrapper
    except RspError as e:
        failed = ("session", str(e))
    finally:
        try:
            r.detach()
        except (RspError, OSError):
            pass

    print("  %d copies, %d wrapper checks, %d stops in other tasks, heap_snapshot_end(%s)" %
          (given, checks, other_stops, ended), flush=True)
    if "sites" in paths:
        with open(paths["sites"], "w") as f:
            f.write("failures=%d\n" % len(failures) if fail_at is not None else "failures=not watched\n")
            if from_at is not None:
                f.write("from %s reached=%d\n" % (paths["from"], reached))
            for i, (site, chain) in enumerate(failures, 1):
                f.write("failure %d site=%s callers=%s\n" % (i, site, "<".join(chain)))
                for name in chain:
                    f.write("failure %d caller %s\n" % (i, name))
    if failed:
        res.add(failed[0], False, failed[1])
    for ident in ("other_stops", "identity", "task_context", "requests", "unchanged"):
        if not failed or failed[0] != ident:
            res.add(ident, me is not None, "no stop in %s" % program)
    res.add("every_copy", ended is not None and ended == given and given > 0,
            "heap_snapshot_end(%s), %d copies given" % (ended, given))
    res.add("wrapper_checks", checks == want_checks, "%d wrapper checks, want %d" % (checks, want_checks))
    return 0 if all(line.startswith("PASS") for line in res.lines) else 1


if __name__ == "__main__":
    sys.exit(main())
