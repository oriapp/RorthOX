#!/usr/bin/env bash
#
# Taking a task out of the task list must keep the list linked both ways, so the tasks left
# keep running and new ones can still be added.
#
# 1. Host check (scripts/tests/host/task-list.c): builds kernel/task/task.c with the host C
#    compiler against stubbed paging and heap code and frees tasks in every order from lists
#    of 1 to 4 tasks, checking head, tail, every next/prev pair and the current task after each
#    step. It also checks the first two task ids and their scheduling fields, and the sleeping
#    tasks of syscall 12: runnable-only selection, wake at the deadline (also across the 2^32
#    wrap), the idle halt when every task sleeps, and freeing sleeping tasks.
# 2. QEMU with its gdbstub on a Unix socket: copy bin/os.bin, add programs/tasktest to the copy
#    only, and boot it. The shell starts tasktest.elf (the parent), which starts "tasktest.elf
#    child" and exits while the child still waits for a key, so the parent leaves the middle of
#    the list (shell, parent, child). scripts/tests/lib/guest-state.sh stops the guest when the
#    child asks for its key: only the shell (the same task as at boot) and the child may be left,
#    and the child owns the keyboard. Then the child exits: the shell must be the only task and own
#    the keyboard again, and still start blank.elf twice, each run gone before the next.
#    The screen saved at the last stop (never halfway through a scroll) must show both blank.elf
#    runs loaded and four programs ended.
# bin/os.bin itself must not change.
#
# Needs python3. Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      STEP_TIMEOUT=20 (for each step after that)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/task-list
PROGRAM=programs/tasktest/build/tasktest.elf
mkdir -p "$OUT"

echo "task-list: host check of kernel/task/task.c"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=undefined -fno-sanitize-recover=all \
    -I kernel -I build/config -I scripts/tests/host \
    scripts/tests/host/task-list.c scripts/tests/host/kalloc.c kernel/task/task.c kernel/memory/memory.c \
    -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed 's/^/  /' "$OUT/host-build.log" | head -n 10 || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "host check: every task list case passed" "$OUT/host-check.log" "task-list host check: 0 of"
check "host check: every sleeping-task case passed" "$OUT/host-check.log" "task-list host check: sleep: 0 of 118 cases failed" line
check "no undefined behaviour" "$OUT/host-check.log" "runtime error" absent

[ -f "$PROGRAM" ] || { echo "task-list: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
command -v python3 > /dev/null || { echo "task-list: python3 not found" >&2; exit 1; }

rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT"/screen-*.txt "$OUT/shell.txt"
echo "task-list: kernel symbols"
guest_state_setup "$OUT"

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/task-list.bin"
mcopy -i "$OUT/task-list.bin" "$PROGRAM" ::
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# state <tag> [options]: the guest-state rendezvous, results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$STEP_TIMEOUT" "$@"
}

echo "task-list: boot"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/task-list.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
    state boot save="$OUT/shell.txt" owner=first screen="$OUT/screen-boot.txt"; then
    type_line "tasktest.elf"
    # When the child asks for its key, the parent must be gone and the child still there.
    if wait_screen_lines "$STEP_TIMEOUT" "prompt:tasktest child: ready" "prompt:tasktest parent: exiting" &&
        state child at=last tasks=2 shell="$OUT/shell.txt" owner=last screen="$OUT/screen-parent-gone.txt"; then
        qemu_cmd "sendkey g"
        if wait_screen_lines "$STEP_TIMEOUT" "prompt:tasktest child: done" &&
            state back shell="$OUT/shell.txt" owner=first; then
            type_line "blank.elf first"
            if wait_screen_lines "$STEP_TIMEOUT" "line:first" && state first shell="$OUT/shell.txt" owner=first; then
                type_line "blank.elf second"
                wait_screen_lines "$STEP_TIMEOUT" "line:second" &&
                    state second shell="$OUT/shell.txt" owner=first screen="$OUT/screen-end.txt" || true
            fi
        fi
    fi
fi
qemu_stop || true

# Counted on the screens saved at the rendezvous: nothing is printing or scrolling there.
# Each process that ended was seen gone at a rendezvous: the parent (child), the child (back),
# the first and the second blank.elf.
ended=$(cat "$OUT"/state-child.txt "$OUT"/state-back.txt "$OUT"/state-first.txt "$OUT"/state-second.txt 2> /dev/null |
    grep -cxE 'PASS (child|back|first|second)_task_count' || true)
echo "ended=${ended:-0}" > "$OUT/terminated.txt"
parent_gone=$(grep -cF "Process was terminated" "$OUT/screen-parent-gone.txt" 2> /dev/null || true)
echo "terminated=${parent_gone:-0}" > "$OUT/parent-gone.txt"
loaded=$(grep -cF "Blank loaded" "$OUT/screen-end.txt" 2> /dev/null || true)
echo "loaded=${loaded:-0}" > "$OUT/loaded.txt"
hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

check "kernel reached the end of init (debugcon)"     "$OUT/debugcon.log"      "Thats cool"
check "no kernel panic (debugcon)"                     "$OUT/debugcon.log"      "PANIC" absent
check "shell started (banner on screen)"               "$OUT/screen-boot.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "parent started"                                 "$OUT/screen-parent-gone.txt" "tasktest parent: start"
check "child started from the parent"                  "$OUT/screen-parent-gone.txt" "tasktest child: ready"
check "kernel symbols for the rendezvous found"        "$OUT/symbols.txt"       "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the task's getkey"     "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"   "$f" "PASS $1_list_consistent" line
    check "$2: number of tasks"                  "$f" "PASS $1_task_count" line
    check "$2: keyboard owner"                   "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot   "boot (the shell alone, owns the keyboard)"
stop_checks child  "child waits (parent gone, shell and child left, the child owns the keyboard)"
check "parent ended while the child was waiting"       "$OUT/parent-gone.txt"   "terminated=1" line
check "child still ran after the parent ended"         "$OUT/screen.txt"        "tasktest child: done"
stop_checks back   "child ended (the shell alone, owns the keyboard)"
check "blank.elf started from the shell afterwards"    "$OUT/screen.txt"        "first" line
stop_checks first  "first blank.elf ended (the shell alone)"
check "blank.elf started a second time"                "$OUT/screen.txt"        "second" line
stop_checks second "second blank.elf ended (the shell alone)"
check "both blank.elf runs loaded"                     "$OUT/loaded.txt"        "loaded=2" line
check "all four processes ended (each seen gone)"      "$OUT/terminated.txt"    "ended=4" line
check "bin/os.bin not changed by the test"             "$OUT/image.txt"         "unchanged" line
finish task-list
