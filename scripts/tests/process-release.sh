#!/usr/bin/env bash
#
# A program that ends must give back the kernel heap it used, its process object included.
# Boot a copy of bin/os.bin, count the free kernel heap blocks while the shell is idle, run
# "blank.elf tag1", count again, run "blank.elf tag2" and count once more. Each launch may keep
# only the blocks listed at KEPT_PER_LAUNCH. The free blocks are counted in the kernel's heap
# table, read through the QEMU monitor while the guest is stopped. The shell is idle when
# scripts/tests/lib/guest-state.sh has stopped the guest at its key request with the shell alone
# in the task list (blank.elf has ended) and the same shell owning the keyboard.
# bin/os.bin itself must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each launch)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/process-release
mkdir -p "$OUT"
rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT"/shell.txt "$OUT"/screen-*.txt

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

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone
# with the keyboard; results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$RUN_TIMEOUT" owner=first "$@"
}

echo "process-release: kernel symbols"
guest_state_setup "$OUT"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/process-release.bin"
: > "$OUT/counts.txt"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "process-release: count free heap blocks around two launches of blank.elf"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/process-release.bin" "$OUT"
counts=()
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
    state boot save="$OUT/shell.txt" screen="$OUT/screen-boot.txt" && n=$(free_blocks); then
    counts+=("$n")
    for k in 1 2; do
        type_line "blank.elf tag$k"
        # blank.elf's argument row (the echoed command doesn't match), then the shell alone again.
        wait_screen_lines "$RUN_TIMEOUT" "prompt:tag$k" &&
            state "tag$k" shell="$OUT/shell.txt" screen="$OUT/screen-tag$k.txt" && n=$(free_blocks) || break
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
# Each launch was seen gone at a rendezvous (stop_checks below: the shell alone in the task list), and
# the screen saved there (nothing is printing or scrolling) has the exit row below its argument row.
for s in boot tag1 tag2; do [ -f "$OUT/screen-$s.txt" ] || : > "$OUT/screen-$s.txt"; done
# exit_below <screen> <ERE>: 1 if a "Process was terminated" row (the shell's prompt may be in front)
# is below the last row matching <ERE>.
exit_below() {
    awk -v re="^($2)\$" '$0 ~ re { r = NR; f = 0 } r && $0 ~ /^(> )?Process was terminated$/ { f = 1 } END { print f + 0 }' "$1"
}
echo "terminated=$(($(exit_below "$OUT/screen-tag1.txt" '(> )?tag1') + $(exit_below "$OUT/screen-tag2.txt" '(> )?tag2')))" >> "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen-boot.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "kernel symbols for the rendezvous found"     "$OUT/symbols.txt"  "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"          "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                        "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"           "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot "boot"
stop_checks tag1 "launch 1 ended"
stop_checks tag2 "launch 2 ended"
check "an exit row below each launch's argument"    "$OUT/counts.txt"   "terminated=2" line
check "launch 1 kept $KEPT_PER_LAUNCH blocks"       "$OUT/counts.txt"   "launch 1 kept=$KEPT_PER_LAUNCH" line
check "launch 2 kept $KEPT_PER_LAUNCH blocks"       "$OUT/counts.txt"   "launch 2 kept=$KEPT_PER_LAUNCH" line

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish process-release
