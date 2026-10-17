#!/usr/bin/env bash
# Check the real keyboard IRQ handler against an independent set-1 map under ASan.
# All 256 byte values run with Num Lock off/on, caps off/on and neither/left/right/both shifts held.
# Env: HOST_CC=cc. No QEMU boot in this host bounds/mapping check.
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/keyboard-scancode
mkdir -p "$OUT"
rm -f "$OUT"/host-*
[ -f build/config/rorthox-config.h ] || { echo "keyboard-scancode: run 'make all' first" >&2; exit 1; }
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -fsanitize=address -fno-omit-frame-pointer -no-pie -w \
        -I kernel -I kernel/keyboard -I build/config -I scripts/tests/host \
        scripts/tests/host/keyboard-scancode.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed -n '1,40{s/^/  /;p;}' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed -n '1,10p' "$OUT/host-build.log"
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report" "$OUT/host-check.log" "AddressSanitizer" absent
check "complete byte/modifier sweep ran" "$OUT/host-check.log" \
    "interrupt: all 256 values, Num Lock off/on, caps off/on, neither/left/right/both shifts" line
check "modifier transitions ran" "$OUT/host-check.log" "interrupt: modifier transitions checked" line
check "Num Lock press cycles ran" "$OUT/host-check.log" "interrupt: Num Lock press cycles checked" line
check "Pause ran" "$OUT/host-check.log" "interrupt: Pause checked" line
check "extended byte/modifier sweep ran" "$OUT/host-check.log" \
    "interrupt: E0 plus all 256 values in every Num Lock/Caps/Shift state checked" line
check "navigation keys ran" "$OUT/host-check.log" \
    "interrupt: E0 navigation keys give their key values, breaks give nothing" line
check "every mapping and state check passed" "$OUT/host-check.log" "keyboard-scancode host check: passed" line
if [ "$failures" -ne 0 ]; then
    echo "keyboard-scancode: $failures check(s) failed" >&2
    exit 1
fi
echo "keyboard-scancode: all checks passed"
