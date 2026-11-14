#!/usr/bin/env bash
#
# The machine state the boot sector reads the kernel with. After the far jump to load32 only CS
# holds a protected-mode descriptor; DS, ES, FS, GS and SS keep their real-mode caches (64 KiB limit)
# until they are loaded again, and rep insw writes ES:EDI at 1 MiB. A real CPU (and KVM) raises
# #GP(0) for that, and with no IDT the machine resets. TCG never checks the limit, so every other
# test boots fine either way: this one reads the segment caches through the gdbstub instead
# (scripts/tests/lib/boot_segments.py).
#
# Two boots, each stopped at load32, at the first call after it and at the first rep insw (the
# addresses come from a NASM listing of kernel/boot/boot.asm, which must assemble to bin/boot.bin):
# 1. bios: from the state SeaBIOS leaves.
# 2. dirty: at load32 the debugger sets DF, puts garbage in the upper half of ESP and turns the A20
#    gate off, as another BIOS may leave them.
# Both times DS/ES/FS/GS/SS must be the flat data segment (selector, base 0, limit 0xffffffff,
# writable data) at the call and at rep insw, ESP the 32-bit boot stack at the call, DF 0 and A20 on
# at rep insw. Then the debugger leaves and the shell must start.
#
# TCG only: under KVM the CPU checks the limits itself (the old code didn't boot there at all), and
# KVM's segment caches and A20 line don't read the same through the monitor.
#
# Needs python3 and nasm. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh
qemu_require_tcg

OUT=${RORTHOX_TEST_ROOT:-build/tests}/boot-segments
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "boot-segments: python3 not found" >&2; exit 1; }
CONFIG_INC=build/config/rorthox-config.inc
[ -f bin/boot.bin ] && [ -f "$CONFIG_INC" ] || { echo "boot-segments: run 'make all' first" >&2; exit 1; }

nasm -f bin -Ibuild/config/ kernel/boot/boot.asm -o "$OUT/boot.bin" -l "$OUT/boot.lst"
if cmp -s "$OUT/boot.bin" bin/boot.bin; then echo same > "$OUT/listing.txt"; else echo differs > "$OUT/listing.txt"; fi
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)

GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)

for mode in bios dirty; do
    echo "boot-segments: $mode boot"
    cp bin/os.bin "$OUT/$mode.bin"
    rm -f "$GDB_DIR/gdb"
    QEMU_FLAGS="-S -gdb unix:$GDB_DIR/gdb,server=on,wait=off" qemu_start "$OUT/$mode.bin" "$OUT/$mode"
    if python3 -B scripts/tests/lib/boot_segments.py "$GDB_DIR/gdb" "$OUT/boot.lst" bin/boot.bin "$CONFIG_INC" \
        "$mode" "$OUT/$mode/rsp-result.txt" > "$OUT/$mode/rsp.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$OUT/$mode/rsp.log"
    echo "exit=$status" > "$OUT/$mode/rsp-status.txt"
    wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" || true
    qemu_stop || true
done
rm -rf "$GDB_DIR"
[ "$hash_before" = "$(image_hash)" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

check "kernel/boot/boot.asm assembles to bin/boot.bin (listing addresses hold)" "$OUT/listing.txt" "same" line
for mode in bios dirty; do
    o="$OUT/$mode" r="$OUT/$mode/rsp-result.txt"
    check "$mode: debugger session finished without errors"     "$o/rsp-status.txt" "exit=0" line
    check "$mode: stopped at load32 in the boot sector, CS = code selector" "$r" "PASS load32" line
    [ "$mode" = bios ] || check "$mode: at load32 DF=1, ESP upper half set, A20 off" "$r" "PASS dirty_state" line
    for at in call insw; do
        check "$mode: reached the first $at"                    "$r" "PASS ${at}_reached" line
        for s in ds es fs gs ss; do
            check "$mode: at $at ${s^^} is the flat writable data segment" "$r" "PASS ${at}_${s}_flat" line
        done
    done
    check "$mode: ESP is the 32-bit boot stack 0x7c00 at the call" "$r" "PASS call_esp" line
    check "$mode: DF=0 at rep insw"                             "$r" "PASS insw_df" line
    check "$mode: A20 on at rep insw"                           "$r" "PASS insw_a20" line
    check "$mode: rep insw reads 256 words from 0x1f0 to the load address" "$r" "PASS insw_operands" line
    check "$mode: kernel reached the end of init (debugcon)"    "$o/debugcon.log" "Thats cool"
    check "$mode: shell started after the debugger left"        "$o/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
done
check "bin/os.bin not changed by the test"                      "$OUT/image.txt" "unchanged" line
finish boot-segments
