#!/usr/bin/env bash
#
# Opening a file must never write through a failed allocation, never return part of a path,
# and must give back what it allocated when it fails.
#
# 1. Host check (scripts/tests/host/open-path.c): builds the real path parser, VFS, FAT16 and
#    disk streamer code with the host C compiler, reads a copy of bin/os.bin (with a directory
#    "dir" holding a second blank.elf added), and makes each allocation, then each disk read, of
#    path parsing, FAT16 setup and fopen fail in turn. See that file for the cases. Allocations
#    inside fat16_load_fat_directory() are not skipped: at least two of them (the directory and
#    its entries) must fail and unwind cleanly.
# 2. QEMU: the "open..." cases of programs/oomtest, run through heap-oom.sh: oomtest.elf leaves
#    5 to 11 free heap blocks, so one allocation of opening blank.elf fails (the path root, the
#    first name, the first path part, the FAT descriptor, the FAT item, the copy of the directory
#    entry, the file descriptor). The syscall must return -EIO, keep only the expected blocks,
#    and blank.elf must start from the shell afterwards.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   OPEN_OOM_CASES="openroot ..." (run only these QEMU cases)
#      and the heap-oom.sh variables (BOOT_TIMEOUT, RUN_TIMEOUT, EXIT_TIMEOUT)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/open-oom
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
host_failed=0

echo "open-oom: host check of the file open path"
[ -f bin/os.bin ] && [ -f "$BLANK_ELF" ] || { echo "open-oom: run 'make all' first" >&2; exit 1; }
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-disk.bin"
host_status="not built"
cp bin/os.bin "$OUT/host-disk.bin"
if mmd -i "$OUT/host-disk.bin" ::dir && mcopy -i "$OUT/host-disk.bin" "$BLANK_ELF" ::dir/ &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/open-path.c scripts/tests/host/kalloc.c kernel/fs/pparser.c kernel/fs/file.c \
        kernel/fs/fat/fat16.c kernel/disk/streamer.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$OUT/host-disk.bin" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
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
if grep -qF "open-path host check: 0 of" "$OUT/host-check.log" 2> /dev/null; then
    echo "  ok   host check: every open path case passed"
else
    echo "  FAIL host check: every open path case passed"
    host_failed=1
fi
if grep -qE "open-path host check: .* \(([2-9]|[1-9][0-9]+) allocation failures inside fat16_load_fat_directory\)" \
        "$OUT/host-check.log" 2> /dev/null; then
    echo "  ok   host check: allocations inside fat16_load_fat_directory failed and unwound"
else
    echo "  FAIL host check: allocations inside fat16_load_fat_directory failed and unwound"
    host_failed=1
fi

if OOM_SUITE=open-oom \
    OOM_CASES="${OPEN_OOM_CASES:-openroot openpart opennode openfat openitem openclone openvfs}" \
    bash scripts/tests/heap-oom.sh; then
    qemu_rc=0
else
    qemu_rc=$?
fi

if [ "$host_failed" -ne 0 ]; then
    echo "open-oom: the host check failed" >&2
    exit 1
fi
exit "$qemu_rc"
