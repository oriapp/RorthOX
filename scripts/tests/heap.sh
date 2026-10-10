#!/usr/bin/env bash
#
# The kernel heap must refuse a request bigger than any free run of blocks. Copy bin/os.bin,
# add programs/heaptest to the copy only, boot it and run heaptest.elf: it asks for 96 MiB of
# the 100 MiB heap (more than is free after boot) and expects NULL, then checks that a small
# allocation still works. bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for heaptest)
#      EXIT_TIMEOUT=15 (for the process to end after PASS)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=build/tests/heap
PROGRAM=programs/heaptest/build/heaptest.elf
SHELL_ELF=programs/shell/build/shell.elf
mkdir -p "$OUT"

# Both ELF files must fit in one FAT cluster (64 KiB), and the 96 MiB request is sized for 128 MiB of RAM.
[ -f "$PROGRAM" ] || { echo "heap: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
for elf in "$SHELL_ELF" "$PROGRAM"; do
    size=$(wc -c < "$elf")
    echo "  $elf: $size bytes"
    [ "$size" -le 65536 ] || { echo "heap: $elf is bigger than 64 KiB" >&2; exit 1; }
done
[ "$QEMU_RAM_MIB" -ge 128 ] || { echo "heap: needs QEMU_RAM_MIB >= 128 (is $QEMU_RAM_MIB)" >&2; exit 1; }

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/heap.bin"
mcopy -i "$OUT/heap.bin" "$PROGRAM" ::

echo "heap: boot a copy of bin/os.bin with heaptest.elf and run it"
qemu_start "$OUT/heap.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "heaptest.elf"
    if wait_screen "heaptest: done" "$RUN_TIMEOUT"; then
        if grep -qF "heaptest: PASS" "$OUT/screen.txt"; then
            wait_screen "Process was terminated" "$EXIT_TIMEOUT" || true
        fi
    else
        echo "  heaptest did not complete; possible allocation-path corruption or hang"
        failures=$((failures + 1))
    fi
fi
qemu_stop || true

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "typed command echoed by the shell"           "$OUT/screen.txt"   "> heaptest.elf"
check "heaptest made the big request"               "$OUT/screen.txt"   "heaptest: requesting more than is free"
check "big request refused"                         "$OUT/screen.txt"   "heaptest: refused"
check "small allocation works after the refusal"    "$OUT/screen.txt"   "heaptest: small allocation usable"
check "heaptest passed"                             "$OUT/screen.txt"   "heaptest: PASS"
check "heaptest finished"                           "$OUT/screen.txt"   "heaptest: done"
check "heaptest reported no failure"                "$OUT/screen.txt"   "heaptest: FAIL" absent
check "heaptest process ended"                      "$OUT/screen.txt"   "Process was terminated"
check "bin/os.bin not changed by the test"          "$OUT/image.txt"    "unchanged" line
finish heap
