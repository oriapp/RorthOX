#!/usr/bin/env bash
#
# Both 8259 PICs must be remapped and masked: boot, run blank.elf (loading it does ATA PIO
# reads, which raise IRQ14 on the slave), then ask QEMU for the PIC state with "info pic".
# Master: vectors 0x20-0x27, only IRQ0-2 open, nothing in service.
# Slave:  vectors 0x28-0x2F, all masked, nothing in service.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT=build/tests/pic
mkdir -p "$OUT"

echo "pic: boot bin/os.bin and run blank.elf"
qemu_start bin/os.bin "$OUT"
if wait_screen "RorthOX v1.0.0" 60; then
    type_line "blank.elf"
    wait_screen "Process was terminated" 30 || true
fi
qemu_cmd "info pic"
qemu_stop || true

# "info pic" prints e.g. "pic0: irr=00 imr=f8 isr=00 hprio=0 irq_base=20 rr_sel=0 elcr=00 fnm=0".
# The monitor log has terminal escapes and carriage returns in it, so strip those first.
sed -e 's/\x1b\[[0-9;?]*[A-Za-z]//g' -e 's/\r//g' "$OUT/monitor.log" > "$OUT/monitor.txt"
: > "$OUT/state.txt"
for chip in pic0 pic1; do
    line=$(grep -a -o "^$chip: irr=.*" "$OUT/monitor.txt" | tail -1 || true)
    echo "  ${line:-$chip: none}"
    for field in irq_base imr isr; do
        value=$(echo "$line" | sed -n "s/.* $field=\([0-9a-f]*\).*/\1/p")
        echo "$chip $field=$value" >> "$OUT/state.txt"
    done
done

check "blank.elf ran (the kernel reached user mode)" "$OUT/screen.txt" "Process was terminated"
check "master PIC at vector 0x20"            "$OUT/state.txt" "pic0 irq_base=20" line
check "master PIC masks all but IRQ0-2"      "$OUT/state.txt" "pic0 imr=f8" line
check "master PIC has nothing in service"    "$OUT/state.txt" "pic0 isr=00" line
check "slave PIC at vector 0x28"             "$OUT/state.txt" "pic1 irq_base=28" line
check "slave PIC masks everything"           "$OUT/state.txt" "pic1 imr=ff" line
check "slave PIC has nothing in service"     "$OUT/state.txt" "pic1 isr=00" line
finish pic
