#!/usr/bin/env bash
#
# Writing files: creating, writing, appending and truncating through the real kernel/fs/file.c,
# pparser.c, fat/fat16.c and disk/streamer.c, built with the host C compiler, on a copy of
# bin/os.bin held in memory (scripts/tests/host/fat-write.c). No QEMU.
#
# The host check's disk keeps written sectors pending until a flush and logs every write and
# flush. Its own FAT16 reader (not the kernel's) checks every chain in both FAT copies, the file
# contents, both FATs equal, exact chain lengths, no leaked cluster and zeroed tails, also for the
# disk as it would be after a crash at every point of a create, an append and a truncate (data,
# then the new chain in both FATs, then the old tail, then the entry; a truncated entry before any
# freed cluster). Writes, flushes and reads of an append fail in turn: -EIO, no further change
# (later writes, creates and truncates are refused), crash-safe either way. Every allocation of a
# WRITE open fails in turn: the file is not touched. The writable cluster limit comes from the
# sector count the disk reports (the image size rounded up, like QEMU's IDENTIFY), never from the
# BPB alone, and on a drive past 2 GiB only clusters that end below byte INT_MAX are used (the
# model's disk grows to 2 GiB for that case). Also: full root and directory (no growth), writes to
# files whose entries are in a directory's second cluster (only their own slot changes),
# deleted/LFN/label slots, a long-name entry right before the end marker (-EIO), a directory past
# the reported capacity (create refused before any write, writing stays on), four FAT copies or
# hidden sectors (writing refused, reading works), 8.3 names, bad chains, FAT copies that differ,
# no space, open descriptors that see a truncate or an append.
#
# The image is the legacy layout with a root label (scripts/tests/lib/legacy-fat-image.py
# labelled: 64 KiB clusters, 64 root entries, 61,029 clusters on a 16 MiB file), whatever layout
# bin/os.bin has. The "save" case leaves the image before (fixture.img) and after (written.img) a
# create, an append, a truncate and a create in a subdirectory: mdir and mtype must show the
# expected files and bytes, and fsck.fat -n must find nothing on either
# (scripts/tests/lib/fsck-fat.sh: exit 0, only the version and summary lines, 61,029 clusters).
#
# Env: HOST_CC=cc   FSCK_FAT=fsck.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-write
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-write: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.img "$OUT"/*.log "$OUT"/expect-* "$OUT"/got-*

IMG="$OUT/os.bin"
python3 -B scripts/tests/lib/legacy-fat-image.py labelled "$IMG" > "$OUT/legacy.txt"

echo "fat-write: host check of writing files"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-write.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$IMG" "$OUT" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    { grep '^  FAIL' "$OUT/host-check.log" -A 2 | head -n 20; grep -v '^ ' "$OUT/host-check.log"; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
# The sweeps and crash cuts must have run (not zero cases).
awk '/crash states checked/ && $2 >= 1000 { print "crash states reached" }
     /each of the [0-9]+ writes failed in turn/ && $5 >= 100 { print "write failures reached" }
     /each of the [0-9]+ flushes failed in turn/ && $5 >= 6 { print "flush failures reached" }
     /each of the [0-9]+ reads failed in turn/ && $5 >= 10 { print "read failures reached" }
     /allocation failures in WRITE opens/ && $2 >= 6 { print "allocation failures reached" }
     /a drive past INT_MAX bytes is written up to cluster/ { print "INT_MAX limit reached" }' \
    "$OUT/host-check.log" > "$OUT/host-reached.txt" 2> /dev/null || true
check "host check built and exited with status 0"         "$OUT/host-status.txt"  "exit=0" line
check "host check: every case passed"                     "$OUT/host-check.log"   "fat-write host check: 0 of"
check "host check: at least 1000 crash states checked"    "$OUT/host-reached.txt" "crash states reached" line
check "host check: each disk write of an append failed"   "$OUT/host-reached.txt" "write failures reached" line
check "host check: each flush of an append failed"        "$OUT/host-reached.txt" "flush failures reached" line
check "host check: each disk read of an append failed"    "$OUT/host-reached.txt" "read failures reached" line
check "host check: allocations of WRITE opens failed"     "$OUT/host-reached.txt" "allocation failures reached" line
check "host check: a drive past INT_MAX bytes was written" "$OUT/host-reached.txt" "INT_MAX limit reached" line

# The saved images, read by mtools.
{
    if [ -f "$OUT/written.img" ] && [ -f "$OUT/fixture.img" ]; then
        mdir -i "$OUT/written.img" :: > "$OUT/mdir.txt" 2>&1 && echo "mdir lists written.img"
        mdir -i "$OUT/written.img" ::dir >> "$OUT/mdir.txt" 2>&1 || true
        awk '$1 == "NEW" && $2 == "BIN" { print "NEW.BIN size " $3 }
             $1 == "OLD" && $2 == "BIN" { print "OLD.BIN size " $3 }
             $1 == "TRUNC" && $2 == "BIN" { print "TRUNC.BIN size " $3 }
             $1 == "NOTE" && $2 == "TXT" { print "NOTE.TXT size " $3 }' "$OUT/mdir.txt"
        for f in NEW.BIN OLD.BIN dir/NOTE.TXT LONGNAME.TXT; do
            mtype -i "$OUT/written.img" "::$f" > "$OUT/got-$(basename "$f")" 2> /dev/null &&
                cmp -s "$OUT/got-$(basename "$f")" "$OUT/expect-$(basename "$f")" && echo "mtype $f matches"
        done
        mtype -i "$OUT/written.img" ::TRUNC.BIN > "$OUT/got-TRUNC.BIN" 2> /dev/null && [ ! -s "$OUT/got-TRUNC.BIN" ] &&
            echo "mtype TRUNC.BIN is empty"
        for f in shell.elf blank.elf; do
            mcopy -n -i "$OUT/written.img" "::$f" "$OUT/got-$f" 2> /dev/null &&
                cmp -s "$OUT/got-$f" "programs/${f%.elf}/build/$f" && echo "$f unchanged"
        done
    fi
} > "$OUT/mtools.txt" 2>&1 || true
check "mtools: mdir reads the written image"      "$OUT/mtools.txt" "mdir lists written.img" line
check "mtools: NEW.BIN is 140000 bytes"           "$OUT/mtools.txt" "NEW.BIN size 140000" line
check "mtools: OLD.BIN is 270000 bytes"           "$OUT/mtools.txt" "OLD.BIN size 270000" line
check "mtools: TRUNC.BIN is 0 bytes"              "$OUT/mtools.txt" "TRUNC.BIN size 0" line
check "mtools: dir/NOTE.TXT is 7 bytes"           "$OUT/mtools.txt" "NOTE.TXT size 7" line
check "mtools: NEW.BIN bytes as written"          "$OUT/mtools.txt" "mtype NEW.BIN matches" line
check "mtools: OLD.BIN bytes old + appended"      "$OUT/mtools.txt" "mtype OLD.BIN matches" line
check "mtools: dir/NOTE.TXT bytes as written"     "$OUT/mtools.txt" "mtype dir/NOTE.TXT matches" line
check "mtools: LONGNAME.TXT untouched"            "$OUT/mtools.txt" "mtype LONGNAME.TXT matches" line
check "mtools: TRUNC.BIN empty"                   "$OUT/mtools.txt" "mtype TRUNC.BIN is empty" line
check "mtools: shell.elf untouched"               "$OUT/mtools.txt" "shell.elf unchanged" line
check "mtools: blank.elf untouched"               "$OUT/mtools.txt" "blank.elf unchanged" line

# fsck.fat -n (never repair) on both saved images.
for tag in fixture written; do
    fsck_clean "fsck.fat: $tag.img clean" "$OUT/$tag.img" 61029 "$OUT/fsck-$tag.log"
done

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-write: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-write: all checks passed"
