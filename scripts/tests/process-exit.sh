#!/usr/bin/env bash
#
# Ending a process must never leave it half destroyed, and must give back everything it had.
#
# Host check (scripts/tests/host/process-exit.c): the real kernel/task/process.c, task.c,
# loader/formats/elfloader.c and isr80h/process.c, built with the host C compiler on the
# stand-ins of scripts/tests/host/process-host.c, end a child (blank.elf, and a flat binary)
# that has arguments, an allocation and open files:
# - with an invalid file type, process_terminate fails before it changes anything (task, page
#   directory, user mappings, allocations, open files, slots, keyboard process, heap), and the
#   process ends normally once the type is put back;
# - process_terminate (the fault path) and syscall 9 (exit) give back the whole heap, destroy the
#   page directory before any memory it maps, and syscall 9 runs the caller next;
# - syscall 9 with an invalid file type stops the kernel (PANIC) with nothing changed;
# - when revoking an allocation fails, at exit or while a launch gives back its argument copies,
#   the allocation is still freed once the page directory is gone.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/process-exit
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt"
[ -f "$BLANK_ELF" ] || { echo "process-exit: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }

echo "process-exit: host check of ending a process"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -Wl,--wrap=PANIC \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/process-exit.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
        kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed 's/^/  /' "$OUT/host-build.log" | head -n 10 || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "host check: every case passed" "$OUT/host-check.log" "process-exit host check: 0 of"

if [ "$failures" -ne 0 ]; then
    echo "process-exit: $failures check(s) failed" >&2
    exit 1
fi
echo "process-exit: all checks passed"
