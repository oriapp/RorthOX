#!/usr/bin/env bash
#
# A program load that fails part way must give back everything the new process took (process,
# program data, stack, task, page directory) and must not publish it: no process slot, no task
# in the list, no change of the current task or of the process that gets the keyboard.
#
# Host check (scripts/tests/host/process-load.c): builds kernel/task/process.c, task.c and
# loader/formats/elfloader.c with the host C compiler, with stand-ins for the file and paging
# layers, and makes each allocation, each fopen/fstat/fread and each mapping of loading
# blank.elf (and a flat binary) into slot 1 fail in turn, next to a running caller in slot 0.
# The stack allocation and the mapping failures can only be picked this way: in QEMU the free
# blocks of fclose() always hold the stack, and mappings fail only for bad addresses.
#
# The QEMU side of the same fix is heap-oom.sh (and open-oom.sh, elf-unwind.sh): programs/oomtest
# expects only the command node and the parsed path to stay taken after a failed load.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/load-unwind
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"

echo "load-unwind: host check of process_load_for_slot()"
[ -f "$BLANK_ELF" ] || { echo "load-unwind: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/process-load.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
        kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
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
check "host check: every process load case passed" "$OUT/host-check.log" "process-load host check: 0 of"
if [ "$failures" -ne 0 ]; then
    echo "load-unwind: $failures check(s) failed" >&2
    exit 1
fi
echo "load-unwind: all checks passed"
