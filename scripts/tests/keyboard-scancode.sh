#!/usr/bin/env bash
#
# The keyboard interrupt must ignore scancodes past the end of the set-1 lookup table in
# kernel/keyboard/classic.c (K18: the bound check was `scancode > size`, so scancode 0x52 read
# one byte past the 82-byte table).
#
# Host check only (scripts/tests/host/keyboard-scancode.c): the real classic.c built with the host
# C compiler and AddressSanitizer, so the out-of-bounds read itself stops the check (a plain value
# check could pass when the byte after the table happens to be 0). It looks up all 256 byte values
# with caps lock off and on, and runs the interrupt handler for all 256 values read from port 0x60.
# No QEMU boot.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/keyboard-scancode
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-*
[ -f build/config/rorthox-config.h ] || { echo "keyboard-scancode: run 'make all' first" >&2; exit 1; }

echo "keyboard-scancode: host check of the scancode lookup (AddressSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -fsanitize=address -fno-omit-frame-pointer -w \
        -I kernel -I kernel/keyboard -I build/config -I scripts/tests/host \
        scripts/tests/host/keyboard-scancode.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 40
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"

check "host check built and exited with status 0"        "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report"                       "$OUT/host-check.log"  "AddressSanitizer" absent
check "lookup of all 256 values ran"                     "$OUT/host-check.log"  "lookup: all 256 values, caps lock off and on" line
check "interrupt handler for all 256 values ran"         "$OUT/host-check.log"  "interrupt: all 256 values from port 0x60, caps lock off and on" line
check "every check passed"                               "$OUT/host-check.log"  "keyboard-scancode host check: 0 of 2055 checks failed" line
if [ "$failures" -ne 0 ]; then
    echo "keyboard-scancode: $failures check(s) failed" >&2
    exit 1
fi
echo "keyboard-scancode: all checks passed"
