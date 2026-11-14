#!/usr/bin/env bash
#
# Memory a program gets must be zero up to the end of its last page, not only up to the size it
# asked for, while syscalls still accept only the requested bytes.
#
# Host part: scripts/tests/host/user-pages.c runs the real process_malloc and raw program
# loading with every free heap block full of stale bytes, and checks that the backing came from
# those blocks, is cleared to the page end, and that ownership stays exact; sizes of 0, sizes that
# can't be rounded up and sizes too big for the heap are refused without a mapping, an entry or a
# kept block.
#
# QEMU part: programs/pagetest fills heap blocks with 0xA5, frees them and asks for less; first
# fit must give back the same blocks (checked), and they must read as zero. The shell frees its
# command list when it gets the CPU back after the launch, below pagetest's blocks, so pagetest
# waits for a key first: scripts/tests/lib/guest-state.sh stops the guest at the shell's next key
# request with both tasks in the list (the shell has freed the list by then), and only then the
# key goes to pagetest.
#
# Needs python3 for the QEMU part.
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for pagetest)
#      EXIT_TIMEOUT=15 (for the process to end)   HOST_ONLY=1 (skip QEMU)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/user-pages
PROGRAM=programs/pagetest/build/pagetest.elf
mkdir -p "$OUT"
rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT"/symbols.txt

rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log"
host_failed=0
if "${HOST_CC:-cc}" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
    -I kernel -I build/config -I scripts/tests/host \
    scripts/tests/host/user-pages.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
    kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
    kernel/string/string.c kernel/memory/memory.c -Wl,--wrap=kzalloc \
    -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" programs/blank/build/blank.elf > "$OUT/host-check.log" 2>&1; then :; else host_failed=1; fi
    cat "$OUT/host-check.log"
else
    cat "$OUT/host-build.log"
    host_failed=1
fi
[ "${HOST_ONLY:-0}" = 0 ] || exit "$host_failed"

[ -f "$PROGRAM" ] || { echo "user-pages: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
command -v python3 > /dev/null || { echo "user-pages: python3 not found" >&2; exit 1; }
. ./scripts/tests/lib/guest-state.sh
guest_state_setup "$OUT"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/user-pages.bin"
mcopy -i "$OUT/user-pages.bin" "$PROGRAM" ::
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "user-pages: boot a copy of bin/os.bin with pagetest.elf and run it"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/user-pages.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "pagetest.elf"
    # The shell back at its key request with pagetest still there: its command list is freed, and
    # pagetest (waiting for its key) owns the keyboard.
    if wait_screen "pagetest: ready" "$RUN_TIMEOUT" &&
        guest_state "$SOCK" "$OUT/state-settled.txt" settled timeout="$RUN_TIMEOUT" tasks=2 owner=last; then
        qemu_cmd "sendkey k"
    fi
    if wait_screen "pagetest: done" "$RUN_TIMEOUT"; then
        wait_screen "Process was terminated" "$EXIT_TIMEOUT" || true
    else
        echo "  pagetest did not complete"
        failures=$((failures + 1))
    fi
fi
qemu_stop || true

[ "$hash_before" = "$(image_hash)" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

[ -f "$OUT/state-settled.txt" ] || : > "$OUT/state-settled.txt"
check "no kernel panic (debugcon)"                 "$OUT/debugcon.log" "PANIC" absent
check "typed command echoed by the shell"          "$OUT/screen.txt"   "> pagetest.elf"
check "kernel symbols for the rendezvous found"    "$OUT/symbols.txt"  "found" line
check "shell back at its key request after launch" "$OUT/state-settled.txt" "PASS settled_rendezvous" line
check "shell and pagetest both in the task list"   "$OUT/state-settled.txt" "PASS settled_task_count" line
check "task list and slots consistent"             "$OUT/state-settled.txt" "PASS settled_list_consistent" line
check "pagetest owns the keyboard"                 "$OUT/state-settled.txt" "PASS settled_keyboard_owner" line
check "freed block reused for malloc(1)"           "$OUT/screen.txt"   "pagetest: FAIL freed block not reused" absent
check "malloc(1) block zero to its end"            "$OUT/screen.txt"   "pagetest: one block zero 1"
check "malloc(1) owns 1 byte"                      "$OUT/screen.txt"   "pagetest: exact ownership 1"
check "malloc(8197) blocks zero to their end"      "$OUT/screen.txt"   "pagetest: three blocks zero 8197"
check "malloc(8197) owns 8197 bytes"               "$OUT/screen.txt"   "pagetest: exact ownership 8197"
check "zero, unroundable and huge sizes refused"   "$OUT/screen.txt"   "pagetest: refused sizes 4"
check "pagetest reported no failure"               "$OUT/screen.txt"   "pagetest: FAIL" absent
check "pagetest passed"                            "$OUT/screen.txt"   "pagetest: PASS"
check "pagetest finished"                          "$OUT/screen.txt"   "pagetest: done"
check "pagetest process ended"                     "$OUT/screen.txt"   "Process was terminated"
check "bin/os.bin not changed by the test"         "$OUT/image.txt"    "unchanged" line
failures=$((failures + host_failed))
finish user-pages
