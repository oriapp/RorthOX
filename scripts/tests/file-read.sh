#!/usr/bin/env bash
#
# fread() must move the file position by what it read, stop at the end of the file, count only
# whole items, and take each cluster's bytes from the cluster chain; elf_load() must not take a
# short read for a whole file.
#
# The test image is a copy of bin/os.bin with pat.bin (3 clusters and 333 bytes of pattern; its
# second cluster is moved by scripts/tests/lib/fatimg.py, so the chain jumps and the cluster after
# the first one on disk is filled with 0xDB), small.bin (5000 bytes), empty.bin (0 bytes), huge.bin
# (size field set to 0xFFFFFFF0), far.bin (first cluster set to 0xFF00) and a directory "dir".
#
# Host check (scripts/tests/host/file-read.c): the real file.c, pparser.c, fat16.c and streamer.c
# read these files in small steps, past the end, with a failing sector, across the moved
# cluster, and at positions that don't fit an int; see that file for the cases.
# scripts/tests/elf-unwind.sh covers the short-read cases of elf_load(). There is no file read
# syscall, so there is no QEMU part: the fatchain.elf and blank.elf launches of the other tests
# read whole files through the same code. bin/os.bin must not change.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/file-read
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "file-read: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log

IMG="$OUT/file-read.img"
rm -f "$IMG"
$FATIMG info bin/os.bin > "$OUT/geometry.txt"
. "$OUT/geometry.txt"
python3 - "$OUT" "$cluster_bytes" <<'EOF'
import sys
out, cluster = sys.argv[1], int(sys.argv[2])
def pattern(size, seed):
    return bytes(((i * seed) ^ (i >> 7) ^ (i >> 13) ^ seed) & 0xFF for i in range(size))
open(out + '/pat.bin', 'wb').write(pattern(3 * cluster + 333, 37))
open(out + '/small.bin', 'wb').write(pattern(5000, 53))
open(out + '/empty.bin', 'wb').write(b'')
open(out + '/huge.bin', 'wb').write(pattern(100, 61))
open(out + '/far.bin', 'wb').write(pattern(100, 67))
EOF
cp bin/os.bin "$IMG"
for f in pat.bin small.bin empty.bin huge.bin far.bin; do
    mcopy -i "$IMG" "$OUT/$f" ::
done
mmd -i "$IMG" ::dir
{
    $FATIMG relocate "$IMG" pat.bin 1 | sed -n 's/^chain=/pat.bin chain=/p'
    $FATIMG set-size "$IMG" huge.bin 0xFFFFFFF0 > /dev/null
    $FATIMG set-cluster "$IMG" far.bin 0xFF00 > /dev/null
    for f in pat.bin small.bin empty.bin; do
        mcopy -n -i "$IMG" "::$f" "$OUT/readback-$f"
        cmp -s "$OUT/readback-$f" "$OUT/$f" && echo "$f reads back unchanged"
    done
    $FATIMG slots "$IMG" | grep -c ' used ' | sed 's/^/root entries=/'
} > "$OUT/fixture.txt" 2>&1 || true
sed -n 's/^pat.bin chain=/  pat.bin chain: /p' "$OUT/fixture.txt"
check "fixture: pat.bin reads back unchanged"   "$OUT/fixture.txt" "pat.bin reads back unchanged" line
check "fixture: small.bin reads back unchanged" "$OUT/fixture.txt" "small.bin reads back unchanged" line
check "fixture: empty.bin reads back unchanged" "$OUT/fixture.txt" "empty.bin reads back unchanged" line
check "fixture: pat.bin's chain was moved"      "$OUT/fixture.txt" "pat.bin chain="

echo "file-read: host check of fread"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/file-read.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$IMG" "$OUT/pat.bin" "$OUT/small.bin" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
    { grep -v '^  ok' "$OUT/host-check.log" | head -n 16; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every fread case passed"         "$OUT/host-check.log"  "file-read host check: 0 of 12 cases failed" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "file-read: $failures check(s) failed" >&2
    exit 1
fi
echo "file-read: all checks passed"
