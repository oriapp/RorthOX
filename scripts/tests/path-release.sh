#!/usr/bin/env bash
#
# fopen() must give back the path it parsed, whether the open works or not.
#
# Host check (scripts/tests/host/path-release.c): builds the real kernel/fs/file.c, pparser.c,
# fat/fat16.c and disk/streamer.c with the host C compiler and opens files from a copy of
# bin/os.bin (with a directory "dir" holding a second blank.elf added): two files that open and
# close, a missing file, a bad mode, a missing disk, a disk without a filesystem, the root alone,
# a full descriptor table and a failed descriptor allocation. Nothing may stay allocated.
# The QEMU side is in process-release.sh (a launch keeps only the shell's two command nodes) and
# in the heap-oom.sh cases (open-oom.sh, argument-oom.sh): a failed launch keeps no kernel block.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/path-release
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"

echo "path-release: host check of fopen() and the parsed path"
[ -f bin/os.bin ] && [ -f "$BLANK_ELF" ] || { echo "path-release: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-disk.bin"
host_status="not built"
cp bin/os.bin "$OUT/host-disk.bin"
if mmd -i "$OUT/host-disk.bin" ::dir && mcopy -i "$OUT/host-disk.bin" "$BLANK_ELF" ::dir/ &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/path-release.c scripts/tests/host/kalloc.c kernel/fs/pparser.c kernel/fs/file.c \
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

failed=0
if [ "$host_status" = 0 ]; then
    echo "  ok   host check built and exited with status 0"
else
    echo "  FAIL host check built and exited with status 0 (got: $host_status)"
    failed=1
fi
if grep -qF "path-release host check: 0 of" "$OUT/host-check.log" 2> /dev/null; then
    echo "  ok   host check: every fopen() case gave its path back"
else
    echo "  FAIL host check: every fopen() case gave its path back"
    failed=1
fi
if [ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ]; then
    echo "  ok   bin/os.bin not changed by the test"
else
    echo "  FAIL bin/os.bin not changed by the test"
    failed=1
fi
[ "$failed" -eq 0 ] && echo "path-release: all checks passed" || { echo "path-release: a check failed" >&2; exit 1; }
