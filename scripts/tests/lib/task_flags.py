"""task-flags.sh, debugger side: task_return must build the iret EFLAGS from the task's saved flags.

frame <gdb socket> <kernel.map> <results file>
    QEMU waits at reset. Break at the first task_return (the shell's first start), then for each test
    pattern write it into the saved registers.flags (with distinct values in the other fields),
    single-step the real task_return up to its iretd and check the frame it built:
        EFLAGS = (saved & 0x00250dd5) | 0x202
    (CF PF AF ZF SF TF DF OF RF AC ID kept, IF and bit 1 set, IOPL, NT, VM, VIF, VIP and the
    reserved bits clear), EIP/CS/ESP/SS and the general registers from the struct, and DS..GS
    loaded with SS. The iretd is never executed: after each pattern the registers, the stack and
    the struct are put back, and the debugger detaches at task_return so the boot goes on as usual.

entry <gdb socket> <kernel.map> <results file> <flagstest.elf>
    Attached after flagstest got its key: wait until it spins in wait_for_debugger, release it, then
    break at interrupt_handler and isr80h_handler. For every entry from ring 3 whose saved user
    EFLAGS (top of the TSS stack) has DF set, the live DF on entry to C must be clear. Needs one
    such timer-vector entry and one syscall entry. Also breaks at task_return: each of the four
    timer-wait patterns must come back through it with the saved EIP inside the wait loop.
"""

import struct
import sys
import time

from rsp import EFLAGS_DF, REGS, Results, Rsp, RspError, read_config, read_elf_symbols, read_map

KEEP = 0x00250DD5
FORCE = 0x00000202
PATTERNS = [0x00000000, 0xFFFFFFFF] + [1 << i for i in range(32)] + [0x55555555, 0xAAAAAAAA]
# struct registers: edi esi ebp ebx edx ecx eax ip cs flags esp ss
FIELDS = ["edi", "esi", "ebp", "ebx", "edx", "ecx", "eax", "ip", "cs", "flags", "esp", "ss"]
GATE_VALUE = 0x67617266
TIMER_PATTERNS = [0x00240CD5, 0x00000000, 0x00040491, 0x00200844]   # flagstest.c patterns
GATE_CODE = b"\x3d" + struct.pack("<I", GATE_VALUE) + b"\x75\xf9"   # cmp eax, GATE_VALUE; jne .spin


