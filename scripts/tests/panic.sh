#!/usr/bin/env bash
#
# A kernel panic must stop the machine: boot an image without shell.elf, so the kernel
# panics with "Failed to load shell.elf", and check that the panic screen is shown and the
# CPU stays halted with interrupts off.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT=build/tests/panic
mkdir -p "$OUT"
cp bin/os.bin "$OUT/no-shell.bin"
mdel -i "$OUT/no-shell.bin" ::shell.elf

echo "panic: boot an image without shell.elf"
qemu_start "$OUT/no-shell.bin" "$OUT"
wait_screen "A problem has been detected" 60 || true
sleep 1                               # give a PANIC that returns time to do something else
qemu_cmd "info registers"
screen_snapshot
qemu_stop || true

# "info registers" prints e.g. "EFL=00000006 [-----P-] CPL=0 II=0 A20=1 SMM=0 HLT=1".
regs=$(grep -a -o 'EFL=[0-9a-f]* .*HLT=[01]' "$OUT/monitor.log" | tail -1)
echo "  registers: ${regs:-none}"
eflags=$(echo "$regs" | sed -n 's/^EFL=\([0-9a-f]*\).*/\1/p')
echo "$regs" | grep -q 'HLT=1' && halted="halted" || halted=""
if [ -n "$eflags" ] && (( (16#$eflags & 0x200) == 0 )); then irq_off="interrupts off"; else irq_off=""; fi
{
    echo "$halted"
    echo "$irq_off"
    echo "panics=$(grep -a -c -F '[PANIC]' "$OUT/debugcon.log" || true)"
} > "$OUT/state.txt"

check "panic reported on debugcon"         "$OUT/debugcon.log" "[PANIC]: Failed to load shell.elf"
check "panic reported only once"           "$OUT/state.txt"    "panics=1" line
check "panic screen shown"                 "$OUT/screen.txt"   "A problem has been detected"
check "CPU halted"                         "$OUT/state.txt"    "halted" line
check "interrupts off"                     "$OUT/state.txt"    "interrupts off" line
finish panic
