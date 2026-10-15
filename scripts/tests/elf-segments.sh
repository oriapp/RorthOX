#!/usr/bin/env bash
#
# Every ELF load segment must have memory of its own: the p_filesz bytes from the file, zeroes up
# to p_memsz (.bss) and in the rest of its pages, mapped writable only with PF_W. Nothing else of
# the file may be mapped, and a file with the ELF magic that is malformed or unsupported must be
# refused, not run as a flat binary.
#
# 1. Host check (scripts/tests/host/elf-segments.c): builds kernel/task/process.c, task.c and
#    loader/formats/elfloader.c with the host C compiler (page-aligned host heap, unused memory
#    filled with a pattern) and loads made-up ELF files and the real programs: the mappings, the
#    memory behind them, the flags, the release at exit, the malformed files, the flat binary
#    fallback, and each allocation and mapping failing in turn. The malformed and short files
#    also load with the file buffer between unreadable guard pages, so a read outside the file
#    crashes the check.
# 2. The program headers of every built program: all load pages inside the ELF load window
#    [PROGRAM_LOAD_ADDRESS, PROGRAM_LOAD_LIMIT), and programs/elftest has real .data and a .bss
#    over several pages with an odd-sized end.
# 3. QEMU: copy bin/os.bin, add elftest.elf to the copy only, boot, and run "elftest.elf one" and
#    "elftest.elf two". Each run checks its initial data and zeroed .bss and writes every byte.
#    The free kernel heap blocks are counted while the shell is idle before and after each run:
#    each run may keep only the shell's two command nodes. blank.elf must start afterwards.
# bin/os.bin itself must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each run)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=build/tests/elf-segments
PROGRAM=programs/elftest/build/elftest.elf
BLANK_ELF=programs/blank/build/blank.elf
SHELL_ELF=programs/shell/build/shell.elf
CONFIG_H=build/config/rorthox-config.h
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT/host-check" "$OUT/host-build.log" "$OUT/host-check.log" "$OUT/host-status.txt" \
    "$OUT/headers.log" "$OUT/headers-status.txt" "$OUT/counts.txt" "$OUT/image.txt" "$OUT/heap.bin" \
    "$OUT/elf-segments.bin" "$OUT/screen.txt" "$OUT/debugcon.log"

for f in "$PROGRAM" "$BLANK_ELF" "$SHELL_ELF"; do
    [ -f "$f" ] || { echo "elf-segments: $f not found, run 'make all' first" >&2; exit 1; }
done

echo "elf-segments: host check of ELF load segments"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/elf-segments.c scripts/tests/host/process-host.c scripts/tests/host/kalloc.c \
        kernel/task/process.c kernel/task/task.c kernel/loader/formats/elfloader.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$BLANK_ELF" "$PROGRAM" "$SHELL_ELF" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed 's/^/  /' "$OUT/host-build.log" | head -n 10 || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"   "$OUT/host-status.txt" "exit=0" line
check "host check: every ELF segment case passed"   "$OUT/host-check.log"  "elf-segments host check: 0 of"

