#!/usr/bin/env bash
#
# A program that ends must give back the kernel heap it used, its process object included.
# Boot a copy of bin/os.bin, count the free kernel heap blocks while the shell is idle, run
# "blank.elf tag1", count again, run "blank.elf tag2" and count once more. Each launch may keep
# only the blocks listed at KEPT_PER_LAUNCH. The free blocks are counted in the kernel's heap
# table, read through the QEMU monitor while the guest is stopped.
# bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each launch)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=build/tests/process-release
mkdir -p "$OUT"

# Blocks each launch keeps for good: the shell's two command nodes for "blank.elf tagN" (the
# user library never frees them).
KEPT_PER_LAUNCH=2

# The heap table: one byte per block, 0 = free.
config_value() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h
}
table_address=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
heap_bytes=$(config_value RORTHOX_HEAP_SIZE_BYTES)
block_size=$(config_value RORTHOX_HEAP_BLOCK_SIZE)
if [ -z "$table_address" ] || [ -z "$heap_bytes" ] || [ -z "$block_size" ]; then
    echo "process-release: heap settings not found in kernel/config.h" >&2
    exit 1
fi
table_bytes=$((heap_bytes / block_size))

# free_blocks: print the number of free heap blocks. Reads the table twice with the guest stopped
# and retries until both reads agree, so a block that print borrows for a moment is not counted.
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

# wait_idle <launches>: wait until launch number <launches> (0: none yet) has printed its tag and
# ended, and the shell has printed its prompt again (one ">" per prompt below the shell banner;
# the kernel's date line above it has one too).
wait_idle() {
    local k="$1" deadline=$((SECONDS + RUN_TIMEOUT)) tags ends prompts
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running"; return 1; }
        screen_snapshot
        tags=2
        [ "$k" = 0 ] || tags=$(grep -cF "tag$k" "$Q_OUT/screen.txt" 2> /dev/null || true)
        ends=$(grep -cF "Process was terminated" "$Q_OUT/screen.txt" 2> /dev/null || true)
        prompts=$(sed -n "/${RORTHOX_NAME} v${RORTHOX_VERSION}/,\$p" "$Q_OUT/screen.txt" | tr -cd '>' | wc -c)
        # "tagN" shows up twice: in the typed command and in blank.elf's argument list.
        if [ "${tags:-0}" -ge 2 ] && [ "${ends:-0}" -ge "$k" ] && [ "$prompts" -ge $((k + 1)) ]; then
            echo "  launch $k ended and the shell is back after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for launch $k to end after $RUN_TIMEOUT s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/process-release.bin"
: > "$OUT/counts.txt"

echo "process-release: count free heap blocks around two launches of blank.elf"
qemu_start "$OUT/process-release.bin" "$OUT"
counts=()
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_idle 0 &&
    n=$(free_blocks); then
    counts+=("$n")
    for k in 1 2; do
        type_line "blank.elf tag$k"
        wait_idle "$k" && n=$(free_blocks) || break
        counts+=("$n")
    done
fi
qemu_stop || true

echo "  free blocks: ${counts[*]:-none} (each launch may keep $KEPT_PER_LAUNCH)"
for k in 1 2; do
    if [ "${#counts[@]}" -gt "$k" ]; then
        echo "launch $k kept=$((counts[k - 1] - counts[k]))" >> "$OUT/counts.txt"
    fi
done
terminated=$(grep -cF "Process was terminated" "$OUT/screen.txt" 2> /dev/null || true)
echo "terminated=${terminated:-0}" >> "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "blank.elf ran twice"                         "$OUT/counts.txt"   "terminated=2" line
check "launch 1 kept $KEPT_PER_LAUNCH blocks"       "$OUT/counts.txt"   "launch 1 kept=$KEPT_PER_LAUNCH" line
check "launch 2 kept $KEPT_PER_LAUNCH blocks"       "$OUT/counts.txt"   "launch 2 kept=$KEPT_PER_LAUNCH" line

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish process-release
