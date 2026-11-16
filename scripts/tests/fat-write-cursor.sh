#!/usr/bin/env bash
#
# File writes keep the chain they checked: a handle checks its file's cluster chain once and
# then knows its length and last cluster, and keeps the cluster it wrote last, so a run of small
# writes doesn't walk the chain again. Host check (scripts/tests/host/fat-write-cursor.c) of the
# real file.c, pparser.c, fat16.c and streamer.c on the image disk model (scripts/tests/host/
# fat-image.c), with bin/os.bin as the disk and a file whose every cluster is in another FAT
# sector, so each link followed is one sector read:
#
#   - 64-byte writes over an 8-cluster file follow at most one link per cluster and use no heap;
#     growing a file by 64-byte writes or appends follows no link and takes at most 16 bytes of
#     heap per new cluster;
#   - a handle whose file another handle truncated and rewrote, or grew, writes to the file's new
#     clusters; two appenders taking turns each append at the current end; a peer's write that
#     keeps the size doesn't cost an appender a walk of the chain;
#   - a write one cluster too big for the free space gives -ENOSPC and changes no sector, no
#     heap and no position; a FAT read that fails before any data is written leaves the
#     position and the bytes; a FAT read that fails in the middle of an overwrite gives -EIO,
#     keeps the position, leaves the bytes written before it and turns writes off; a failed FAT
#     or directory write leaves the position and the size, also for a reader in the last
#     cluster when the directory write fails after the new cluster was linked;
#   - the order on the disk stays: data, flush, the new clusters in each FAT, flush after each,
#     the old last cluster's link in each FAT, flush after each, the directory entry, flush.
#
# Every case that ends with a valid volume exports it, and fsck.fat must find nothing on it.
# Built with AddressSanitizer. Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-write-cursor
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-write-cursor: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.img "$OUT"/*.log

echo "fat-write-cursor: host check of file writes"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-write-cursor.c scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" bin/os.bin "$OUT" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every case passed"              "$OUT/host-check.log"  "fat-write-cursor host check: 0 of 8 cases failed" line
for c in "overwrite" "growth" "peer truncate" "two appenders" "same size" "no space" "failed write" "order"; do
    grep -E "^  $c: [1-9][0-9]* checks, 0 failed$" "$OUT/host-check.log" > /dev/null 2>&1 && echo "$c ran" >> "$OUT/cases.txt" || true
    check "host check: $c"  "$OUT/cases.txt" "$c ran" line
done

CLUSTERS=$(python3 -c 'import struct,sys
b = open(sys.argv[1], "rb").read(64)
bps, spc, res, fats, root, small, _, spf = struct.unpack_from("<HBHBHHBH", b, 11)
total = small or struct.unpack_from("<I", b, 32)[0]
print((total - res - fats * spf - root * 32 // bps) // spc)' bin/os.bin)
for c in overwrite growth peer-truncate two-appenders same-size no-space order; do
    if [ -f "$OUT/$c.img" ]; then
        fsck_clean "fsck.fat: the $c image is clean" "$OUT/$c.img" "$CLUSTERS" "$OUT/fsck-$c.log"
    else
        echo "no image" > "$OUT/fsck-$c.txt"
        check "fsck.fat: the $c image is clean" "$OUT/fsck-$c.txt" "exported" line
    fi
done
rm -f "$OUT"/*.img

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-write-cursor: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-write-cursor: all checks passed"
