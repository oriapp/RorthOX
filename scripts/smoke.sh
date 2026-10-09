#!/usr/bin/env bash
#
# Headless smoke test for RorthOX.
#
# Boots bin/os.bin in QEMU without a display, waits for the shell, types a command into it
# through the QEMU monitor ("sendkey"), waits for the program to finish, then checks:
#   - the debugcon log (the kernel writes to I/O port 0xE9), and
#   - the VGA text screen, read straight from guest memory at 0xB8000 ("pmemsave").
# It never relies on fixed delays: it polls the screen until the expected text shows up,
# so it also works on a slow machine (QEMU without KVM on a CI runner).
#
# Usage:  ./scripts/smoke.sh            (or: make smoke)
# Env:    QEMU=qemu-system-i386   QEMU_FLAGS="extra flags"   OUT=build/smoke
#         BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for the program)
#
# Results land in $OUT: debugcon.log, screen.txt (80x25 text), screen.ppm, monitor.log.
# Needs only qemu-system-i386 and the base system tools (timeout, od, awk, grep, sed).

set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${IMAGE:-bin/os.bin}"
OUT="${OUT:-build/smoke}"
QEMU="${QEMU:-qemu-system-i386}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
COMMAND="blank.elf one two"
SHELL_READY="Rorth v1.0.0"              # the shell's banner: the shell is running
PROGRAM_DONE="Process was terminated"   # printed by the kernel when a program exits

[ -f "$IMAGE" ] || { echo "smoke: $IMAGE not found, run 'make all' first" >&2; exit 2; }
mkdir -p "$OUT"
rm -f "$OUT/debugcon.log" "$OUT/screen.txt" "$OUT/screen.ppm" "$OUT/vga.bin" "$OUT/monitor.log"

# VGA text memory is (character, attribute) pairs, 80 columns x 25 rows. Keep the characters,
# show anything that isn't printable ASCII as a space, and strip trailing spaces.
vga_to_text() {
    od -An -v -tu1 -w2 "$1" |
        awk '{ printf "%c", ($1 >= 32 && $1 < 127) ? $1 : 32 } NR % 80 == 0 { printf "\n" }' |
        sed 's/ *$//'
}

# The functions below run in the monitor driver: their standard output goes to the QEMU monitor.

# Save the VGA text buffer to $OUT/vga.bin and decode it to $OUT/screen.txt.
snapshot() {
    local i
    rm -f "$OUT/vga.bin"
    # Quote file names: the monitor would parse "4000 /tmp/x" as the expression 4000/tmp.
    echo "pmemsave 0xb8000 4000 \"$OUT/vga.bin\""
    for ((i = 0; i < 100; i++)); do
        [ -f "$OUT/vga.bin" ] && [ "$(wc -c < "$OUT/vga.bin")" -eq 4000 ] && break
        sleep 0.1
    done
    [ -f "$OUT/vga.bin" ] && vga_to_text "$OUT/vga.bin" > "$OUT/screen.txt"
    return 0
}

# wait_screen <text> <seconds>: take screen snapshots until <text> shows up.
wait_screen() {
    local text="$1" deadline=$((SECONDS + $2))
    while ((SECONDS < deadline)); do
        snapshot
        if grep -qF -- "$text" "$OUT/screen.txt" 2>/dev/null; then
            echo "smoke: \"$text\" on screen after ${SECONDS} s" >&2
            return 0
        fi
        sleep 0.5
    done
    echo "smoke: gave up waiting for \"$text\" after $2 s" >&2
    return 1
}

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

# Drive QEMU through its monitor on stdin. If a wait times out, carry on: the checks below report it.
{
    if wait_screen "$SHELL_READY" "$BOOT_TIMEOUT"; then
        type_keys "$COMMAND"
        echo "sendkey ret"
        wait_screen "$PROGRAM_DONE" "$RUN_TIMEOUT" || true
    fi
    snapshot
    echo "screendump \"$OUT/screen.ppm\""
    sleep 1
    echo "quit"
} | timeout $((BOOT_TIMEOUT + RUN_TIMEOUT + 30)) "$QEMU" -m 128 -display none -no-reboot \
        -drive "file=$IMAGE,format=raw,index=0,media=disk" \
        -debugcon "file:$OUT/debugcon.log" -serial none -monitor stdio \
        ${QEMU_FLAGS:-} > "$OUT/monitor.log" 2>&1 || {
    echo "smoke: QEMU failed or timed out, see $OUT/monitor.log" >&2
    exit 1
}

[ -f "$OUT/screen.txt" ] || { echo "smoke: no screen dump, see $OUT/monitor.log" >&2; exit 1; }

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
