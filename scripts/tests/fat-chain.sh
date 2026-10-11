#!/usr/bin/env bash
#
# Files of more than one cluster must follow their FAT chain: entry N of the first FAT is at
# byte reserved sectors * sector size + 2 * N (the position used to be multiplied instead).
#
# The test image is a copy of bin/os.bin with three files added by mtools: chain.bin and frag.bin
# (pattern bytes, 4 clusters each) and programs/fatchain (fatchain.elf, 200 KiB of pattern words
# in .rodata). scripts/tests/lib/fatimg.py then moves the second and third clusters of frag.bin
# and fatchain.elf to free clusters further on (both FAT copies, old clusters filled with 0xDB),
# so their chains jump forward and back; mtools must still read them back unchanged.
#
# Host check (scripts/tests/host/fat-chain.c): the real file.c, pparser.c, fat16.c and
# streamer.c read each file with one fread() from position 0; every byte must match, and every
# FAT position fat16.c seeks to must be the BPB formula's entry of a chain cluster.
# QEMU: fatchain.elf must start and find every pattern word, and blank.elf must start afterwards.
# bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=60

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/fat-chain
PROGRAM=programs/fatchain/build/fatchain.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "fat-chain: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT"/*.elf

# Pattern files of 4 clusters (the last one partly used), different for each file.
$FATIMG info bin/os.bin > "$OUT/geometry.txt"
. "$OUT/geometry.txt"
python3 - "$OUT" "$cluster_bytes" <<'EOF'
import sys
out, cluster = sys.argv[1], int(sys.argv[2])
size = 3 * cluster + 333
for name, seed in (('chain.bin', 11), ('frag.bin', 29)):
    data = bytes(((i * seed) ^ (i >> 8) ^ (i >> 16) ^ seed) & 0xFF for i in range(size))
    open('%s/%s' % (out, name), 'wb').write(data)
EOF

cp bin/os.bin "$OUT/fat-chain.bin"
for f in "$OUT/chain.bin" "$OUT/frag.bin" "$PROGRAM"; do
    mcopy -i "$OUT/fat-chain.bin" "$f" ::
done
{
    for f in frag.bin fatchain.elf; do
        $FATIMG relocate "$OUT/fat-chain.bin" "$f" 1 > /dev/null
        $FATIMG relocate "$OUT/fat-chain.bin" "$f" 2 > /dev/null
    done
    for f in chain.bin frag.bin fatchain.elf; do
        $FATIMG entry "$OUT/fat-chain.bin" "$f" | sed -n "s/^chain=/$f /p"
        mcopy -n -i "$OUT/fat-chain.bin" "::$f" "$OUT/readback-$f"
    done
} > "$OUT/chains.txt" 2>&1 || true
sed 's/^/  /' "$OUT/chains.txt"
{
    cmp -s "$OUT/readback-chain.bin" "$OUT/chain.bin" && cmp -s "$OUT/readback-frag.bin" "$OUT/frag.bin" &&
        cmp -s "$OUT/readback-fatchain.elf" "$PROGRAM" && echo "mtools reads every file back unchanged"
    # A fragmented chain has a link that goes backwards.
    for f in frag.bin fatchain.elf; do
        awk -v f="$f" '$1 == f { n = split($2, c, ","); for (i = 2; i <= n; i++) if (c[i] + 0 < c[i - 1] + 0) back = 1;
            if (back && n >= 4) print f " is fragmented" }' "$OUT/chains.txt"
    done
    # The pattern words of fatchain.elf reach past its second cluster.
    python3 - "$PROGRAM" "$cluster_bytes" <<'EOF'
import struct, sys
data, cluster = open(sys.argv[1], 'rb').read(), int(sys.argv[2])
shoff, shentsize, shnum, shstrndx = struct.unpack_from('<I', data, 32)[0], *struct.unpack_from('<HHH', data, 46)
sections = [struct.unpack_from('<IIIIII', data, shoff + i * shentsize) for i in range(shnum)]
strtab = sections[shstrndx][4]
for name, _type, _flags, _addr, offset, size in sections:
    if data[strtab + name:data.index(b'\0', strtab + name)] == b'.rodata' and offset + size > 2 * cluster:
        print('fatchain.elf pattern data reaches cluster 3')
EOF
} > "$OUT/fixture.txt" 2>&1 || true
check "fixture: mtools reads every file back unchanged"  "$OUT/fixture.txt" "mtools reads every file back unchanged" line
check "fixture: frag.bin chain is fragmented"            "$OUT/fixture.txt" "frag.bin is fragmented" line
check "fixture: fatchain.elf chain is fragmented"        "$OUT/fixture.txt" "fatchain.elf is fragmented" line
check "fixture: fatchain.elf data reaches cluster 3"     "$OUT/fixture.txt" "fatchain.elf pattern data reaches cluster 3" line

echo "fat-chain: host check of multi-cluster reads"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-chain.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$OUT/fat-chain.bin" "CHAIN   BIN" "$OUT/chain.bin" "FRAG    BIN" "$OUT/frag.bin" \
            "FATCHAINELF" "$PROGRAM" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"         "$OUT/host-status.txt" "exit=0" line
check "host check: every file read back through the FAT"  "$OUT/host-check.log"  "fat-chain host check: 0 of 3 files failed" line

qemu_start "$OUT/fat-chain.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "fatchain.elf"
    if wait_screen "fatchain: done" "$RUN_TIMEOUT" && wait_screen "Process was terminated" "$RUN_TIMEOUT"; then
        type_line "blank.elf after"
        wait_screen "Blank loaded" "$RUN_TIMEOUT" || true
        wait_screen "after" "$RUN_TIMEOUT" || true
    fi
fi
qemu_stop || true

check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "fatchain.elf found every pattern word"       "$OUT/screen.txt"   "fatchain: PASS 51200 words"
check "fatchain.elf reported no bad word"           "$OUT/screen.txt"   "fatchain: FAIL" absent
check "fatchain.elf finished"                       "$OUT/screen.txt"   "fatchain: done"
check "blank.elf started afterwards"                "$OUT/screen.txt"   "after" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish fat-chain
