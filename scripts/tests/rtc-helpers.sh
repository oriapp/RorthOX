#!/usr/bin/env bash
#
# RTC time helpers in kernel/time/rtc/rtc.h: getformattedtime and gettime return an allocated,
# terminated string that fits its allocation (or NULL when an allocation fails, leaving nothing
# allocated), and printtime prints the clock with leading zeros without using the heap.
#
# Host check only (no QEMU): scripts/tests/host/rtc-helpers.c includes the real rtc.h and builds it
# with the real kernel/string/string.c (formatting names prefixed with kernel_, as in
# formatting.sh) and the test heap scripts/tests/host/kalloc.c. Results and format strings end at
# an unmapped page, every case runs in its own process with a time limit.
# Cases:
#   tokens      h m s (leading zero) and H M S (leading space) alone and in clock formats, at
#               several times
#   repeat      repeated tokens: the text is longer than the format (up to 8192 characters)
#   literals    every other byte is copied as it is (%, backslashes, bytes above 0x7f)
#   empty       an empty format gives an empty string
#   alloc_fail  each allocation failing in turn: NULL, nothing left allocated (also gettime)
#   gettime     "H:M:S" without padding, up to 255:255:255
#   printtime   a fake CMOS (BCD, binary, 12-hour): "System Time: " in light green, then
#               HH:MM:SS and a newline, no heap allocation
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=${RORTHOX_TEST_ROOT:-build/tests}/rtc-helpers
rm -rf "$OUT"
mkdir -p "$OUT"
[ -f build/config/rorthox-config.h ] || { echo "rtc-helpers: run 'make all' first" >&2; exit 1; }

CASES="tokens repeat literals empty alloc_fail gettime printtime"
RENAME="-Dsnprintf=kernel_snprintf -Dvsnprintf=kernel_vsnprintf"

echo "rtc-helpers: host check"
status="not built"
# shellcheck disable=SC2086
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -fsanitize=signed-integer-overflow -fno-sanitize-recover=all $RENAME \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/rtc-helpers.c scripts/tests/host/kalloc.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 20 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$status" > "$OUT/host-status.txt"

check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
for c in $CASES; do
    check "$c" "$OUT/host-check.log" "PASS $c" line
done
check "every case ran and passed" "$OUT/host-check.log" "rtc helpers host check: 0 of 7 cases failed" line
if [ "$failures" -ne 0 ]; then
    echo "rtc-helpers: $failures check(s) failed" >&2
    exit 1
fi
echo "rtc-helpers: all checks passed"
