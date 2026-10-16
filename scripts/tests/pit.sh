#!/usr/bin/env bash
# Check the real PIT driver's fixed 100 Hz programming and IRQ0 tick order.
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
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/pit.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed -n '1,40{s/^/  /;p;}' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed -n '1,10p' "$OUT/host-build.log"
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "PIT programming, registration and tick order" "$OUT/host-check.log" \
    "PIT: divisor 11931, mode 3, IRQ0, EOI then tick then task_next: passed" line
if [ "$failures" -ne 0 ]; then
    echo "pit: $failures check(s) failed" >&2
    exit 1
fi
echo "pit: all checks passed"
