#!/usr/bin/env bash
# The per-process key queue drops keys when it is full instead of overwriting unread ones.
# Host check of the real kernel/keyboard/keyboard.c under ASan. Env: HOST_CC=cc. No QEMU boot.
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh

OUT=build/tests/keyboard-queue
mkdir -p "$OUT"
rm -f "$OUT"/host-*
[ -f build/config/rorthox-config.h ] || { echo "keyboard-queue: run 'make all' first" >&2; exit 1; }
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -fsanitize=address -fno-omit-frame-pointer -no-pie -w \
        -I kernel -I kernel/keyboard -I build/config -I scripts/tests/host \
        scripts/tests/host/keyboard-queue.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed -n '1,20{s/^/  /;p;}' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    sed -n '1,10p' "$OUT/host-build.log"
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report" "$OUT/host-check.log" "AddressSanitizer" absent
check "full queue keeps unread keys, wraps correctly" "$OUT/host-check.log" "keyboard-queue: PASS" line
if [ "$failures" -ne 0 ]; then
    echo "keyboard-queue: $failures check(s) failed" >&2
    exit 1
fi
echo "keyboard-queue: all checks passed"
