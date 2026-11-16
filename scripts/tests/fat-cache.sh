#!/usr/bin/env bash
#
# The FAT sector cache: fat16.c keeps the last sector of the first FAT it read and serves FAT
# entry lookups in that sector from it. Host check (scripts/tests/host/fat-cache.c) of the real
# file.c, pparser.c, fat16.c and streamer.c on the image disk model (scripts/tests/host/
# fat-image.c), with bin/os.bin as the disk; it counts every physical sector read:
#
#   - 1024 lookups of the 256 entries of one FAT sector read that sector once and nothing else;
#     a 520-cluster file read reads each FAT sector at most once per run of its links in it,
#     and never the second FAT;
#   - a lookup in another sector reads that one (A, B, A: three reads, right values); a chain
#     whose every link changes FAT sector reads back in its order;
#   - after a failed sector read (the lookup returns the read's error) the next lookup reads its
#     sector again, whichever it is;
#   - after a write of the first FAT the next lookup reads the sector and sees the new value,
#     and it still does after the second copy is written and flushed; files rewritten after a
#     warm read read back, and the two FATs agree on the media;
#   - a failed write or flush, and every write preparation, drop the cache;
#   - write preparation compares the copies on the disk, so a warm first-FAT sector never hides
#     a difference (either copy changed behind the cache gives -EIO, a file write is refused).
#
# Built with AddressSanitizer. Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check() and finish()

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-cache
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-cache: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/*.txt

echo "fat-cache: host check of the FAT sector cache"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-cache.c scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" bin/os.bin > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every case passed"              "$OUT/host-check.log"  "fat-cache host check: 0 of 6 cases failed" line
for c in "one read per sector" "eviction" "failed fill" "both copies" "uncertain write" "preparation"; do
    grep -E "^  $c: [1-9][0-9]* checks, 0 failed$" "$OUT/host-check.log" > /dev/null 2>&1 && echo "$c ran" >> "$OUT/cases.txt" || true
    check "host check: $c"  "$OUT/cases.txt" "$c ran" line
done

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-cache: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-cache: all checks passed"
