#!/usr/bin/env bash
#
# The print and printc syscalls copy the user's text through a checked copy that needs no kernel
# heap, so with the heap full they must still print it and return 0, and never print what was
# left in a kernel buffer. Copy bin/os.bin, add programs/printoom to the
# copy only, and boot it. printoom.elf leaves one free block, prints a line, takes the last block,
# calls print and printc, and checks they return 0 and their text shows. No second copy of the
# line before may reach the screen. Then blank.elf must start from the shell.
# printoom.elf counts free blocks in copies of the kernel's heap table that a debugger writes into
# it through QEMU's gdbstub (scripts/tests/lib/heap-snapshot.sh); when print and printc are called,
# the debugger checks that the heap table is still the copy taken just before (no block free).
# scripts/tests/lib/guest-state.sh stops the guest at key requests (never while the heap debugger
# is attached): the heap gets full only once the shell is back at its own key request and
# printoom.elf has the keyboard, and after printoom.elf and blank.elf the shell is alone, the same
# task, and has the keyboard. The "no second copy" count is taken on the screen saved at the
# rendezvous after printoom.elf, where nothing is printing or scrolling.
# bin/os.bin itself must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for printoom.elf)
#      EXIT_TIMEOUT=30 (for each process to end, for blank.elf to start, and for each rendezvous)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/heap-snapshot.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/print-oom
PROGRAM=programs/printoom/build/printoom.elf
mkdir -p "$OUT"
rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT/shell.txt" "$OUT"/screen-*.txt
[ -f "$PROGRAM" ] || { echo "print-oom: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
[ "$QEMU_RAM_MIB" -ge 128 ] || { echo "print-oom: needs QEMU_RAM_MIB >= 128 (is $QEMU_RAM_MIB)" >&2; exit 1; }

heap_snapshot_setup "$OUT" || { echo "print-oom: cannot read the heap settings" >&2; exit 1; }
guest_state_setup "$OUT"

# state <tag> [options]: the guest-state rendezvous, results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$HEAP_GDB_DIR/gdb" "$OUT/state-$tag.txt" "$tag" timeout="$EXIT_TIMEOUT" "$@"
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/print-oom.bin"
mcopy -i "$OUT/print-oom.bin" "$PROGRAM" ::

QEMU_FLAGS="$HEAP_QEMU_FLAGS" qemu_start "$OUT/print-oom.bin" "$OUT"
heap_snapshot_trap
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
    state boot save="$OUT/shell.txt" owner=first screen="$OUT/screen-boot.txt"; then
    type_line "printoom.elf"
    # The heap gets full only after a key, once the shell is back at its own key request (its
    # prompt printed, nothing left to allocate) with printoom owning the keyboard, and the heap
    # debugger is attached. Two syscalls (print, printc) are checked against their copies.
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:printoom: ready" &&
        state ready tasks=2 shell="$OUT/shell.txt" owner=last &&
        heap_snapshot_attach "$PROGRAM" "$OUT" 2; then
        qemu_cmd "sendkey g"
        if wait_screen_lines "$RUN_TIMEOUT" "prompt:printoom: done" && heap_snapshot_detached "$EXIT_TIMEOUT" &&
            state ended shell="$OUT/shell.txt" owner=first screen="$OUT/screen-printoom.txt"; then
            type_line "blank.elf after"
            wait_screen_lines "$EXIT_TIMEOUT" "prompt:after" &&
                state after shell="$OUT/shell.txt" owner=first || true
        fi
    fi
fi
qemu_stop || true
heap_snapshot_wait "$OUT"
[ -f "$OUT/screen-printoom.txt" ] || : > "$OUT/screen-printoom.txt"
good=$(grep -cF "printoom: last good line" "$OUT/screen-printoom.txt" 2> /dev/null || true)
echo "good=${good:-0}" > "$OUT/good.txt"

check "kernel reached the end of init (debugcon)"    "$OUT/debugcon.log"        "Thats cool"
check "no kernel panic (debugcon)"                    "$OUT/debugcon.log"        "PANIC" absent
check "shell started (banner on screen)"              "$OUT/screen-boot.txt"     "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "printoom.elf waited for the key"               "$OUT/screen-printoom.txt" "printoom: ready"
check "no setup failure"                              "$OUT/screen-printoom.txt" "printoom: FAIL setup" absent
check "print returned 0 with the heap full"           "$OUT/screen-printoom.txt" "printoom: FAIL print result" absent
check "printc returned 0 with the heap full"          "$OUT/screen-printoom.txt" "printoom: FAIL printc result" absent
check "print text shown"                              "$OUT/screen-printoom.txt" "print got through"
check "printc text shown"                             "$OUT/screen-printoom.txt" "printc got through"
check "no free block after print and printc"          "$OUT/screen-printoom.txt" "printoom: FAIL free blocks after" absent
check "old buffer contents not printed again"         "$OUT/good.txt"            "good=1" line
check "printoom passed"                               "$OUT/screen-printoom.txt" "printoom: PASS"
check "printoom finished"                             "$OUT/screen-printoom.txt" "printoom: done"
check "blank.elf started from the shell afterwards"   "$OUT/screen.txt"          "Blank loaded"
check "blank.elf got its argument"                    "$OUT/screen.txt"          "after" line
check "kernel symbols for the rendezvous found"       "$OUT/symbols.txt"         "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the task's getkey"           "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: number of tasks"                        "$f" "PASS $1_task_count" line
    check "$2: keyboard owner"                         "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot  "boot (the shell alone)"
stop_checks ready "shell back at its key request before the heap fills, printoom owns the keyboard"
stop_checks ended "printoom ended (the shell alone, owns the keyboard)"
stop_checks after "blank.elf ended (the shell alone)"
heap_snapshot_checks "" "$OUT"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish print-oom
