#!/usr/bin/env bash
#
# A kernel panic must stop the machine: boot an image without shell.elf, so the kernel
# panics with "Failed to load shell.elf", and check that the panic screen is shown and the
# CPU stays halted with interrupts off, also after an NMI. The screen (read from video memory) must show the reason
# and where PANIC was called (panic() in kernel/kernel.c passes its own file and line), next to the
# unchanged text of the panic screen, and no "Reason!" placeholder.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT=build/tests/panic
mkdir -p "$OUT"
rm -f "$OUT/state.txt"
cp bin/os.bin "$OUT/no-shell.bin"
mdel -i "$OUT/no-shell.bin" ::shell.elf

# panic() calls PANIC(msg, __FILE__, __LINE__): that line of kernel/kernel.c is the location shown.
panic_line=$(grep -n 'PANIC(msg, __FILE__, __LINE__);' kernel/kernel.c | cut -d: -f1)
[ "$(echo "$panic_line" | wc -w)" = 1 ] || { echo "panic: no single PANIC call in panic() in kernel/kernel.c" >&2; exit 1; }
echo "panic: expecting the location ./kernel/kernel.c:$panic_line"

echo "panic: boot an image without shell.elf"
qemu_start "$OUT/no-shell.bin" "$OUT"
wait_screen "A problem has been detected" 60 || true
sleep 1                               # give a PANIC that returns time to do something else
qemu_cmd "info registers"
screen_snapshot
# An NMI wakes the CPU from hlt even with interrupts off; the kernel handles it and PAUSE must
# halt again with interrupts off.
qemu_cmd "nmi"
sleep 1                               # give a broken halt loop time to run on
qemu_cmd "info registers"
screen_snapshot
qemu_stop || true

# "info registers" prints e.g. "EIP=00109b3e EFL=00000006 [-----P-] CPL=0 II=0 A20=1 SMM=0 HLT=1".
regs=$(grep -a -o 'EFL=[0-9a-f]* .*HLT=[01]' "$OUT/monitor.log" | head -1)
echo "  registers: ${regs:-none}"
eflags=$(echo "$regs" | sed -n 's/^EFL=\([0-9a-f]*\).*/\1/p')
echo "$regs" | grep -q 'HLT=1' && halted="halted" || halted=""
if [ -n "$eflags" ] && (( (16#$eflags & 0x200) == 0 )); then irq_off="interrupts off"; else irq_off=""; fi
nmi_regs=$(grep -a -o 'EIP=[0-9a-f]* EFL=[0-9a-f]* .*HLT=[01]' "$OUT/monitor.log" | sed -n 2p)
echo "  registers after the NMI: ${nmi_regs:-none}"
nmi_eip=$(echo "$nmi_regs" | sed -n 's/^EIP=\([0-9a-f]*\).*/\1/p')
nmi_eflags=$(echo "$nmi_regs" | sed -n 's/^EIP=[0-9a-f]* EFL=\([0-9a-f]*\).*/\1/p')
pause=$(awk '$2 == "PAUSE" { print $1 }' build/kernel.map | head -1)
{
    echo "$halted"
    echo "$irq_off"
    echo "panics=$(grep -a -c -F '[PANIC]' "$OUT/debugcon.log" || true)"
    echo "$nmi_regs" | grep -q 'HLT=1' && echo "halted after nmi"
    [ -n "$nmi_eflags" ] && (( (16#$nmi_eflags & 0x200) == 0 )) && echo "interrupts off after nmi"
    [ -n "$nmi_eip" ] && [ -n "$pause" ] && (( 16#$nmi_eip >= pause && 16#$nmi_eip < pause + 8 )) && echo "in PAUSE after nmi"
} > "$OUT/state.txt" || true

S="$OUT/screen.txt"
check "panic reported on debugcon"         "$OUT/debugcon.log" "[PANIC]: Failed to load shell.elf"
check "panic reported only once"           "$OUT/state.txt"    "panics=1" line
check "panic screen shown"                 "$S" "A problem has been detected"
check "CPU halted"                         "$OUT/state.txt"    "halted" line
check "interrupts off"                     "$OUT/state.txt"    "interrupts off" line
check "NMI: CPU halted again"              "$OUT/state.txt"    "halted after nmi" line
check "NMI: interrupts still off"          "$OUT/state.txt"    "interrupts off after nmi" line
check "NMI: back in PAUSE"                 "$OUT/state.txt"    "in PAUSE after nmi" line
check "screen: copyright row kept"         "$S" "All rights reserved | R C N" line
check "screen: advice kept"                "$S" "Disable BIOS memory options such as caching or shadowing." line
check "screen: issue URL kept"             "$S" "Finally, if all else failes, go to https://github.com/oriapp/RorthOX/issues" line
check "screen: reason shown"               "$S" "Reason: Failed to load shell.elf" line
check "screen: file and line shown"        "$S" "at ./kernel/kernel.c:$panic_line" line
check "screen: no placeholder"             "$S" "Reason!" absent
check "screen: final text kept"            "$S" "Error description" line
check "screen: final text kept (2)"        "$S" ".More Information" line
finish panic
