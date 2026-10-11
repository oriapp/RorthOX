#!/usr/bin/env bash
#
# File descriptors are 1 to RORTHOX_MAX_FILE_DESCRIPTORS (slots 0 to MAX - 1), so the last one
# must work like the others: fstat, fread, fseek and fclose on it, and closing it frees its slot.
#
# Host check only (scripts/tests/host/file-descriptors.c, built with AddressSanitizer so a read
# past the end of the descriptor table stops it): the real file.c, pparser.c, fat16.c and
# streamer.c on a copy of bin/os.bin open blank.elf until the table is full, use and close the
# last descriptor, reuse slots, and try descriptors out of range. A user program can't hold
# hundreds of files open (there is no file syscall), so there is no QEMU part.
# bin/os.bin must not change.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/file-descriptors
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$BLANK_ELF" ] || { echo "file-descriptors: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.img

IMG="$OUT/file-descriptors.img"
cp bin/os.bin "$IMG"
mcopy -n -i "$IMG" ::blank.elf "$OUT/readback-blank.elf.txt"
cmp -s "$OUT/readback-blank.elf.txt" "$BLANK_ELF" && echo "blank.elf reads back unchanged" > "$OUT/fixture.txt" ||
    echo "blank.elf differs" > "$OUT/fixture.txt"
check "fixture: the image has blank.elf" "$OUT/fixture.txt" "blank.elf reads back unchanged" line

echo "file-descriptors: host check of the descriptor table"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/file-descriptors.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$IMG" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    { grep -v '^  ok' "$OUT/host-check.log" | head -n 12; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every descriptor case passed"   "$OUT/host-check.log"  "file-descriptors host check: 0 of 3 cases failed" present

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "file-descriptors: $failures check(s) failed" >&2
    exit 1
fi
echo "file-descriptors: all checks passed"
