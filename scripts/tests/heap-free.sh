#!/usr/bin/env bash
#
# heap_free() must ignore pointers that don't start a taken run of blocks (NULL, outside the heap,
# unaligned, the middle of a run, a second free). Copy bin/os.bin, add programs/heapfree to the
# copy only, boot it and run heapfree.elf twice from the shell. heapfree.elf runs the kernel's
# own heap.c on a private table with guard bytes around it, and also calls the real free(NULL)
# syscall. After each run scripts/tests/lib/guest-state.sh stops the guest at the shell's next
# key request: heapfree.elf has ended and the same shell has the keyboard. bin/os.bin itself must
# not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for each run)
#      EXIT_TIMEOUT=15 (for the process to end after its last line)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/heap-free
PROGRAM=programs/heapfree/build/heapfree.elf
SHELL_ELF=programs/shell/build/shell.elf
mkdir -p "$OUT"
rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT"/shell.txt "$OUT"/screen-*.txt

# Both ELF files must fit in one FAT cluster (64 KiB).
[ -f "$PROGRAM" ] || { echo "heap-free: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
for elf in "$SHELL_ELF" "$PROGRAM"; do
    size=$(wc -c < "$elf")
    echo "  $elf: $size bytes"
    [ "$size" -le 65536 ] || { echo "heap-free: $elf is bigger than 64 KiB" >&2; exit 1; }
done

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone
# with the keyboard; results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$EXIT_TIMEOUT" owner=first "$@"
}

echo "heap-free: kernel symbols"
guest_state_setup "$OUT"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/heap-free.bin"
mcopy -i "$OUT/heap-free.bin" "$PROGRAM" ::
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "heap-free: boot a copy of bin/os.bin with heapfree.elf and run it twice"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/heap-free.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot save="$OUT/shell.txt" screen="$OUT/screen-boot.txt"; then
    type_line "heapfree.elf one"
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:heapfree one: done"; then
        # A second run only after a clean first one: failure lines would fill the screen.
        if grep -qxE "(> )?heapfree one: PASS" "$OUT/screen.txt" &&
            state one shell="$OUT/shell.txt" screen="$OUT/screen-one.txt"; then
            type_line "heapfree.elf two"
            if wait_screen_lines "$RUN_TIMEOUT" "prompt:heapfree two: done"; then
                state two shell="$OUT/shell.txt" screen="$OUT/screen-end.txt" || true
            else
                echo "  second run did not complete"
                failures=$((failures + 1))
            fi
        fi
    else
        echo "  first run did not complete; possible heap corruption or hang"
        failures=$((failures + 1))
    fi
fi
qemu_stop || true

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
# Each run was seen gone at a rendezvous (the shell alone in the task list).
ended=$(cat "$OUT"/state-one.txt "$OUT"/state-two.txt 2> /dev/null | grep -cxE 'PASS (one|two)_task_count' || true)
echo "ended=${ended:-0}" > "$OUT/ended.txt"
# The checks below read the screens saved at the rendezvous after each run (nothing is printing or
# scrolling there); the failure lines are looked for on those and on the last screen.
for s in boot one end; do [ -f "$OUT/screen-$s.txt" ] || : > "$OUT/screen-$s.txt"; done
cat "$OUT/screen-one.txt" "$OUT/screen-end.txt" "$OUT/screen.txt" > "$OUT/screen-runs.txt" 2> /dev/null || true

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log"  "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log"  "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen-boot.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "first command echoed by the shell"          "$OUT/screen-one.txt" "> heapfree.elf one"
check "first run: free(NULL) syscall returned"      "$OUT/screen-one.txt" "heapfree one: free(NULL) returned"
check "heap: NULL free ignored"                     "$OUT/screen-runs.txt" "heapfree: FAIL NULL free changed guard" absent
check "heap: free below the heap ignored"           "$OUT/screen-runs.txt" "heapfree: FAIL free below heap" absent
check "heap: free past the heap end ignored"        "$OUT/screen-runs.txt" "heapfree: FAIL free past heap end" absent
check "heap: unaligned free ignored"                "$OUT/screen-runs.txt" "heapfree: FAIL unaligned free" absent
check "heap: middle block free ignored"             "$OUT/screen-runs.txt" "heapfree: FAIL middle block free" absent
check "heap: valid free releases only its run"      "$OUT/screen-runs.txt" "heapfree: FAIL valid free" absent
check "heap: double free ignored"                   "$OUT/screen-runs.txt" "heapfree: FAIL double free" absent
check "heap: last block and exact fit"              "$OUT/screen-runs.txt" "heapfree: FAIL last block" absent
check "heap: exact fit"                             "$OUT/screen-runs.txt" "heapfree: FAIL exact fit" absent
check "heap: zero and huge sizes refused"           "$OUT/screen-runs.txt" "heapfree: FAIL zero or huge size" absent
check "heapfree reported no failure"                "$OUT/screen-runs.txt" "heapfree: FAIL" absent
check "first run passed"                            "$OUT/screen-one.txt" "heapfree one: PASS"
check "first run finished"                          "$OUT/screen-one.txt" "heapfree one: done"
check "second command echoed by the shell"         "$OUT/screen-end.txt" "heapfree.elf two"
check "second run: free(NULL) syscall returned"     "$OUT/screen-end.txt" "heapfree two: free(NULL) returned"
check "second run passed"                           "$OUT/screen-end.txt" "heapfree two: PASS"
check "second run finished"                         "$OUT/screen-end.txt" "heapfree two: done"
check "kernel symbols for the rendezvous found"     "$OUT/symbols.txt"   "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"          "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                        "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"           "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot "boot"
stop_checks one  "first run ended"
stop_checks two  "second run ended"
check "both processes ended (each seen gone)"       "$OUT/ended.txt"     "ended=2" line
check "bin/os.bin not changed by the test"          "$OUT/image.txt"     "unchanged" line
finish heap-free
