#!/usr/bin/env bash
# Check the real PIT driver: its fixed 100 Hz programming (divisor 11932, rounded up so a tick is
# never shorter than 10 ms), the milliseconds-to-ticks conversion of sleep, and the IRQ0 tick
# through the real interrupt dispatcher from ring 3 (EOI, count, wake, switch) and from the
# kernel's idle loop in ring 0 (count, wake, return, one EOI, no task state touched).
# Details in scripts/tests/host/pit.c.
# Env: HOST_CC=cc. No QEMU boot in this host check.
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/pit
mkdir -p "$OUT"
rm -f "$OUT"/host-*
[ -f build/config/rorthox-config.h ] || { echo "pit: run 'make all' first" >&2; exit 1; }
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address -fno-omit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/pit.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    grep -v ' ms -> ' "$OUT/host-check.log" | sed -n '1,40{s/^/  /;p;}'
else
    echo "  host build failed, see $OUT/host-build.log"
    sed -n '1,10p' "$OUT/host-build.log"
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
h="$OUT/host-check.log"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report" "$h" "AddressSanitizer" absent
check "PIT programming and registration" "$h" "PIT: divisor 11932, mode 3, IRQ0, at least 10 ms per tick" line
check "port writes 0x36, 0x9c, 0x2e" "$h" "PASS init" line
check "ms to ticks: ceil(ms / 10) + 2, 0 for 0" "$h" "PASS convert" line
check "IRQ0 from ring 3: save, EOI, count, wake, switch" "$h" "PASS tick-3" line
check "IRQ0 from ring 0: count, wake, one EOI, return" "$h" "PASS tick-0" line
check "IRQ1/IRQ7 from ring 3: save, task_page, one EOI" "$h" "PASS irq-3" line
check "IRQ1/IRQ7 from ring 0: no save, no task_page, one EOI" "$h" "PASS irq-0" line
check "every case passed" "$h" "pit host check: 0 of 6 cases failed" line
if [ "$failures" -ne 0 ]; then
    echo "pit: $failures check(s) failed" >&2
    exit 1
fi
echo "pit: all checks passed"