def frame_mode(sock, map_path, result_path):
    res = Results(result_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    symbols, _ = read_map(map_path)
    task_return = symbols["task_return"]

    r = Rsp(sock)
    r.add_breakpoint(task_return)
    eip = r.cont(60)
    r.remove_breakpoint(task_return)
    ok = eip == task_return and r.reg("cs") == config["KERNEL_CODE_SELECTOR"]
    res.add("at_task_return", ok, "stopped at %#x" % eip)
    if not ok:
        raise SystemExit(1)

    entry = r.regs()
    arg = r.read32(entry["esp"] + 4)
    saved = r.read(arg, 48)
    original = dict(zip(FIELDS, struct.unpack("<12I", saved)))
    stack_lo = entry["esp"] - 256
    stack = r.read(stack_lo, 256)
    print("  task_return(%#x): ip %#x cs %#x flags %#x esp %#x ss %#x" %
          (arg, original["ip"], original["cs"], original["flags"], original["esp"], original["ss"]), flush=True)
    res.add("first_task_flags_zero", original["flags"] == 0, "saved flags %#x" % original["flags"])

    failures = {"frame_flags": [], "frame_fields": [], "frame_registers": [], "frame_segments": [], "reached_iretd": []}
    for k, pattern in enumerate(PATTERNS):
        values = dict(original)
        values.update({"edi": 0x11110000 + k, "esi": 0x22220000 + k, "ebp": 0x33330000 + k,
                       "ebx": 0x44440000 + k, "edx": 0x55550000 + k, "ecx": 0x66660000 + k,
                       "eax": 0x77770000 + k, "ip": 0x00401000 + 16 * k, "esp": 0x003FE000 - 4 * k,
                       "flags": pattern})
        r.write(arg, struct.pack("<12I", *[values[f] for f in FIELDS]))
        r.set_regs(entry)

        for _ in range(200):
            if r.read(r.reg("eip"), 1) == b"\xcf":    # iretd
                break
            r.step()
        else:
            failures["reached_iretd"].append("pattern %#010x: no iretd within 200 steps" % pattern)
            continue

        now = r.regs()
        ip, cs, flags, esp, ss = struct.unpack("<5I", r.read(now["esp"], 20))
        want = (pattern & KEEP) | FORCE
        if flags != want:
            failures["frame_flags"].append("saved %#010x: iret EFLAGS %#010x, expected %#010x" % (pattern, flags, want))
        got = {"ip": ip, "cs": cs, "esp": esp, "ss": ss}
        bad = ["%s=%#x/%#x" % (f, got[f], values[f]) for f in got if got[f] != values[f]]
        if bad:
            failures["frame_fields"].append("saved %#010x: %s" % (pattern, " ".join(bad)))
        bad = ["%s=%#x/%#x" % (f, now[f], values[f]) for f in FIELDS[:7] if now[f] != values[f]]
        if bad:
            failures["frame_registers"].append("saved %#010x: %s" % (pattern, " ".join(bad)))
        bad = ["%s=%#x" % (s, now[s]) for s in ("ds", "es", "fs", "gs") if now[s] != values["ss"]]
        if bad:
            failures["frame_segments"].append("saved %#010x: %s" % (pattern, " ".join(bad)))

        # Rewind to the entry of task_return for the next pattern.
        r.write(stack_lo, stack)
        r.set_regs(entry)

    for name, items in failures.items():
        res.add(name, not items, "%d of %d patterns, first: %s" % (len(items), len(PATTERNS), items[0]) if items else "")

    r.write(arg, saved)
    r.write(stack_lo, stack)
    r.set_regs(entry)
    if [r.reg(n) for n in REGS] != [entry[n] for n in REGS] or r.read(arg, 48) != saved:
        raise SystemExit("could not put task_return's entry state back")
    r.detach()


def entry_mode(sock, map_path, result_path, program_path):
    res = Results(result_path)
    config = read_config("build/config/rorthox-config.h", "build/config/rorthox-config.inc")
    symbols, _ = read_map(map_path)
    handlers = {symbols["interrupt_handler"]: "timer", symbols["isr80h_handler"]: "syscall"}
    top = config["RORTHOX_KERNEL_TSS_STACK_TOP"]
    timer_vector = config["RORTHOX_PIC_MASTER_VECTOR_BASE"]
    # flagstest spins at wait_for_debugger.spin (after "xor eax, eax") until EAX = GATE_VALUE.
    gate = read_elf_symbols(program_path)["wait_for_debugger"] + 2

    r = Rsp(sock)
    r.add_breakpoint(gate)
    for _ in range(50):
        # Other tasks map their own code at the same virtual address: check the bytes.
        if r.cont(60) == gate and r.read(gate, len(GATE_CODE)) == GATE_CODE:
            break
    else:
        raise SystemExit("flagstest never reached wait_for_debugger")
    r.remove_breakpoint(gate)
    res.add("gate_reached", True)
    r.set_reg("eax", GATE_VALUE)
    for addr in handlers:
        r.add_breakpoint(addr)
    usym = read_elf_symbols(program_path)
    window = (usym["flags_after_timer.wait_tick"], usym["flags_after_timer.ticked"])
    task_return = symbols["task_return"]
    r.add_breakpoint(task_return)
    returned = {p: 0 for p in TIMER_PATTERNS}

    seen = {"timer": [], "syscall": []}
    deadline = time.monotonic() + 90
    while not (seen["timer"] and seen["syscall"] and all(returned.values())):
        left = deadline - time.monotonic()
        if left <= 0:
            break
        try:
            eip = r.cont(left)
        except RspError as e:
            print("  %s" % e, flush=True)
            break
        if eip == task_return:
            arg = r.read32(r.reg("esp") + 4)
            ip, _, flags = struct.unpack("<III", r.read(arg + 28, 12))
            if window[0] <= ip < window[1] and flags & 0x00240CD5 in returned:
                returned[flags & 0x00240CD5] += 1
            continue
        kind = handlers.get(eip)
        if kind is None:
            raise SystemExit("stopped at %#x, not at a handler" % eip)
        # CPU frame of an entry from ring 3, at the top of the TSS stack: eip cs eflags esp ss.
        user_cs, user_flags = struct.unpack("<II", r.read(top - 16, 8))
        if user_cs != config["USER_CODE_SEGMENT"] or not user_flags & EFLAGS_DF:
            continue
        if kind == "timer" and r.read32(r.reg("esp") + 4) != timer_vector:
            continue
        seen[kind].append((user_flags, r.reg("eflags")))

    res.add("timer_return_every_pattern", all(returned.values()),
            " ".join("%#x:%d" % (p, n) for p, n in returned.items()))
    for kind in ("timer", "syscall"):
        hits = seen[kind]
        res.add("%s_entry_seen" % kind, bool(hits), "no %s entry from ring 3 with DF set" % kind)
        bad = [live for _, live in hits if live & EFLAGS_DF]
        res.add("%s_entry_df_clear" % kind, bool(hits) and not bad,
                "live EFLAGS %#x on entry to C (user EFLAGS %#x)" % (bad[0], hits[0][0]) if bad else "no entry")
    r.detach()


if __name__ == "__main__":
    if sys.argv[1] == "frame":
        frame_mode(*sys.argv[2:5])
    else:
        entry_mode(*sys.argv[2:6])
