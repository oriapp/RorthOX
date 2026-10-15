#!/usr/bin/env bash
#
# idt_load must follow cdecl: keep EBX, ESI, EDI and EBP, return with the stack balanced, and load
# the IDTR from the descriptor it is given. The IDT covers the 256 x86 vectors: limit 0x7ff.
#
# QEMU starts stopped with its gdbstub on a Unix socket. scripts/tests/lib/idt_load.py breaks at
# idt_load when kernel_main calls it, puts sentinels in the four callee-saved registers, runs the
# real function to its return address and checks the registers, ESP and the IDTR (read back with
# the monitor's "info registers"). It then restores the caller's registers and detaches, and the
# shell must come up. The image is a copy of bin/os.bin, which must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell after detaching)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/idt-load
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "idt-load: python3 not found" >&2; exit 1; }

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/idt-load.bin"
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)

echo "idt-load: boot"
QEMU_FLAGS="-S -gdb unix:$GDB_DIR/gdb,server=on,wait=off" qemu_start "$OUT/idt-load.bin" "$OUT"
if python3 -B scripts/tests/lib/idt_load.py "$GDB_DIR/gdb" build/kernel.map "$OUT/rsp-result.txt" \
    > "$OUT/rsp.log" 2>&1; then status=0; else status=$?; fi
sed 's/^/  /' "$OUT/rsp.log"
echo "exit=$status" > "$OUT/rsp-status.txt"
if [ "$status" -eq 0 ]; then
    wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" || true
fi
qemu_stop || true
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

r="$OUT/rsp-result.txt"
check "debugger session finished without errors"      "$OUT/rsp-status.txt" "exit=0" line
check "stopped at idt_load called from the kernel"    "$r" "PASS at_idt_load" line
check "argument is the kernel's idtr descriptor"      "$r" "PASS argument_is_idtr" line
check "IDTR not yet loaded before the call"           "$r" "PASS idtr_differs_before" line
check "idt_load returned to its caller"               "$r" "PASS returned" line
check "EBX preserved"                                 "$r" "PASS ebx_preserved" line
check "ESI preserved"                                 "$r" "PASS esi_preserved" line
check "EDI preserved"                                 "$r" "PASS edi_preserved" line
check "EBP preserved"                                 "$r" "PASS ebp_preserved" line
check "stack balanced (ESP past the return address)"  "$r" "PASS stack_balanced" line
check "IDTR holds the descriptor's base and limit"    "$r" "PASS idtr_loaded" line
check "IDTR limit covers the 256 vectors (0x7ff)"     "$r" "PASS idtr_limit_256_vectors" line
check "kernel reached the end of init (debugcon)"     "$OUT/debugcon.log" "Thats cool"
check "shell started after the debugger left"         "$OUT/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "bin/os.bin not changed by the test"            "$OUT/image.txt" "unchanged" line
finish idt-load
