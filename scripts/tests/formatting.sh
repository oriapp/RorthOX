#!/usr/bin/env bash
#
# Kernel text formatting (K11): the buffer formatter (snprintf, sprintf, vsnprintf, vsprintf) is
# one bounded function that never calls itself, itoa_hex puts the digits in the right order, and
# the terminal's print_hex and printf("%x") show all eight digits of the value.
#
# Host check only (no QEMU): scripts/tests/host/formatting.c builds the real
# kernel/string/string.c and kernel/arch/i686/tty.c with the host C compiler. The formatting
# functions of string.c get a kernel_ prefix, so the check can't call the C library's by mistake.
# Every destination, format string and %s argument ends at an unmapped page, every case runs in
# its own process with a time limit, and signed overflow stops the case (-fsanitize).
# Cases:
#   api              snprintf, sprintf, vsnprintf, vsprintf (the va_list functions directly)
#   empty            empty output, size 0 with a NULL destination, size 1; a NULL destination
#                    with a size above 0 returns -1 and writes nothing
#   truncate         a mixed format at every size from 0 to its length + 2: return value, text,
#                    NUL, nothing written after the size or before the buffer
#   pad              %02d (0, 7, 12, 123, negatives, INT_MIN) and the RTC time format
#   limits           %d %i %u %x %X at 0, INT_MAX, INT_MIN, UINT_MAX and high-bit values
#   char             %c with a zero byte is output and counted
#   string           %s ending at a page end, %s of NULL
#   unsupported      widths, flags, length modifiers, %p %f %n %q: -1, a terminated prefix, no
#                    write through an argument
#   trailing         a lone % at the end prints %; "%0" and "%02" at the end are not read past
#   int_max          an output of exactly INT_MAX characters, and one more (-1)
#   itoa_hex         digits in order, negatives, INT_MIN, bases 2, 10, 16, 36 (exact-size buffers)
#   itoa_base        bases 0, 1, -16, 37, 100 give an empty string
#   print_hex        0x and eight uppercase digits on the screen
#   printf_hex       kernel printf("%x") shows the same and counts 10
#   printf_trailing  kernel printf with a % at the end of a format that ends at a page end
#   printf_others    %i %s %b %q %B, newlines and scrolling keep their output and count
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/formatting
rm -rf "$OUT"
mkdir -p "$OUT"
[ -f build/config/rorthox-config.h ] || { echo "formatting: run 'make all' first" >&2; exit 1; }

CASES="api empty truncate pad limits char string unsupported trailing int_max itoa_hex itoa_base
       print_hex printf_hex printf_trailing printf_others"
RENAME="-Dsnprintf=kernel_snprintf -Dsprintf=kernel_sprintf -Dvsnprintf=kernel_vsnprintf
        -Dvsprintf=kernel_vsprintf -Ditoa_hex=kernel_itoa_hex"

echo "formatting: host check"
status="not built"
# shellcheck disable=SC2086
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -fsanitize=signed-integer-overflow -fno-sanitize-recover=all $RENAME \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/formatting.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
    # The formatter under test is the kernel's.
    nm "$OUT/host-check" | grep -E ' T kernel_(vsnprintf|vsprintf|snprintf|sprintf|itoa_hex)$' | wc -l |
        sed 's/^ */kernel functions=/' > "$OUT/symbols.txt"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 20 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$status" > "$OUT/host-status.txt"

check "host check built and exited with status 0"        "$OUT/host-status.txt" "exit=0" line
check "the kernel's five formatting functions are linked" "$OUT/symbols.txt" "kernel functions=5" line
for c in $CASES; do
    check "$c" "$OUT/host-check.log" "PASS $c" line
done
check "every case ran and passed" "$OUT/host-check.log" "formatting host check: 0 of 16 cases failed" line
if [ "$failures" -ne 0 ]; then
    echo "formatting: $failures check(s) failed" >&2
    exit 1
fi
echo "formatting: all checks passed"
