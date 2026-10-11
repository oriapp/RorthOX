#!/usr/bin/env bash
#
# A FAT16 entry that isn't a data cluster must never be used as one: 0 and 1, the reserved values
# 0xFFF0-0xFFF6, the bad cluster mark 0xFFF7 and the end of chain marks 0xFFF8-0xFFFF end a read
# that needs another cluster with -EIO. A failed read of the FAT itself ends it with that read's
# own error (the error used to be taken as the next cluster). FAT12's marks 0xFF0-0xFFF are
# ordinary FAT16 clusters.
#
# The test image is a copy of bin/os.bin with pair.bin (2 clusters) and four.bin (4 clusters) of
# pattern bytes added by mtools. Host check (scripts/tests/host/fat-entry.c): the real file.c,
# pparser.c, fat16.c and streamer.c read them, with pair.bin's first link changed to each value
# and with each FAT read of four.bin failing in turn; besides fread()'s result, every seek and
# sector read is checked, because a value used as a cluster can end in the same -EIO further on.
# The same values as a file's first cluster must be refused before any disk access (fat-entry.c
# includes fat16.c to call that check directly).
# bin/os.bin must not change.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OUT=build/tests/fat-entry
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
mkdir -p "$OUT"
[ -f bin/os.bin ] || { echo "fat-entry: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin
failed=0
result() {
    if [ "$1" = ok ]; then echo "  ok   $2"; else echo "  FAIL $2"; failed=1; fi
}

$FATIMG info bin/os.bin > "$OUT/geometry.txt"
. "$OUT/geometry.txt"
python3 - "$OUT" "$cluster_bytes" <<'EOF'
import sys
out, cluster = sys.argv[1], int(sys.argv[2])
for name, size, seed in (('pair.bin', cluster + 777, 13), ('four.bin', 3 * cluster + 99, 37)):
    data = bytes(((i * seed) ^ (i >> 8) ^ (i >> 16) ^ seed) & 0xFF for i in range(size))
    open('%s/%s' % (out, name), 'wb').write(data)
EOF
cp bin/os.bin "$OUT/fat-entry.bin"
mcopy -i "$OUT/fat-entry.bin" "$OUT/pair.bin" "$OUT/four.bin" ::
pair_chain=$($FATIMG entry "$OUT/fat-entry.bin" pair.bin | sed -n 's/^chain=//p')
four_chain=$($FATIMG entry "$OUT/fat-entry.bin" four.bin | sed -n 's/^chain=//p')
echo "  pair.bin $pair_chain, four.bin $four_chain"
[ "$(echo "$pair_chain" | tr ',' '\n' | wc -l)" -eq 2 ] && [ "$(echo "$four_chain" | tr ',' '\n' | wc -l)" -eq 4 ] &&
    result ok "fixture: pair.bin has 2 clusters, four.bin 4" || result fail "fixture: pair.bin has 2 clusters, four.bin 4"

echo "fat-entry: host check of FAT16 entry values"
host_status="not built"
rm -f "$OUT/host-check.log"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-entry.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$OUT/fat-entry.bin" "$OUT/pair.bin" "$OUT/four.bin" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
    { grep -E '^  FAIL' "$OUT/host-check.log" | head -n 14; tail -n 1 "$OUT/host-check.log"; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
[ "$host_status" = 0 ] && result ok "host check built and exited with status 0" ||
    result fail "host check built and exited with status 0 (got: $host_status)"
grep -qxF "fat-entry host check: 0 of 59 cases failed" "$OUT/host-check.log" 2> /dev/null &&
    result ok "host check: all 59 entry cases passed" || result fail "host check: all 59 entry cases passed"
[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && result ok "bin/os.bin not changed by the test" ||
    result fail "bin/os.bin not changed by the test"
[ "$failed" -eq 0 ] && echo "fat-entry: all checks passed" || { echo "fat-entry: a check failed" >&2; exit 1; }
