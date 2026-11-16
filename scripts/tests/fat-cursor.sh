#!/usr/bin/env bash
#
# Read cursors: fat16.c remembers, per open file, the cluster of the last read, so sequential
# reads and forward seeks walk the FAT chain only from there. Host check (scripts/tests/host/
# fat-cursor.c) of the real file.c, pparser.c, fat16.c and streamer.c on the image disk model
# (scripts/tests/host/fat-image.c), with bin/os.bin plus a fragmented 16-cluster FRAG.BIN as the
# disk. Its clusters alternate between two FAT sectors, so each link walked reads one FAT sector:
#
#   - the whole file read with file_read or fread, 1 or 4096 bytes per call, walks at most 15
#     links (one per cluster boundary), not one walk from the first cluster per call;
#   - reads that end exactly at a cluster boundary, start at one or cross one give the right
#     bytes and walk at most one link for the next cluster;
#   - forward seeks walk only from the last cluster read, backward seeks (file_seek, fseek) read
#     the right bytes, two open files each keep their place;
#   - every refused seek leaves the position and the cursor as they were;
#   - a read whose n-th disk read fails (every n, FAT and data sectors): file_read keeps its
#     position, fread the items read before it; the retry reads the right bytes and walks only
#     from the last cluster read;
#   - a file truncated and written again elsewhere through another handle, with poison in its
#     old clusters: readers see end of file, then the new bytes at their old position;
#   - with the chain looped back to its start and a size past the longest chain the volume can
#     have, a read in the cluster after that gives -EIO, also with the cursor right before it.
#
# The image with FRAG.BIN and the one after the rewrite must pass fsck.fat -n (scripts/tests/lib/
# fsck-fat.sh). Built with AddressSanitizer. Env: HOST_CC=cc   FSCK_FAT=fsck.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
FSCK_FAT="${FSCK_FAT:-fsck.fat}"
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-cursor
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-cursor: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.img "$OUT"/*.log

echo "fat-cursor: host check of the read cursors"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-cursor.c scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/fs/fat/vfat.c kernel/disk/streamer.c \
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
check "host check: every case passed"              "$OUT/host-check.log"  "fat-cursor host check: 0 of 7 cases failed" line
for c in "sequential reads" "cluster boundaries" "seeks" "failed seeks" "failed reads" "truncate" "looped chain"; do
    grep -E "^  $c: [1-9][0-9]* checks, 0 failed$" "$OUT/host-check.log" > /dev/null 2>&1 && echo "$c ran" >> "$OUT/cases.txt" || true
    check "host check: $c"  "$OUT/cases.txt" "$c ran" line
done

CLUSTERS=$(sed -n 's/^clusters \([0-9]*\)$/\1/p' "$OUT/host-check.log")
fsck_clean "fsck.fat: the image with FRAG.BIN is clean"   "$OUT/fixture.img"   "${CLUSTERS:-unknown}" "$OUT/fsck-fixture.log"
fsck_clean "fsck.fat: the image after the rewrite is clean" "$OUT/rewritten.img" "${CLUSTERS:-unknown}" "$OUT/fsck-rewritten.log"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-cursor: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-cursor: all checks passed"
