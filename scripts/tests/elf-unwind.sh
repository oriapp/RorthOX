#!/usr/bin/env bash
#
# A failed ELF load must give back the ELF object and the file buffer and close the file, and a
# file that is not an ELF file must be gone from the heap before the kernel tries it as a flat
# binary.
#
# 1. Host check (scripts/tests/host/elf-load.c): builds kernel/loader/formats/elfloader.c, and
#    process_load_elf()/process_load_data() cut out of kernel/task/process.c, with the host C
#    compiler, and makes the object allocation, fopen, fstat, the buffer allocation, fread and
#    the format check fail in turn.
# 2. QEMU: the "elfdata" case of programs/oomtest, run through heap-oom.sh: allocating the
#    buffer for blank.elf fails, and only the command node, the process and the parsed path may
#    stay taken (7 blocks, not 8 with the ELF object). blank.elf must start from the shell
#    afterwards.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   and the heap-oom.sh variables (BOOT_TIMEOUT, RUN_TIMEOUT, EXIT_TIMEOUT)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/elf-unwind
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
host_failed=0

echo "elf-unwind: host check of elf_load()"
[ -f "$BLANK_ELF" ] || { echo "elf-unwind: $BLANK_ELF not found, run 'make all' first" >&2; exit 1; }
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/process-load-data.c"
host_status="not built"
# The two functions that pick the ELF loader or the flat binary loader, as they are in process.c.
awk '/^static int process_load_elf\(/,/^}/' kernel/task/process.c > "$OUT/process-load-data.c"
awk '/^static int process_load_data\(/,/^}/' kernel/task/process.c >> "$OUT/process-load-data.c"
if [ "$(grep -c '^}' "$OUT/process-load-data.c")" -ne 2 ]; then
    echo "  could not find process_load_elf() and process_load_data() in kernel/task/process.c"
elif "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host -I "$OUT" \
        scripts/tests/host/elf-load.c scripts/tests/host/kalloc.c kernel/loader/formats/elfloader.c \
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
if grep -qF "elf-load host check: 0 of" "$OUT/host-check.log" 2> /dev/null; then
    echo "  ok   host check: every elf_load case passed"
else
    echo "  FAIL host check: every elf_load case passed"
    host_failed=1
fi

if OOM_SUITE=elf-unwind OOM_CASES=elfdata bash scripts/tests/heap-oom.sh; then
    qemu_rc=0
else
    qemu_rc=$?
fi

if [ "$host_failed" -ne 0 ]; then
    echo "elf-unwind: the host check failed" >&2
    exit 1
fi
exit "$qemu_rc"
