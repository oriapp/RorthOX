#!/usr/bin/env bash
#
# The kernel's .bss must be zero before any C code runs.
#
# The boot sector loads whole sectors, so today the bytes after kernel.bin come from the zeroed
# disk image and .bss only looks clean by luck. This test makes it dirty on purpose: QEMU starts
# stopped with its gdbstub on a Unix socket, scripts/tests/lib/kernel_bss.py breaks at _start
# (after the kernel is loaded), fills .bss with 0xA5, puts a canary byte just below and just above
# it and sets DF, then breaks at the first instruction of kernel_main and checks that every .bss
# byte is zero, both canaries and the loaded image are untouched, and DF/IF are clear on the boot
# stack. Then it detaches and the shell must come up.
#
# Two images, both copies of bin/os.bin:
#   kernel       the real kernel.bin
#   odd-tail     the same kernel objects linked with scripts/tests/lib/bss-tail.asm, a 4099-byte
#                .bss.* section placed last, so .bss ends on an odd address (catches a clear that
#                works in dwords, stops short or ignores .bss.* sections)
# bin/os.bin itself must not change.
#
# Needs python3, nasm and i686-elf-gcc (the cross compiler used by make).
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell after detaching)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/kernel-bss
rm -rf "$OUT"
mkdir -p "$OUT"
for tool in python3 nasm i686-elf-gcc; do
    command -v "$tool" > /dev/null || { echo "kernel-bss: $tool not found" >&2; exit 1; }
done
[ -f build/kernelfull.o ] || { echo "kernel-bss: build/kernelfull.o not found, run 'make all' first" >&2; exit 1; }

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
kernel_sectors=$(sed -n 's/^%define KERNEL_SECTORS \([0-9]*\)$/\1/p' build/config/rorthox-config.inc)
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)

# run_case <name> <image> <kernel.map> <kernel.bin> [<fixture symbol> <fixture size>]
run_case() {
    local name="$1" image="$2" map="$3" kernel="$4" status
    local dir="$OUT/$name" sock="$GDB_DIR/$1"
    shift 4
    echo "kernel-bss: $name"
    QEMU_FLAGS="-S -gdb unix:$sock,server=on,wait=off" qemu_start "$image" "$dir"
    if python3 -B scripts/tests/lib/kernel_bss.py "$sock" "$map" "$kernel" "$dir/rsp-result.txt" "$@" \
        > "$dir/rsp.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$dir/rsp.log"
    echo "exit=$status" > "$dir/rsp-status.txt"
    if [ "$status" -eq 0 ]; then
        wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" || true
    fi
    qemu_stop || true

    local r="$dir/rsp-result.txt"
    check "$name: debugger session finished without errors" "$dir/rsp-status.txt" "exit=0" line
    check "$name: stopped at _start after the kernel load"  "$r" "PASS at_start" line
    check "$name: paging off at _start"                     "$r" "PASS start_paging_off" line
    check "$name: interrupts off at _start"                 "$r" "PASS start_interrupts_off" line
    check "$name: kernel.bin is in memory at _start"        "$r" "PASS kernel_loaded" line
    check "$name: __bss_start/__bss_end match .bss"         "$r" "PASS bss_symbols" line
    check "$name: stopped at kernel_main"                   "$r" "PASS at_kernel_main" line
    check "$name: .bss all zero at kernel_main"             "$r" "PASS bss_zero" line
    check "$name: byte below .bss untouched"                "$r" "PASS canary_below" line
    check "$name: byte above .bss untouched"                "$r" "PASS canary_above" line
    check "$name: kernel image below .bss untouched"        "$r" "PASS image_unchanged" line
    check "$name: interrupts still off at kernel_main"      "$r" "PASS main_interrupts_off" line
    check "$name: DF clear at kernel_main"                  "$r" "PASS main_direction_clear" line
    check "$name: kernel_main on the boot stack"            "$r" "PASS main_boot_stack" line
    check "$name: kernel reached the end of init"           "$dir/debugcon.log" "Thats cool"
    check "$name: shell started after the debugger left"    "$dir/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
}

cp bin/os.bin "$OUT/kernel.bin.img"
run_case kernel "$OUT/kernel.bin.img" build/kernel.map bin/kernel.bin

# Same objects and linker script as bin/kernel.bin, plus the odd-sized fixture linked last.
FIX="$OUT/odd-tail"
mkdir -p "$FIX"
nasm -f elf32 scripts/tests/lib/bss-tail.asm -o "$FIX/bss-tail.o"
i686-elf-gcc -ffreestanding -O0 -nostdlib -Wl,-Map,"$FIX/kernel.map" -Wl,-L,build/config \
    -T kernel/linker.ld -o "$FIX/kernel.bin" build/kernelfull.o "$FIX/bss-tail.o"
size=$(wc -c < "$FIX/kernel.bin")
[ "$size" -le $((kernel_sectors * 512)) ] || { echo "kernel-bss: fixture kernel too big ($size)" >&2; exit 1; }
cp bin/os.bin "$FIX/odd-tail.img"
dd if=/dev/zero of="$FIX/odd-tail.img" bs=512 seek=1 count="$kernel_sectors" conv=notrunc status=none
dd if="$FIX/kernel.bin" of="$FIX/odd-tail.img" bs=512 seek=1 conv=notrunc status=none
run_case odd-tail "$FIX/odd-tail.img" "$FIX/kernel.map" "$FIX/kernel.bin" bss_test_tail 4099
check "odd-tail: fixture ends .bss on an odd address" "$FIX/rsp-result.txt" "PASS fixture_odd_end" line

rm -rf "$GDB_DIR"
hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
finish kernel-bss
