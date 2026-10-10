#!/usr/bin/env bash
#
# Taking a task out of the task list must keep the list linked both ways, so the tasks left
# keep running and new ones can still be added.
#
# 1. Host check (scripts/tests/host/task-list.c): builds kernel/task/task.c with the host C
#    compiler against stubbed paging and heap code and frees tasks in every order from lists
#    of 1 to 4 tasks, checking head, tail, every next/prev pair and the current task after each
#    step. It also checks the first two task ids and their scheduling fields.
# 2. QEMU: copy bin/os.bin, add programs/tasktest to the copy only, and boot it. The shell starts
#    tasktest.elf (the parent), which starts "tasktest.elf child" and exits while the child
#    still waits for a key, so the parent leaves the middle of the list (shell, parent, child).
#    Then the child exits, and the shell must still start blank.elf twice.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      STEP_TIMEOUT=20 (for each step after that)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/task-list
PROGRAM=programs/tasktest/build/tasktest.elf
mkdir -p "$OUT"

echo "task-list: host check of kernel/task/task.c"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -I kernel -I build/config -I scripts/tests/host \
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

[ -f "$PROGRAM" ] || { echo "task-list: $PROGRAM not found, run 'make all' first" >&2; exit 1; }

# wait_lines <regex> <count> <seconds>: wait until <count> screen lines match <regex>.
wait_lines() {
    local regex="$1" count="$2" deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for /$regex/ x$count"; return 1; }
        screen_snapshot
        n=$(grep -cE -- "$regex" "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "${n:-0}" -ge "$count" ]; then
            echo "  /$regex/ x$count on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for /$regex/ x$count after $3 s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/task-list.bin"
mcopy -i "$OUT/task-list.bin" "$PROGRAM" ::

echo "task-list: boot"
qemu_start "$OUT/task-list.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "tasktest.elf"
    # The parent must be gone (one termination) while the child still waits for its key.
    if wait_screen "tasktest child: ready" "$STEP_TIMEOUT" &&
        wait_screen "tasktest parent: exiting" "$STEP_TIMEOUT" &&
        wait_lines 'Process was terminated' 1 "$STEP_TIMEOUT"; then
        screen_snapshot
        cp "$OUT/screen.txt" "$OUT/screen-parent-gone.txt"
        qemu_cmd "sendkey g"
        if wait_screen "tasktest child: done" "$STEP_TIMEOUT" &&
            wait_lines 'Process was terminated' 2 "$STEP_TIMEOUT"; then
            type_line "blank.elf first"
            if wait_lines '^first$' 1 "$STEP_TIMEOUT" && wait_lines 'Process was terminated' 3 "$STEP_TIMEOUT"; then
                type_line "blank.elf second"
                wait_lines '^second$' 1 "$STEP_TIMEOUT" && wait_lines 'Process was terminated' 4 "$STEP_TIMEOUT" || true
            fi
        fi
    fi
fi
qemu_stop || true

terminated=$(grep -cF "Process was terminated" "$OUT/screen.txt" 2> /dev/null || true)
echo "terminated=${terminated:-0}" > "$OUT/terminated.txt"
parent_gone=$(grep -cF "Process was terminated" "$OUT/screen-parent-gone.txt" 2> /dev/null || true)
echo "terminated=${parent_gone:-0}" > "$OUT/parent-gone.txt"
loaded=$(grep -cF "Blank loaded" "$OUT/screen.txt" 2> /dev/null || true)
echo "loaded=${loaded:-0}" > "$OUT/loaded.txt"
hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

check "kernel reached the end of init (debugcon)"     "$OUT/debugcon.log"      "Thats cool"
check "no kernel panic (debugcon)"                     "$OUT/debugcon.log"      "PANIC" absent
check "shell started (banner on screen)"               "$OUT/screen.txt"        "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "parent started"                                 "$OUT/screen.txt"        "tasktest parent: start"
check "child started from the parent"                  "$OUT/screen.txt"        "tasktest child: ready"
check "parent ended while the child was waiting"       "$OUT/parent-gone.txt"   "terminated=1" line
check "child still ran after the parent ended"         "$OUT/screen.txt"        "tasktest child: done"
check "blank.elf started from the shell afterwards"    "$OUT/screen.txt"        "first" line
check "blank.elf started a second time"                "$OUT/screen.txt"        "second" line
check "both blank.elf runs loaded"                     "$OUT/loaded.txt"        "loaded=2" line
check "all four processes ended"                       "$OUT/terminated.txt"    "terminated=4" line
check "bin/os.bin not changed by the test"             "$OUT/image.txt"         "unchanged" line
finish task-list
