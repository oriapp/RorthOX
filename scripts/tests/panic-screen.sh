#!/usr/bin/env bash
#
# The panic screen shows the reason (K32): PANIC(msg, file, line) prints "Reason: <msg>" and
# "at <file>:<line>" where the screen had the placeholder "Reason: Reason!", keeps every other line
# of the screen as it was, and fits 25 rows. It turns interrupts off first, uses no heap, writes the
# debugcon line "[PANIC]: <msg> at <file>:<line>" without building it in a fixed buffer, and halts.
#
# Host check only (scripts/tests/host/panic-screen.c): the real sys.c, tty.c and string.c, built
# with the host C compiler and AddressSanitizer, on a VGA page at 0xB8000 with unmapped pages
# around it. Cases:
#   basic           message, file and line: every cell of the screen against the model, and the
#                   debugcon line
#   newline         the boot panic "Failed to load shell.elf\n" (the newline shows as a space)
#   empty           empty message and file
#   format          a message and file with % sequences show as they are
#   control         newline, return, backspace, tab, ESC in the message and file show as spaces and
#                   don't move the text to other rows
#   lines           INT_MAX, INT_MIN and -1 as the line
#   fit             the longest message (71) and file (64, with an 11-character line) that fit
#   long            longer messages and files are cut to one row each, and the rows after them stay
#                   in place; debugcon has both fields, at most 1023 characters of each
#   null            NULL message or file: fixed text, the rest in place
#   debug_overflow  a 999-character message with a 1000-character file (overflowed the old
#                   1024-byte debug buffer)
# Every case: IRQ_OFF before any output, PAUSE reached once, PANIC never returns, no kmalloc/
# kzalloc/kfree, no itoa or strcat, nothing written after the screen, row 0 still the copyright.
# The QEMU side is in panic.sh and kernel-exceptions.sh.
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/panic-screen
rm -rf "$OUT"
mkdir -p "$OUT"
[ -f build/config/rorthox-config.h ] || { echo "panic-screen: run 'make all' first" >&2; exit 1; }

CASES="basic newline empty format control lines fit long null debug_overflow"
CFLAGS=(-std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address -fno-omit-frame-pointer
        -Dsnprintf=kernel_snprintf -Dsprintf=kernel_sprintf -Dvsnprintf=kernel_vsnprintf
        -Dvsprintf=kernel_vsprintf -Ditoa_hex=kernel_itoa_hex
        -I kernel -I build/config -I scripts/tests/host)

echo "panic-screen: host check of PANIC (AddressSanitizer)"
status="not built"
build() {
    "$HOST_CC" "${CFLAGS[@]}" -Ditoa=panic_itoa -Dstrcat=panic_strcat -c kernel/arch/i686/sys.c -o "$OUT/sys.o" &&
    # tty.c has its own printf and vprintf: renamed, so the check keeps the C library's printf.
    "$HOST_CC" "${CFLAGS[@]}" -Dprintf=kernel_printf -Dvprintf=kernel_vprintf -c kernel/arch/i686/tty.c -o "$OUT/tty.o" &&
    "$HOST_CC" "${CFLAGS[@]}" -c kernel/string/string.c -o "$OUT/string.o" &&
    "$HOST_CC" "${CFLAGS[@]}" -c kernel/memory/memory.c -o "$OUT/memory.o" &&
    "$HOST_CC" "${CFLAGS[@]}" -c scripts/tests/host/panic-screen.c -o "$OUT/panic-screen.o" &&
    "$HOST_CC" "${CFLAGS[@]}" "$OUT/panic-screen.o" "$OUT/sys.o" "$OUT/tty.o" "$OUT/string.o" "$OUT/memory.o" \
        -o "$OUT/host-check"
}
if build > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 80
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 20 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$status" > "$OUT/host-status.txt"

check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report"                 "$OUT/host-check.log" "AddressSanitizer" absent
for c in $CASES; do
    check "$c" "$OUT/host-check.log" "PASS $c" line
done
check "every case ran and passed" "$OUT/host-check.log" "panic-screen host check: 0 of 10 cases failed" line
if [ "$failures" -ne 0 ]; then
    echo "panic-screen: $failures check(s) failed" >&2
    exit 1
fi
echo "panic-screen: all checks passed"
