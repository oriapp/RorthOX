#!/usr/bin/env bash
#
# Both 8259 PICs must be remapped and masked: boot, run blank.elf (loading it does ATA PIO
# reads, which raise IRQ14 on the slave), then ask QEMU for the PIC state with "info pic".
# Master: only IRQ0-2 open, nothing in service.
# Slave: all masked, nothing in service.
#
# "info pic" is read with the guest stopped at the shell's key request (the guest-state rendezvous,
# scripts/tests/lib/guest-state.sh), once after boot and once after blank.elf ended: a syscall made
# from ring 3 with interrupts off, so no interrupt handler is halfway through and every EOI the
# kernel sends is out. Read while the guest runs, a timer tick the CPU has taken but not
# acknowledged yet shows as isr=01 (with KVM the CPU runs on while the monitor reads).
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for blank.elf)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/pic
mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.txt.log
[ -f bin/os.bin ] || { echo "pic: run 'make all' first" >&2; exit 1; }

guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# pic_at <tag>: the rendezvous; "info pic" there goes to $OUT/pic-<tag>.txt, the checks to
# $OUT/state-<tag>.txt.
pic_at() {
    guest_state "$SOCK" "$OUT/state-$1.txt" "$1" timeout="$RUN_TIMEOUT" monitor="$OUT/pic-$1.txt,info pic"
}

echo "pic: boot a copy of bin/os.bin and run blank.elf"
cp bin/os.bin "$OUT/pic.bin"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/pic.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    pic_at boot || true
    type_line "blank.elf"
    if wait_screen "Process was terminated" "$RUN_TIMEOUT"; then
        pic_at blank || true
    fi
fi
qemu_stop || true

# "info pic" prints e.g. "pic0: irr=00 imr=f8 isr=00 hprio=0 irq_base=20 rr_sel=0 elcr=00 fnm=0".
# fields-<tag>.txt gets "<chip> <field>=<value>" lines.
for tag in boot blank; do
    [ -f "$OUT/pic-$tag.txt" ] || : > "$OUT/pic-$tag.txt"
    [ -f "$OUT/state-$tag.txt" ] || : > "$OUT/state-$tag.txt"
    : > "$OUT/fields-$tag.txt"
    for chip in pic0 pic1; do
        line=$(tr -d '\r' < "$OUT/pic-$tag.txt" | grep -a -o "^$chip: irr=.*" | tail -1 || true)
        echo "  $tag: ${line:-$chip: none}"
        for field in irq_base imr isr; do
            value=$(echo "$line" | sed -n "s/.* $field=\([0-9a-f]*\).*/\1/p")
            echo "$chip $field=$value" >> "$OUT/fields-$tag.txt"
        done
    done
done

B="$OUT/fields-boot.txt"
S="$OUT/fields-blank.txt"
check "kernel symbols for the rendezvous found"      "$OUT/symbols.txt" "found" line
check "boot: PIC read at the shell's key request"   "$OUT/state-boot.txt" "PASS boot_rendezvous" line
check "boot: master PIC has nothing in service"     "$B" "pic0 isr=00" line
check "boot: slave PIC has nothing in service"      "$B" "pic1 isr=00" line
check "blank.elf ran (the kernel reached user mode)" "$OUT/screen.txt" "Process was terminated"
check "PIC read at the shell's key request"         "$OUT/state-blank.txt" "PASS blank_rendezvous" line
check "blank.elf had ended: the shell alone"         "$OUT/state-blank.txt" "PASS blank_task_count" line
printf -v master_base '%x' "$PIC_MASTER_VECTOR_BASE"
printf -v slave_base '%x' "$PIC_SLAVE_VECTOR_BASE"
check "master PIC at configured vector"       "$S" "pic0 irq_base=$master_base" line
check "master PIC masks all but IRQ0-2"      "$S" "pic0 imr=f8" line
check "master PIC has nothing in service"    "$S" "pic0 isr=00" line
check "slave PIC at configured vector"        "$S" "pic1 irq_base=$slave_base" line
check "slave PIC masks everything"           "$S" "pic1 imr=ff" line
check "slave PIC has nothing in service"     "$S" "pic1 isr=00" line
finish pic