echo "elf-segments: program headers"
headers_status=0
python3 -B - "$CONFIG_H" "$PROGRAM" programs/*/build/*.elf > "$OUT/headers.log" 2>&1 << 'EOF' || headers_status=$?
import re
import struct
import sys

config = open(sys.argv[1]).read()


def define(name):
    m = re.search(r"^#define " + name + r" (\S+)$", config, re.M)
    return int(m.group(1), 0) if m else None


start = define("RORTHOX_PROGRAM_VIRTUAL_ADDRESS")
end = define("RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT")
bad = 0
if start is None or end is None:
    print("FAIL the build config has no ELF load window (RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT)")
    bad += 1
    end = end or 0x500000
    start = start or 0x400000


def loads(path):
    data = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", data, 28)
    phnum, = struct.unpack_from("<H", data, 44)
    out = []
    for i in range(phnum):
        p = struct.unpack_from("<8I", data, phoff + 32 * i)
        if p[0] == 1:
            out.append(p)
    return out


elftest = sys.argv[2]
for path in sorted(set(sys.argv[3:])):
    for (_t, off, vaddr, _pa, filesz, memsz, flags, _al) in loads(path):
        if memsz == 0:
            continue
        lo = vaddr & ~0xFFF
        hi = (vaddr + memsz + 0xFFF) & ~0xFFF
        if lo < start or hi > end:
            print("FAIL %s: load pages 0x%x-0x%x outside 0x%x-0x%x" % (path, lo, hi, start, end))
            bad += 1
    print("%s: loads inside 0x%x-0x%x" % (path, start, end))

segs = loads(elftest)
data_seg = [p for p in segs if p[6] & 2 and p[4] > 0]
bss_bytes = sum(p[5] - p[4] for p in segs if p[6] & 2)
odd_end = [p for p in segs if p[6] & 2 and (p[2] + p[5]) % 0x1000 != 0 and p[5] > p[4]]
for p in segs:
    print("elftest LOAD off 0x%x vaddr 0x%x filesz 0x%x memsz 0x%x flags %d" % (p[1], p[2], p[4], p[5], p[6]))
if not data_seg:
    print("FAIL elftest.elf has no writable PT_LOAD with file data (.data)")
    bad += 1
if bss_bytes < 3 * 0x1000:
    print("FAIL elftest.elf has only 0x%x bytes of .bss, expected several pages" % bss_bytes)
    bad += 1
if not odd_end:
    print("FAIL elftest.elf has no .bss that ends inside a page")
    bad += 1
print("headers: %d problems" % bad)
sys.exit(1 if bad else 0)
EOF
sed 's/^/  /' "$OUT/headers.log"
echo "exit=$headers_status" > "$OUT/headers-status.txt"
check "program headers: every program loads inside the window, elftest has .data and .bss" \
    "$OUT/headers-status.txt" "exit=0" line

# The heap table: one byte per block, 0 = free.
config_value() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h
}
table_address=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
heap_bytes=$(config_value RORTHOX_HEAP_SIZE_BYTES)
block_size=$(config_value RORTHOX_HEAP_BLOCK_SIZE)
if [ -z "$table_address" ] || [ -z "$heap_bytes" ] || [ -z "$block_size" ]; then
    echo "elf-segments: heap settings not found in kernel/config.h" >&2
    exit 1
fi
table_bytes=$((heap_bytes / block_size))
# Blocks each run keeps for good: the shell's two command nodes (the user library never frees them).
KEPT_PER_RUN=2

# free_blocks: print the number of free heap blocks, read twice with the guest stopped until two
# reads agree (print borrows a block for a moment).
free_blocks() {
    local i n last=
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/heap.bin"
        qemu_cmd "stop"
        qemu_cmd "pmemsave $table_address $table_bytes \"$OUT/heap.bin\""
        qemu_cmd "cont"
        wait_file "$OUT/heap.bin" "$table_bytes" || return 1
        n=$(LC_ALL=C tr -d '\001-\377' < "$OUT/heap.bin" | wc -c)
        if [ "$n" = "$last" ]; then
            echo "$n"
            return 0
        fi
        last=$n
        sleep 0.3
    done
    return 1
}

# wait_idle <runs> [<tag>]: wait until run number <runs> printed its result line and ended, and
# the shell printed its prompt again (one ">" per prompt below the shell banner).
wait_idle() {
    local k="$1" tag="${2:-}" deadline=$((SECONDS + RUN_TIMEOUT)) results ends prompts
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running"; return 1; }
        screen_snapshot
        results=1
        [ -z "$tag" ] || results=$(grep -cE "elftest $tag: (PASS|FAILED)" "$Q_OUT/screen.txt" 2> /dev/null || true)
        ends=$(grep -cF "Process was terminated" "$Q_OUT/screen.txt" 2> /dev/null || true)
        prompts=$(sed -n "/${RORTHOX_NAME} v${RORTHOX_VERSION}/,\$p" "$Q_OUT/screen.txt" | tr -cd '>' | wc -c)
        if [ "${results:-0}" -ge 1 ] && [ "${ends:-0}" -ge "$k" ] && [ "$prompts" -ge $((k + 1)) ]; then
            echo "  run $k ended and the shell is back after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for run $k to end after $RUN_TIMEOUT s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/elf-segments.bin"
mcopy -i "$OUT/elf-segments.bin" "$PROGRAM" ::

echo "elf-segments: run elftest.elf twice, counting free heap blocks"
: > "$OUT/counts.txt"
qemu_start "$OUT/elf-segments.bin" "$OUT"
counts=()
runs=0
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_idle 0 && n=$(free_blocks); then
    counts+=("$n")
    for tag in one two; do
        type_line "elftest.elf $tag"
        wait_idle $((runs + 1)) "$tag" && n=$(free_blocks) || break
        runs=$((runs + 1))
        counts+=("$n")
    done
    if [ "$runs" = 2 ]; then
        type_line "blank.elf after"
        wait_screen "Blank loaded" "$RUN_TIMEOUT" && wait_idle 3 || true
    fi
fi
qemu_stop || true

echo "  free blocks: ${counts[*]:-none} (each run may keep $KEPT_PER_RUN)"
for k in 1 2; do
    if [ "${#counts[@]}" -gt "$k" ]; then
        echo "run $k kept=$((counts[k - 1] - counts[k]))" >> "$OUT/counts.txt"
    fi
done
terminated=$(grep -cF "Process was terminated" "$OUT/screen.txt" 2> /dev/null || true)
echo "terminated=${terminated:-0}" >> "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                   "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"             "$OUT/screen.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "first run: .data, .bss and writes correct"    "$OUT/screen.txt"   "elftest one: PASS data bss write"
check "second run: fresh .data and zeroed .bss"      "$OUT/screen.txt"   "elftest two: PASS data bss write"
check "no elftest check failed"                      "$OUT/screen.txt"   ": FAIL" absent
check "first run kept $KEPT_PER_RUN blocks"          "$OUT/counts.txt"   "run 1 kept=$KEPT_PER_RUN" line
check "second run kept $KEPT_PER_RUN blocks"         "$OUT/counts.txt"   "run 2 kept=$KEPT_PER_RUN" line
check "blank.elf started afterwards"                 "$OUT/screen.txt"   "Blank loaded"
check "blank.elf got its argument"                   "$OUT/screen.txt"   "after" line
check "all three processes ended"                    "$OUT/counts.txt"   "terminated=3" line

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish elf-segments
