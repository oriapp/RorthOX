#!/usr/bin/env bash
#
# Headless smoke test for RorthOX.
#
# Boots bin/os.bin in QEMU without a display, types a command into the shell through
# the QEMU monitor ("sendkey"), then checks:
#   - the debugcon log (the kernel writes to I/O port 0xE9), and
#   - the VGA text screen, read straight from guest memory at 0xB8000 ("pmemsave").
#
# Usage:  ./scripts/smoke.sh            (or: make smoke)
# Env:    QEMU=qemu-system-i386   QEMU_FLAGS="extra flags"   OUT=build/smoke
#         BOOT_WAIT=3 (seconds before typing)
#
# Results land in $OUT: debugcon.log, screen.txt (80x25 text), screen.ppm, monitor.log.
# Needs only qemu-system-i386 and the base system tools (timeout, od, awk, grep, sed).

set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${IMAGE:-bin/os.bin}"
OUT="${OUT:-build/smoke}"
QEMU="${QEMU:-qemu-system-i386}"
BOOT_WAIT="${BOOT_WAIT:-3}"
COMMAND="blank.elf one two"

[ -f "$IMAGE" ] || { echo "smoke: $IMAGE not found, run 'make all' first" >&2; exit 2; }
mkdir -p "$OUT"
rm -f "$OUT/debugcon.log" "$OUT/screen.txt" "$OUT/screen.ppm" "$OUT/vga.bin" "$OUT/monitor.log"

# Print QEMU monitor "sendkey" commands that type $1.
type_keys() {
    local s="$1" i c key
    for ((i = 0; i < ${#s}; i++)); do
        c="${s:i:1}"
        case "$c" in
            ' ') key=spc ;;
            '.') key=dot ;;
            *)   key="$c" ;;
        esac
        echo "sendkey $key"
        sleep 0.1
    done
}

# Drive QEMU through its monitor on stdin.
{
    sleep "$BOOT_WAIT"
    type_keys "$COMMAND"
    echo "sendkey ret"
    sleep 2
    # Quote file names: the monitor would parse "4000 /tmp/x" as the expression 4000/tmp.
    echo "pmemsave 0xb8000 4000 \"$OUT/vga.bin\""
    echo "screendump \"$OUT/screen.ppm\""
    sleep 1
    echo "quit"
} | timeout 60 "$QEMU" -m 128 -display none -no-reboot \
        -drive "file=$IMAGE,format=raw,index=0,media=disk" \
        -debugcon "file:$OUT/debugcon.log" -serial none -monitor stdio \
        ${QEMU_FLAGS:-} > "$OUT/monitor.log" 2>&1 || {
    echo "smoke: QEMU failed or timed out (exit $?), see $OUT/monitor.log" >&2
    exit 1
}

# VGA text memory is (character, attribute) pairs, 80 columns x 25 rows. Keep the characters,
# show anything that isn't printable ASCII as a space, and strip trailing spaces.
[ -f "$OUT/vga.bin" ] || { echo "smoke: no screen dump, see $OUT/monitor.log" >&2; exit 1; }
od -An -v -tu1 -w2 "$OUT/vga.bin" |
    awk '{ printf "%c", ($1 >= 32 && $1 < 127) ? $1 : 32 } NR % 80 == 0 { printf "\n" }' |
    sed 's/ *$//' > "$OUT/screen.txt"

failures=0
check() {   # check <description> <file> <fixed string> [present|absent|line]
    local what="$1" file="$2" text="$3" mode="${4:-present}" opts=-qF
    [ "$mode" = line ] && { opts=-qxF; mode=present; }   # "line": the whole line must match
    if grep $opts -- "$text" "$file"; then found=1; else found=0; fi
    if { [ "$mode" = present ] && [ $found = 1 ]; } || { [ "$mode" = absent ] && [ $found = 0 ]; }; then
        echo "  ok   $what"
    else
        echo "  FAIL $what"
        failures=$((failures + 1))
    fi
}

echo "smoke: checks"
check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                   "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"             "$OUT/screen.txt"   "Rorth v1.0.0"
check "typed command echoed by the shell"            "$OUT/screen.txt"   "> $COMMAND"
check "blank.elf was loaded from FAT16 and ran"      "$OUT/screen.txt"   "Blank loaded"
check "argv passed to the program (own line)"       "$OUT/screen.txt"   "two" line

echo "smoke: screen"
sed 's/^/  | /' "$OUT/screen.txt"

if [ "$failures" -ne 0 ]; then
    echo "smoke: $failures check(s) failed" >&2
    exit 1
fi
echo "smoke: all checks passed"
