#!/usr/bin/env bash
#
# The writable image disk that host checks of the FAT code run on (scripts/tests/host/
# fat-image.c, API in fat-image.h), checked on its own: scripts/tests/host/fat-image-model.c.
#
# Three free sectors at the end of the old disk layout with a root label (scripts/tests/lib/
# legacy-fat-image.py labelled, whatever layout bin/os.bin has) get known contents through the disk
# interface the kernel uses (disk_read_block, disk_write_block, disk_flush, disk_write_capacity):
# written sectors stay pending until a flush, a failed write or flush changes nothing, the n-th
# read, write, flush or allocation fails when asked, writes past the reported capacity are
# refused, and the counters and the ledger record every call. Crash cuts (the durable sectors at
# a point of the ledger, plus none, a prefix or any subset of the writes still pending there)
# must give exactly the images worked out by hand, also when exported to a file, also for one
# sector written more times than the image has sectors. Views stay as they were taken. Built with
# AddressSanitizer.
#
# Then the real file.c, pparser.c, fat16.c and streamer.c create a 5000-byte MODEL.TXT on the
# model. Its exports are read by mtools (the file is there with its bytes; the export from
# before has no such file, LC_ALL=C) and by fsck.fat -n (scripts/tests/lib/fsck-fat.sh): the
# images before and after the write must be clean (exit 0, only the version and summary lines),
# and an export with one byte of the second FAT changed must give exit 1 and exactly the
# "FATs differ" finding.
#
# Env: HOST_CC=cc   FSCK_FAT=fsck.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
FSCK_FAT="${FSCK_FAT:-fsck.fat}"
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-image-model
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-image-model: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.img "$OUT"/*.log "$OUT"/expect-* "$OUT"/got-* "$OUT"/*.diag "$OUT"/*.out "$OUT"/*.err
IMG="$OUT/legacy.img"
python3 -B scripts/tests/lib/legacy-fat-image.py labelled "$IMG" > "$OUT/legacy.txt"
CLUSTERS=$(sed -n 's/^clusters=//p' "$OUT/legacy.txt")

echo "fat-image-model: host check of the image disk model"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-image-model.c scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/fs/fat/vfat.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" "$IMG" "$OUT" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every case passed"              "$OUT/host-check.log"  "fat-image host check: 0 of 10 cases failed" line
for c in "layers, ledger and cuts" "one sector written twice" "one sector written many times" "views are copies" \
         "read failures" "write failures" "flush failures" "capacity" "allocation failures" \
         "kernel FAT code on the model"; do
    grep -E "^  $c: [1-9][0-9]* checks, 0 failed$" "$OUT/host-check.log" > /dev/null 2>&1 && echo "$c ran" >> "$OUT/cases.txt" || true
    check "host check: $c"  "$OUT/cases.txt" "$c ran" line
done

# The exports, read by mtools.
(
    export LC_ALL=C
    if [ -f "$OUT/written.img" ] && [ -f "$OUT/before.img" ]; then
        mdir -b -i "$OUT/written.img" :: > "$OUT/mdir-written.txt" 2>&1 && echo "mdir lists written.img"
        grep -qx '::/MODEL.TXT' "$OUT/mdir-written.txt" && echo "MODEL.TXT in written.img"
        mdir -b -i "$OUT/before.img" :: > "$OUT/mdir-before.txt" 2>&1 && ! grep -q MODEL "$OUT/mdir-before.txt" &&
            echo "no MODEL.TXT in before.img"
        mtype -i "$OUT/written.img" ::MODEL.TXT > "$OUT/got-MODEL.TXT" 2> /dev/null &&
            cmp -s "$OUT/got-MODEL.TXT" "$OUT/expect-MODEL.TXT" && echo "mtype MODEL.TXT matches"
        for f in shell.elf blank.elf; do
            mcopy -n -i "$OUT/written.img" "::$f" "$OUT/got-$f" 2> /dev/null &&
                cmp -s "$OUT/got-$f" "programs/${f%.elf}/build/$f" && echo "$f unchanged"
        done
    fi
) > "$OUT/mtools.txt" 2>&1 || true
check "mtools: mdir reads the written export"   "$OUT/mtools.txt" "mdir lists written.img" line
check "mtools: MODEL.TXT in the written export" "$OUT/mtools.txt" "MODEL.TXT in written.img" line
check "mtools: no MODEL.TXT in the export from before" "$OUT/mtools.txt" "no MODEL.TXT in before.img" line
check "mtools: MODEL.TXT bytes as written"      "$OUT/mtools.txt" "mtype MODEL.TXT matches" line
check "mtools: shell.elf untouched"             "$OUT/mtools.txt" "shell.elf unchanged" line
check "mtools: blank.elf untouched"             "$OUT/mtools.txt" "blank.elf unchanged" line

# fsck.fat -n (never repair) on the exports, with the shared check: the fixture and the written
# image are clean, the one with FAT 2 changed gives exactly its finding.
cat > "$OUT/fat2.diag" << 'EOF'
FATs differ but appear to be intact.
  Using first FAT.

Leaving filesystem unchanged.
EOF
fsck_clean  "fsck.fat: the export from before is clean" "$OUT/before.img"  "$CLUSTERS" "$OUT/fsck-before.log"
fsck_clean  "fsck.fat: the written export is clean"     "$OUT/written.img" "$CLUSTERS" "$OUT/fsck-written.log"
fsck_expect "fsck.fat: FAT 2 changed gives exit 1 and only that finding" "$OUT/fat2.img" "$CLUSTERS" 1 \
    "$OUT/fsck-fat2.log" "$OUT/fat2.diag"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-image-model: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-image-model: all checks passed"
