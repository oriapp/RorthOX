#!/usr/bin/env bash
#
# A task must get its own EFLAGS back when the scheduler returns to it (task_return), and the
# kernel must enter C with DF clear whatever the user program left in DF.
#
# QEMU starts stopped with its gdbstub on a Unix socket, on a copy of bin/os.bin with
# programs/flagstest added (bin/os.bin itself must not change).
# 1. scripts/tests/lib/task_flags.py frame: at the first task_return, writes 36 saved-flags
#    patterns into the task's registers one after the other, runs the real task_return up to its
#    iretd each time and checks the frame it built: EFLAGS = (saved & 0x00250dd5) | 0x202, and the
#    other fields and registers from the saved state. Then it rewinds and lets the boot go on.
# 2. In the shell, flagtest.elf (flagstest.elf copied under an 8.3 name: the FAT16 driver has no
#    long names) prints "flagstest: armed" and waits for a key, then spins until task_flags.py
#    entry, attached after the key, releases it. It then sets CF PF AF ZF SF DF OF AC ID to four
#    patterns and waits for a real timer tick (save, switch to the shell, come back through
#    task_return; it spins with GS set to the null selector until the kernel's way back to ring 3
#    loads GS again), does the same around a syscall, and prints "flagstest: PASS" if every flag
#    came back. Meanwhile task_flags.py breaks at interrupt_handler and isr80h_handler and checks
#    that DF is clear on entry to C while the interrupted user code had DF set, and at
#    task_return to check that each of the four timer waits really came back through it.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell), STEP_TIMEOUT=60 (each step after)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/task-flags
PROGRAM=programs/flagstest/build/flagstest.elf
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "task-flags: python3 not found" >&2; exit 1; }
[ -f "$PROGRAM" ] || { echo "task-flags: $PROGRAM not found, run 'make all' first" >&2; exit 1; }

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/task-flags.bin"
mcopy -i "$OUT/task-flags.bin" "$PROGRAM" ::flagtest.elf
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "task-flags: boot, frames built by task_return"
QEMU_FLAGS="-S -gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/task-flags.bin" "$OUT"
if python3 -B scripts/tests/lib/task_flags.py frame "$SOCK" build/kernel.map "$OUT/frame-result.txt" \
    > "$OUT/frame.log" 2>&1; then frame_status=0; else frame_status=$?; fi
sed 's/^/  /' "$OUT/frame.log"
echo "exit=$frame_status" > "$OUT/frame-status.txt"

entry_status="not run"
if [ "$frame_status" -eq 0 ] && wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    echo "task-flags: flagtest.elf"
    type_line "flagtest.elf"
    if wait_screen "flagstest: armed" "$STEP_TIMEOUT"; then
        # QEMU drops key presses while a debugger has the guest stopped, so the key goes in first.
        # The screen dump after it is a monitor round trip: once it is back, the key is in.
        qemu_cmd "sendkey g"
        screen_snapshot
        if python3 -B scripts/tests/lib/task_flags.py entry "$SOCK" build/kernel.map "$OUT/entry-result.txt" \
            "$PROGRAM" > "$OUT/entry.log" 2>&1; then entry_status=0; else entry_status=$?; fi
        sed 's/^/  /' "$OUT/entry.log"
        # The only program in this boot: its result, then its end (the prompt may land in front).
        wait_screen_lines "$STEP_TIMEOUT" "regex:(> )?flagstest: (PASS|FAIL)" "prompt:Process was terminated" || true
    fi
fi
echo "exit=$entry_status" > "$OUT/entry-status.txt"
qemu_stop || true
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

f="$OUT/frame-result.txt"
e="$OUT/entry-result.txt"
check "frame session finished without errors"             "$OUT/frame-status.txt" "exit=0" line
check "stopped at the first task_return"                   "$f" "PASS at_task_return" line
check "first task starts with saved flags 0"               "$f" "PASS first_task_flags_zero" line
check "task_return reached its iretd for every pattern"    "$f" "PASS reached_iretd" line
check "iret EFLAGS = (saved & 0x00250dd5) | 0x202"         "$f" "PASS frame_flags" line
check "iret EIP/CS/ESP/SS from the saved registers"        "$f" "PASS frame_fields" line
check "general registers from the saved registers"         "$f" "PASS frame_registers" line
check "DS/ES/FS/GS loaded with the saved SS"               "$f" "PASS frame_segments" line
check "shell started after the frame checks"               "$OUT/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "flagstest started and waited for its key"           "$OUT/screen.txt" "flagstest: armed"
check "flags kept across the timer path and a syscall"     "$OUT/screen.txt" "flagstest: PASS"
check "flagstest found no wrong flags"                     "$OUT/screen.txt" "flagstest: FAIL" absent
check "flagstest exited"                                   "$OUT/screen.txt" "Process was terminated"
check "entry session finished without errors"             "$OUT/entry-status.txt" "exit=0" line
check "flagstest got its key and waited for the debugger"  "$e" "PASS gate_reached" line
check "timer vector entered from ring 3 with DF set"       "$e" "PASS timer_entry_seen" line
check "DF clear on entry to interrupt_handler"             "$e" "PASS timer_entry_df_clear" line
check "every timer pattern came back through task_return"  "$e" "PASS timer_return_every_pattern" line
check "syscall entered from ring 3 with DF set"            "$e" "PASS syscall_entry_seen" line
check "DF clear on entry to isr80h_handler"                "$e" "PASS syscall_entry_df_clear" line
check "no kernel panic (debugcon)"                         "$OUT/debugcon.log" "PANIC" absent
check "bin/os.bin not changed by the test"                 "$OUT/image.txt" "unchanged" line
finish task-flags
