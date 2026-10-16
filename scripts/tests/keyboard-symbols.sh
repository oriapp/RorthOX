#!/usr/bin/env bash
# Check Shift symbols, the whole keypad, and Caps+Shift on the real PS/2 input path.
set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh
OUT="$(pwd)/build/tests/keyboard-symbols"
qemu_start bin/os.bin "$OUT"
wait_screen "$RORTHOX_NAME v$RORTHOX_VERSION" 20

press() {
    qemu_cmd "sendkey $1"
    sleep 0.1 # only space key presses; screen text acknowledges completion below
}
for key in 1 2 3 4 5 6 7 8 9 0 minus equal bracket_left bracket_right semicolon apostrophe grave_accent backslash comma dot slash; do
    press "shift-$key"
done
press spc
for key in kp_7 kp_8 kp_9 kp_subtract kp_4 kp_5 kp_6 kp_add kp_1 kp_2 kp_3 kp_0 kp_decimal; do
    press "$key"
done
press spc
press caps_lock
press a
press shift-a
press shift_r-b
press caps_lock
press c
press spc
press x
for key in up down left right; do press "$key"; done
press y
# Unshifted suffix acknowledges that the whole sequence reached the shell.
for key in spc e n d; do press "$key"; done
wait_screen ' end' 20
qemu_stop || true
symbols='!@#$%^&*()_+{}:"~|<>?'
expected="> $symbols 789-456+1230. Aabc xy end"
check "shifted symbol echo" "$OUT/screen.txt" "$symbols"
check "keypad echo (Num Lock is not tracked)" "$OUT/screen.txt" '789-456+1230.'
check "caps on: unshifted uppercase, both shifts lowercase; caps off again" "$OUT/screen.txt" ' Aabc xy end'
check "arrow keys insert nothing" "$OUT/screen.txt" ' xy end'
check "exact echoed line" "$OUT/screen.txt" "$expected" line
finish keyboard-symbols
