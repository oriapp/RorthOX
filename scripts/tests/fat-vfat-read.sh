#!/usr/bin/env bash
#
# VFAT long names, read side: the extended READ open finds files by long name or by short alias,
# and a damaged long name is never trusted. Host checks of the real file.c, pparser.c, fat16.c,
# vfat.c and streamer.c on the image disk model (scripts/tests/host/fat-image.c), built with
# AddressSanitizer and UndefinedBehaviorSanitizer (any report fails the check).
#
# The images come from scripts/tests/lib/vfat-fixtures.py, all names written by mtools:
#   legacy.img   the legacy layout with its root label, long names of 13, 26 and 255 characters
#                (the 255 one crosses a root sector), case variants, a long-named directory tree;
#   foreign.img  mkfs.fat with 512-byte clusters: 'Split directory' has a non-contiguous chain
#                and long names that cross from one of its clusters into the next;
#   damaged copies of foreign.img, one fault each (checksum, ordinal gap/order/range, LAST flag,
#                type byte, first-cluster word, padding, more than 255 characters, characters
#                outside printable ASCII or not allowed in names, '.'/'..'), crafted records
#                (a directory with first cluster 0, 1, past the end or 0xFFFF; two records with
#                the same name; a long name whose short entry was deleted; odd short names; a
#                long name on a volume label).
#
# scripts/tests/host/fat-vfat-read.c (only public file calls, so it also builds on a kernel
# without long names) runs each line of cases.txt:
#   - long names (in any letter case), aliases, long and short parents open and read exactly the
#     bytes mtools wrote; no proper prefix of any long name opens;
#   - a damaged long name gives -ENOENT and no prefix of it opens, while its alias still reads
#     the file; the volume label gives -ENOENT; a directory with a bad first cluster gives -EIO;
#     a name that two records answer to (long + long, or long + alias) gives -EIO;
#   - every allocation and every disk read failing in turn: -ENOMEM / an error, nothing left
#     allocated, never wrong bytes;
#   - the old entry points (fopen_status, file_open) give what they gave before long names.
# scripts/tests/host/fat-vfat-codec.c checks the codec and the record scanner directly
# (checksums against mtools, decoding, the 255-character bound, ordinals out of range, name
# rules, short-name display and matching, directory listings with no allocation).
#
# fsck.fat -n: the valid images are clean; each damaged image gives exactly its pinned exit
# status and findings (vfat-fixtures.py, measured with dosfstools 4.2; much damage is invisible
# to it). Env: HOST_CC=cc   FSCK_FAT=fsck.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-vfat-read
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-vfat-read: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -rf "$OUT/fixtures"
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.log

echo "fat-vfat-read: fixtures"
if python3 -B scripts/tests/lib/vfat-fixtures.py "$OUT/fixtures" > "$OUT/fixtures.log" 2>&1; then
    echo built > "$OUT/fixtures.txt"
else
    sed 's/^/  /' "$OUT/fixtures.log"
fi
check "fixtures built by mtools" "$OUT/fixtures.txt" "built" line

# host_run <name> <sources...>: build <name> from the sources and run it on the fixtures.
host_run() {
    local name="$1" status="not built"
    shift
    if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
            -fsanitize=address,undefined -fno-sanitize-recover=all \
            -I kernel -I build/config -I scripts/tests/host "$@" \
            scripts/tests/host/fat-image.c scripts/tests/host/kalloc.c \
            kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/vfat.c kernel/disk/streamer.c \
            kernel/string/string.c kernel/memory/memory.c \
            -o "$OUT/host-$name" > "$OUT/host-$name-build.log" 2>&1; then
        if ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
                "$OUT/host-$name" "$OUT/fixtures" > "$OUT/host-$name.log" 2>&1; then status=0; else status=$?; fi
        sed 's/^/  /' "$OUT/host-$name.log"
    else
        echo "  host build of $name failed, see $OUT/host-$name-build.log"
        head -n 10 "$OUT/host-$name-build.log" | sed 's/^/  /' || true
    fi
    echo "exit=$status" > "$OUT/host-$name-status.txt"
}

echo "fat-vfat-read: lookups through the file calls"
host_run read scripts/tests/host/fat-vfat-read.c kernel/fs/fat/fat16.c
check "read check built and exited with status 0" "$OUT/host-read-status.txt" "exit=0" line
CASES=$(grep -c . "$OUT/fixtures/cases.txt" 2> /dev/null || echo 0)
check "read check: all $CASES cases passed" "$OUT/host-read.log" "fat-vfat-read host check: 0 of $CASES cases failed" line
for k in ext fopen open prefixes oom readfail; do
    grep -E "^  $k: [1-9][0-9]* cases, 0 failed$" "$OUT/host-read.log" > /dev/null 2>&1 && echo "$k ran" >> "$OUT/kinds.txt" || true
    check "read check: $k cases" "$OUT/kinds.txt" "$k ran" line
done

echo "fat-vfat-read: codec and record scanner"
host_run codec scripts/tests/host/fat-vfat-codec.c   # includes fat16.c
check "codec check built and exited with status 0" "$OUT/host-codec-status.txt" "exit=0" line
check "codec check: every part passed" "$OUT/host-codec.log" "fat-vfat-codec host check: 0 of 7 parts failed" line
for p in checksum decode damaged names short listing lookup; do
    grep -E "^  $p: [1-9][0-9]* checks, 0 failed$" "$OUT/host-codec.log" > /dev/null 2>&1 && echo "$p ran" >> "$OUT/parts.txt" || true
    check "codec check: $p" "$OUT/parts.txt" "$p ran" line
done

echo "fat-vfat-read: fsck.fat"
[ -s "$OUT/fixtures/fsck.txt" ] || echo "missing.img	0	0	-" > "$OUT/fixtures/fsck.txt"
while IFS=$'\t' read -r image clusters status diag; do
    if [ "$diag" = - ]; then
        fsck_clean "fsck.fat: $image is clean" "$OUT/fixtures/$image" "$clusters" "$OUT/fsck-${image%.img}.log"
    else
        fsck_expect "fsck.fat: $image gives exit $status and its pinned findings" "$OUT/fixtures/$image" \
            "$clusters" "$status" "$OUT/fsck-${image%.img}.log" "$diag"
    fi
done < "$OUT/fixtures/fsck.txt"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-vfat-read: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-vfat-read: all checks passed"
