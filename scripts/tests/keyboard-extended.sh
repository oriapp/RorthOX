#!/usr/bin/env bash
#
# Special keys and Num Lock on the real input path: PS/2 IRQ, decoder, key queue, syscall 2 and
# the user library's getkeyblock, with the keys QEMU sends.
#
# Copy bin/os.bin, add programs/libtest to the copy only, boot, run "libtest.elf keys". The
# program reads raw key values and prints each group of them in hex when it reads a 'z'
# ("lt-k<n>: ..."), and ends after a 'q'. Groups:
#  1. up down left right home end insert delete pgup pgdn: the ten special values 0x100-0x109,
#     each once (a key release adds nothing).
#  2. the keypad with Num Lock on (the boot default): digits, '.', + - * /, Enter (CR).
#  3. num_lock, then the keypad again: digits and '.' give nothing, + - * / and Enter still
#     work, Shift+keypad digits give nothing either, and up/end/delete still give their values.
#  4. num_lock again: Shift+keypad gives the digits (Shift doesn't reverse Num Lock).
#  5. print, a, pause, keypad 6, ctrl-pause, b: Print Screen and Pause give nothing and change
#     nothing, the next letter and keypad digit come out as usual.
# bin/os.bin must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh

OUT=build/tests/keyboard-extended
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/*.txt "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "keyboard-extended: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

# press <key>...: QEMU key names, one after the other. The sleep only spaces the presses; the
# program's output on the screen says when they arrived.
press() {
    local key
    for key in "$@"; do
        qemu_cmd "sendkey $key"
        sleep 0.1
    done
}

# vga_dump <file>: the 4000-byte VGA page, read until two reads agree (nothing is still printing).
vga_dump() {
    local i
    rm -f "$1" "$OUT/dump-a.bin"
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/dump-b.bin"
        qemu_cmd "pmemsave 0xb8000 4000 \"$OUT/dump-b.bin\""
        wait_file "$OUT/dump-b.bin" 4000 || return 1
        if [ -f "$OUT/dump-a.bin" ] && cmp -s "$OUT/dump-a.bin" "$OUT/dump-b.bin"; then
            mv "$OUT/dump-b.bin" "$1"
            rm -f "$OUT/dump-a.bin"
            return 0
        fi
        mv "$OUT/dump-b.bin" "$OUT/dump-a.bin"
        sleep 0.3
    done
    return 1
}

IMG="$OUT/keyboard-extended.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::

: > "$OUT/steps.txt"
qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "libtest.elf keys"
    # Wait until the shell's prompt (printed while the program starts) is on the screen too.
    if wait_screen "libtest keys: armed" "$RUN_TIMEOUT" && vga_dump "$OUT/armed.bin"; then
        echo "armed" >> "$OUT/steps.txt"
        press up down left right home end insert delete pgup pgdn z
        wait_screen "lt-k1:" "$RUN_TIMEOUT" || true
        press kp_0 kp_1 kp_2 kp_3 kp_4 kp_5 kp_6 kp_7 kp_8 kp_9 kp_decimal
        press kp_add kp_subtract kp_multiply kp_divide kp_enter z
        wait_screen "lt-k2:" "$RUN_TIMEOUT" || true
        press num_lock kp_0 kp_1 kp_2 kp_3 kp_4 kp_5 kp_6 kp_7 kp_8 kp_9 kp_decimal
        press kp_add kp_subtract kp_multiply kp_divide kp_enter shift-kp_1 shift_r-kp_decimal up end delete z
        wait_screen "lt-k3:" "$RUN_TIMEOUT" || true
        press num_lock shift-kp_7 shift_r-kp_decimal kp_4 z
        wait_screen "lt-k4:" "$RUN_TIMEOUT" || true
        press print a pause kp_6 ctrl-pause b q
        if wait_screen "libtest keys: done" "$RUN_TIMEOUT"; then
            echo "done" >> "$OUT/steps.txt"
        fi
    fi
fi
qemu_stop || true

# The shell's "> " can land in front of a line the program prints: compare without it.
sed 's/^> //' "$OUT/screen.txt" > "$OUT/lines.txt" 2> /dev/null || : > "$OUT/lines.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "libtest keys armed"                          "$OUT/steps.txt"    "armed" line
check "libtest keys read every group"               "$OUT/steps.txt"    "done" line
check "the ten special keys, once each"             "$OUT/lines.txt" \
    "lt-k1: 100 101 102 103 104 105 106 107 108 109" line
check "keypad with Num Lock on at boot"             "$OUT/lines.txt" \
    "lt-k2: 30 31 32 33 34 35 36 37 38 39 2e 2b 2d 2a 2f d" line
check "Num Lock off: operators, Enter and special keys only" "$OUT/lines.txt" \
    "lt-k3: 2b 2d 2a 2f d 100 105 107" line
check "Num Lock on again: Shift+keypad gives digits" "$OUT/lines.txt" "lt-k4: 37 2e 34" line
check "Print Screen and Pause give nothing"         "$OUT/lines.txt"    "lt-k5: 61 36 62" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish keyboard-extended
