#!/usr/bin/env bash
#
# Sleep with every task asleep (syscall 12): the kernel halts in its idle loop with interrupts on
# until a timer tick makes a task due, and IRQs that come in meanwhile return to that loop.
#
# A copy of bin/os.bin gets programs/libtest as shell.elf (and as libtest.elf), so the program the
# kernel starts is the only task. With no arguments it times its sleeps with the tick syscall (19)
# and reads no kernel memory. It sleeps 5000 ms alone, then starts
# "libtest.elf idlechild", which sleeps 6000 ms, and sleeps 3000 ms itself (both asleep, different
# deadlines), then sleeps forever. The program checks its own results (0, enough ticks, the key).
# Meanwhile the VM is stopped a few times and the QEMU monitor shows, every time:
#   - the CPU halted in ring 0 at IRQ_ON_AND_PAUSE + 2 (the cli after sti; hlt), with IF set,
#     CS 0x08, DS 0x10 and the kernel's page directory in CR3;
#   - no IRQ in service at the master PIC (isr=00), so the tick and the keyboard still get through;
#   - the task list, walked from task_head on the host, holds exactly the expected tasks (by the
#     file their process runs: shell.elf alone, then shell.elf and its libtest.elf child), each
#     still in state 1 (sleeping) before its deadline.
# During the first sleep a key and an NMI come in: the task stays asleep, its saved registers (a
# ring 3 frame, eax already 0) don't change, ticks go on and the idle stack pointer stays where it was; the
# program reads the key after its natural wake. The live bytes of IRQ_ON_AND_PAUSE are
# sti; hlt; cli; ret (fb f4 fa c3).
# A stop can land while the CPU is not halted: in the IRQ stub or handler for a tick or key that
# just ended the halt, in task_next's loop around the halt, or (first look) before the program's
# sleep call. Such a sample is kept as busy-<n>.txt and the VM runs on and is sampled again, up
# to SAMPLE_TRIES times. Every look must end in a halted sample, and the checks above (and the
# stack pointer comparisons) use only that one.
#
# Env: BOOT_TIMEOUT=60   STEP_TIMEOUT=60   SAMPLE_TRIES=20

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-60}"
SAMPLE_TRIES="${SAMPLE_TRIES:-20}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/kernel-symbols.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/sleep-idle
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
rm -rf "$OUT"/snap-* "$OUT"/*.txt "$OUT"/*.bin
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f build/kernelfull.o ] || { echo "sleep-idle: run 'make all' first" >&2; exit 1; }

echo "sleep-idle: kernel symbols"
if kernel_symbols "$OUT"; then echo "found" > "$OUT/symbols.txt"; else echo "missing" > "$OUT/symbols.txt"; fi
PAUSE_RESUME=$(printf '%08x' $((0x${SYM_PAUSE:-0} + 2)))

hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
cp bin/os.bin "$OUT/idle.bin"
mattrib -i "$OUT/idle.bin" -r ::shell.elf
mcopy -o -i "$OUT/idle.bin" "$PROGRAM" ::shell.elf
mcopy -i "$OUT/idle.bin" "$PROGRAM" ::libtest.elf

R="$OUT/results.txt"
# The file names the kernel keeps for the program it starts and for the child that program runs.
PROGRAM_FILE=0:/shell.elf
CHILD_FILE=0:/libtest.elf
: > "$R"
: > "$OUT/screens.txt"
keep() { screen_snapshot; cat "$OUT/screen.txt" >> "$OUT/screens.txt"; }

# word <file> [offset]: the little-endian 32-bit word at offset (hex) in file, as 8 hex digits.
word() { od -An -tx4 -j $((0x${2:-0})) -N4 "$1" | tr -d ' \n'; }

# sample <dir>: stop the VM, save registers, PIC state, the tick counter, the kernel page
# directory and the task list in <dir>, then let it run on. The list is walked from task_head
# while the VM is stopped, at most 12 tasks: task<i>.bin is the first 0x60 bytes of the i-th
# task, and tasks.txt has "<task> <its process's file name>" for each, in list order.
sample() {
    local d="$1" t p i=0 lines
    rm -f "$d"/*.bin "$d/tasks.txt" "$d/monitor.txt"
    qemu_cmd "stop"
    lines=$(wc -l < "$Q_OUT/monitor.log")
    qemu_cmd "info registers"
    qemu_cmd "info pic"
    qemu_cmd "pmemsave 0x$SYM_TICKS 4 \"$d/ticks.bin\""
    qemu_cmd "pmemsave 0x$SYM_KERNEL_CHUNK 4 \"$d/chunk.bin\""
    qemu_cmd "pmemsave 0x$SYM_PAUSE 4 \"$d/pause.bin\""
    qemu_cmd "pmemsave 0x$SYM_TASK_HEAD 4 \"$d/head.bin\""
    wait_file "$d/pause.bin" 4 && wait_file "$d/chunk.bin" 4 && wait_file "$d/head.bin" 4 || true
    if [ -f "$d/chunk.bin" ]; then
        qemu_cmd "pmemsave 0x$(word "$d/chunk.bin") 4 \"$d/directory.bin\""
        wait_file "$d/directory.bin" 4 || true
    fi
    : > "$d/tasks.txt"
    t=$(word "$d/head.bin" 2> /dev/null || true)
    while [ -n "$t" ] && [ "$t" != 00000000 ]; do
        if [ "$i" -ge 12 ]; then echo "more than 12 tasks" >> "$d/tasks.txt"; break; fi
        qemu_cmd "pmemsave 0x$t 0x60 \"$d/task$i.bin\""
        wait_file "$d/task$i.bin" 96 || break
        p=$(word "$d/task$i.bin" "$TASK_PROCESS_OFFSET")
        qemu_cmd "pmemsave $((0x$p + 0x$PROCESS_FILENAME_OFFSET)) 64 \"$d/name$i.bin\""
        wait_file "$d/name$i.bin" 64 || break
        echo "$t $(tr '\0' '\n' < "$d/name$i.bin" | head -n 1)" >> "$d/tasks.txt"
        t=$(word "$d/task$i.bin" "$TASK_NEXT_OFFSET")
        i=$((i + 1))
    done
    tail -n +"$((lines + 1))" "$Q_OUT/monitor.log" | tr -d '\r' > "$d/monitor.txt"
    qemu_cmd "cont"
}

# snap <name>: sample into snap-<name>/ until the CPU is halted (HLT=1), at most
# SAMPLE_TRIES times. A halted sample is final whatever else it shows (idle_ok judges it); the
# only halt the kernel has is IRQ_ON_AND_PAUSE (and PANIC). After the last try the sample stays
# as it is, so idle_ok fails it as not halted.
snap() {
    local name="$1" d="$OUT/snap-$1" try
    rm -rf "$d"
    mkdir -p "$d"
    for ((try = 1; ; try++)); do
        sample "$d"
        grep -q 'HLT=1' "$d/monitor.txt" && break
        echo "  $name: sample $try not halted ($(grep -oE '(EIP|ESP|EFL)=[0-9a-f]*|CPL=[0-3]' "$d/monitor.txt" |
            head -n 4 | tr '\n' ' ')$(grep -oE '^pic0: .* isr=[0-9a-f]*' "$d/monitor.txt" | grep -o 'isr=[0-9a-f]*'))"
        ((try < SAMPLE_TRIES)) || break
        mv "$d/monitor.txt" "$d/busy-$try.txt"
        sleep 0.1
    done
}

# ticks_now: the tick counter, read while the VM runs on.
ticks_now() {
    rm -f "$OUT/ticks-now.bin"
    qemu_cmd "pmemsave 0x$SYM_TICKS 4 \"$OUT/ticks-now.bin\""
    wait_file "$OUT/ticks-now.bin" 4 && word "$OUT/ticks-now.bin"
}

# idle_ok <name> <file name>...: the CPU in snap-<name> is halted in the idle loop, the task list
# holds exactly one task per <file name> (the file its process runs), in that order, and every
# task (task0.bin, ...) sleeps before its deadline. Writes "PASS <name> idle" or the reasons.
idle_ok() {
    local name="$1" d="$OUT/snap-$1" m bad="" eip efl cr3 dir ticks i=0 state wake t file got
    shift
    m="$d/monitor.txt"
    eip=$(grep -o 'EIP=[0-9a-f]*' "$m" | head -n 1 | cut -d= -f2)
    efl=$(grep -o 'EFL=[0-9a-f]*' "$m" | head -n 1 | cut -d= -f2)
    cr3=$(grep -o 'CR3=[0-9a-f]*' "$m" | head -n 1 | cut -d= -f2)
    dir=$(word "$d/directory.bin" 2> /dev/null || true)
    ticks=$(word "$d/ticks.bin" 2> /dev/null || true)
    echo "  $name: EIP=$eip EFL=$efl CR3=$cr3 (kernel directory $dir) ticks 0x$ticks $(grep -o 'ESP=[0-9a-f]*' "$m" | head -n 1)"
    grep -E '^pic[01]: ' "$m" | sed 's/^/    /' || true
    [ "$eip" = "$PAUSE_RESUME" ] || bad="$bad EIP=$eip (want $PAUSE_RESUME)"
    grep -q 'HLT=1' "$m" || bad="$bad not halted"
    grep -q 'CPL=0' "$m" || bad="$bad not ring 0"
    grep -q 'CS =0008' "$m" || bad="$bad CS not 0x08"
    grep -q 'DS =0010' "$m" || bad="$bad DS not 0x10"
    [ -n "$efl" ] && (((0x$efl & 0x200) != 0)) || bad="$bad IF clear"
    [ -n "$cr3" ] && [ -n "$dir" ] && [ "$cr3" = "$dir" ] || bad="$bad CR3 not the kernel directory"
    grep -qE '^pic0: .* isr=00 ' "$m" || bad="$bad master PIC has an IRQ in service"
    got=$(cut -d' ' -f2- "$d/tasks.txt" 2> /dev/null | tr '\n' ' ' | sed 's/ $//' || true)
    [ "$got" = "$*" ] || bad="$bad tasks [$got], want [$*]"
    while read -r t file; do
        state=$(word "$d/task$i.bin" "$TASK_STATE_OFFSET" 2> /dev/null || true)
        wake=$(word "$d/task$i.bin" "$TASK_WAKE_OFFSET" 2> /dev/null || true)
        echo "    task $t ($file): state 0x$state, wakes at 0x$wake"
        [ "$state" = 00000001 ] || bad="$bad task $t not sleeping"
        # Before its deadline: deadline - now is 1 .. 2^31 - 1 (mod 2^32).
        if [ -n "$wake" ] && [ -n "$ticks" ]; then
            local left=$(((0x$wake - 0x$ticks) & 0xffffffff))
            ((left > 0 && left < 0x80000000)) || bad="$bad task $t past its deadline"
        else
            bad="$bad task $t not read"
        fi
        i=$((i + 1))
    done < "$d/tasks.txt"
    if [ -z "$bad" ]; then echo "PASS $name idle" >> "$R"; else echo "  $name:$bad"; fi
}

# esp_of <name>: ESP of a halted sample, empty for any other.
esp_of() {
    local m="$OUT/snap-$1/monitor.txt"
    grep -q 'HLT=1' "$m" && grep -o 'ESP=[0-9a-f]*' "$m" | head -n 1 | cut -d= -f2
}

echo "sleep-idle: boot (libtest.elf as shell.elf)"
qemu_start "$OUT/idle.bin" "$OUT"
if wait_screen "libtest idle: armed" "$BOOT_TIMEOUT"; then
    if wait_screen "lt-i: sleeping" "$STEP_TIMEOUT"; then
        keep
        snap s1
        [ "$(od -An -tx1 "$OUT/snap-s1/pause.bin" | tr -d ' \n')" = fbf4fac3 ] && echo "PASS pause bytes" >> "$R"
        # A key and an NMI while it sleeps; look again once the clock has moved on.
        qemu_cmd "sendkey k"
        qemu_cmd "nmi"
        t1=$(word "$OUT/snap-s1/ticks.bin")
        for ((try = 0; try < 40; try++)); do
            sleep 0.2
            now=$(ticks_now || echo "$t1")
            (((0x$now - 0x$t1) >= 5)) && break
        done
        snap s2
        echo "  ticks 0x$t1 -> 0x$(word "$OUT/snap-s2/ticks.bin")"
        idle_ok s1 "$PROGRAM_FILE"
        idle_ok s2 "$PROGRAM_FILE"
        (((0x$(word "$OUT/snap-s2/ticks.bin") - 0x$t1) >= 5)) && echo "PASS ticks go on" >> "$R"
        [ -n "$(esp_of s1)" ] && [ "$(esp_of s1)" = "$(esp_of s2)" ] && echo "PASS idle stack" >> "$R"
        # The saved registers (a ring 3 frame: cs 0x1b) of the same task are the same before and
        # after the IRQs.
        regs() { od -An -tx1 -j $((0x$TASK_REGISTERS_OFFSET)) -N48 "$OUT/snap-$1/task0.bin" | tr -d ' \n'; }
        first_task() { head -n 1 "$OUT/snap-$1/tasks.txt" | cut -d' ' -f1; }
        cs=$(word "$OUT/snap-s1/task0.bin" "$(printf '%x' $((0x$TASK_REGISTERS_OFFSET + 32)))")
        eax=$(word "$OUT/snap-s1/task0.bin" "$(printf '%x' $((0x$TASK_REGISTERS_OFFSET + 24)))")
        echo "  saved registers of task $(first_task s1): $(regs s1) (cs 0x$cs, eax 0x$eax)"
        [ -n "$(first_task s1)" ] && [ "$(first_task s1)" = "$(first_task s2)" ] && [ "$(regs s1)" = "$(regs s2)" ] &&
            [ "$cs" = 0000001b ] && echo "PASS saved registers" >> "$R"
        [ "$eax" = 00000000 ] && echo "PASS saved eax" >> "$R"
    fi
    if wait_screen "lt-i: woke" "$STEP_TIMEOUT" && wait_screen "lt-i: second sleeping" "$STEP_TIMEOUT" &&
        wait_screen "lt-ic: sleeping" "$STEP_TIMEOUT"; then
        keep
        snap s3
        idle_ok s3 "$PROGRAM_FILE" "$CHILD_FILE"
        [ -n "$(esp_of s1)" ] && [ "$(esp_of s1)" = "$(esp_of s3)" ] && echo "PASS idle stack again" >> "$R"
    fi
    if wait_screen "lt-i: second woke" "$STEP_TIMEOUT" && wait_screen "lt-ic: woke" "$STEP_TIMEOUT" &&
        wait_screen "Process was terminated" "$STEP_TIMEOUT"; then
        keep
        # The child ended while the parent sleeps on: idle again, entered from the exit.
        snap s4
        idle_ok s4 "$PROGRAM_FILE"
    fi
fi
keep
qemu_stop || true
[ "$hash_before" = "$(sha256sum bin/os.bin | cut -d' ' -f1)" ] && echo "unchanged" > "$OUT/image.txt" ||
    echo "changed" > "$OUT/image.txt"

S="$OUT/screens.txt"
check "kernel symbols and struct task offsets found"          "$OUT/symbols.txt" "found" line
check "no kernel panic (debugcon)"                            "$OUT/debugcon.log" "PANIC" absent
check "IRQ_ON_AND_PAUSE is sti; hlt; cli; ret"                "$R" "PASS pause bytes" line
check "alone asleep: halted in the idle loop, kernel CR3"     "$R" "PASS s1 idle" line
check "after a key and an NMI: still halted, still asleep"    "$R" "PASS s2 idle" line
check "ticks went on while halted"                            "$R" "PASS ticks go on" line
check "idle stack pointer unchanged over ticks, key and NMI"  "$R" "PASS idle stack" line
check "saved ring 3 registers unchanged while asleep"         "$R" "PASS saved registers" line
check "saved eax is already 0 (the result) while asleep"     "$R" "PASS saved eax" line
check "two tasks asleep: halted, both sleeping"               "$R" "PASS s3 idle" line
check "idle stack pointer the same in the second sleep"       "$R" "PASS idle stack again" line
check "child ended, parent asleep: halted"                    "$R" "PASS s4 idle" line
check "first sleep: 0 after 502+ ticks, then the key"         "$S" "lt-i: first sleep ok"
check "child sleep: 0 after 602+ ticks"                       "$S" "lt-ic: sleep ok"
check "second sleep: 0 after 302+ ticks"                      "$S" "lt-i: second sleep ok"
check "registers and stack kept by every sleep"               "$S" "libtest idle: done"
check "no FAIL line"                                          "$S" "FAIL" absent
check "bin/os.bin not changed by the test"                    "$OUT/image.txt" "unchanged" line
finish sleep-idle
