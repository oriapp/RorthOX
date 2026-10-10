#!/usr/bin/env bash
#
# Headless smoke test for RorthOX.
#
# Boots bin/os.bin in QEMU without a display, waits for the shell, types a command into it
# through the QEMU monitor ("sendkey"), waits for the program to finish, then checks:
#   - the debugcon log (the kernel writes to I/O port 0xE9), and
#   - the VGA text screen, read straight from guest memory at 0xB8000.
# The QEMU helpers are in scripts/qemu-lib.sh.
#
# Usage:  ./scripts/smoke.sh            (or: make smoke)
# Env:    IMAGE=bin/os.bin   OUT=build/smoke   QEMU=qemu-system-i386   QEMU_FLAGS="extra flags"
#         BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for the program)
#
# Results land in $OUT: debugcon.log, screen.txt (80x25 text), screen.ppm, monitor.log.

set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="${IMAGE:-bin/os.bin}"
OUT="${OUT:-build/smoke}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
COMMAND="blank.elf one two"
QEMU_TIMEOUT=$((BOOT_TIMEOUT + RUN_TIMEOUT + 30))
. ./scripts/qemu-lib.sh
SHELL_READY="${RORTHOX_NAME} v${RORTHOX_VERSION}"            # the shell's banner: the shell is running
PROGRAM_DONE="Process was terminated"   # printed by the kernel when a program exits

echo "smoke: boot $IMAGE"
qemu_start "$IMAGE" "$OUT"
# If a wait times out, carry on: the checks below report what is missing.
if wait_screen "$SHELL_READY" "$BOOT_TIMEOUT"; then
    type_line "$COMMAND"
    wait_screen "$PROGRAM_DONE" "$RUN_TIMEOUT" || true
fi
qemu_stop || true

echo "smoke: checks"
check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                   "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"             "$OUT/screen.txt"   "$SHELL_READY"
check "typed command echoed by the shell"            "$OUT/screen.txt"   "> $COMMAND"
check "blank.elf was loaded from FAT16 and ran"      "$OUT/screen.txt"   "Blank loaded"
check "argv passed to the program (own line)"       "$OUT/screen.txt"   "two" line
finish smoke
