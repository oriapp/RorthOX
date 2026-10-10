#!/usr/bin/env bash
#
# Running a command with arguments (syscall 7) must not start or publish the new process until
# its arguments are copied. If copying them fails, the syscall must give back the new process
# and the copies made so far, and leave the caller running with the keyboard.
#
# 1. Host check (scripts/tests/host/system-command.c): builds kernel/isr80h/process.c,
#    kernel/task/process.c, task.c and loader/formats/elfloader.c with the host C compiler, and
#    makes each allocation, each fopen/fstat/fread and each mapping of
#    "blank.elf arg-one arg-two" fail in turn, then checks a launch where nothing fails.
# 2. QEMU: the "arg..." cases of programs/oomtest, run through heap-oom.sh: oomtest.elf builds
#    the command list first, then leaves room for the whole new process (1038 + E blocks) plus
#    0 to 3 blocks, so allocating argv or one of the three argument copies fails. The syscall
#    must return -ENOMEM and keep only the parsed path (3 blocks), blank.elf must not start,
#    the next key must still reach oomtest, and blank.elf must start from the shell afterwards.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   ARGUMENT_OOM_CASES="argv ..." (run only these QEMU cases)
#      and the heap-oom.sh variables (BOOT_TIMEOUT, RUN_TIMEOUT, EXIT_TIMEOUT)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/argument-oom
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
host_failed=0

echo "argument-oom: host check of syscall 7"
[ -f "$BLANK_ELF" ] || { echo "argument-oom: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/system-command.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
        kernel/isr80h/process.c kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed 's/^/  /' "$OUT/host-build.log" | head -n 10 || true
fi
if [ "$host_status" = 0 ]; then
    echo "  ok   host check built and exited with status 0"
else
    echo "  FAIL host check built and exited with status 0 (got: $host_status)"
    host_failed=1
fi
if grep -qF "system-command host check: 0 of" "$OUT/host-check.log" 2> /dev/null; then
    echo "  ok   host check: every syscall 7 case passed"
else
    echo "  FAIL host check: every syscall 7 case passed"
    host_failed=1
fi

if OOM_SUITE=argument-oom OOM_CASES="${ARGUMENT_OOM_CASES:-argv arg1 arg2 arg3}" bash scripts/tests/heap-oom.sh; then
    qemu_rc=0
else
    qemu_rc=$?
fi

if [ "$host_failed" -ne 0 ]; then
    echo "argument-oom: the host check failed" >&2
    exit 1
fi
exit "$qemu_rc"
