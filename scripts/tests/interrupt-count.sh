#!/usr/bin/env bash
#
# x86 has 256 interrupt vectors (K28: the kernel built 512 stubs, 512 pointer-table entries, a
# 512-entry IDT and 512 callback slots). Host and build checks only, no QEMU; the loaded IDTR limit
# (0x7ff) is checked by idt-load.sh, and exception-frames.sh enters every stub.
#
# 1. Objects (scripts/tests/lib/interrupt_count.py): build/idt/idt.asm.o defines exactly int0 to
#    int255 and a pointer table of 256 entries; in build/idt/idt.o idt_descriptors is 2048 bytes and
#    interrupt_callbacks 1024 bytes; in the kernel linked as ELF, table entry i is the address of
#    int<i>.
# 2. Host check (scripts/tests/host/interrupt-count.c, AddressSanitizer): the real
#    idt_register_interrupt_callback accepts 0 and 255 and rejects -1, 256, 511, INT_MAX, ... without
#    touching the callback array.
# 3. config.mk: TOTAL_INTERRUPTS must be 256. A copy of Makefile and config.mk in build/tests is
#    parsed with "make -n" for each value, once with make's /bin/sh and once with SHELL=/bin/bash:
#    256, 0x100 and "256 # comment" are accepted; 0, 255, 257, 512, 65536, 0256 and values that
#    bash's 64-bit arithmetic wraps to 256 are rejected by the config checks (a make error naming
#    the problem, not a later build error).
#
# Env: HOST_CC=cc

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/interrupt-count
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "interrupt-count: python3 not found" >&2; exit 1; }
[ -f build/idt/idt.asm.o ] && [ -f build/idt/idt.o ] && [ -f build/kernelfull.o ] ||
    { echo "interrupt-count: run 'make all' first" >&2; exit 1; }

echo "interrupt-count: stubs, pointer table and IDT arrays"
i686-elf-ld -L build/config -T kernel/linker.ld --oformat elf32-i386 build/kernelfull.o -o "$OUT/kernel.elf"
if python3 -B scripts/tests/lib/interrupt_count.py build/idt/idt.asm.o build/idt/idt.o "$OUT/kernel.elf" \
    "$OUT/objects.txt" > "$OUT/objects.log" 2>&1; then status=0; else status=$?; fi
sed 's/^/  /' "$OUT/objects.log"
echo "exit=$status" > "$OUT/objects-status.txt"

echo "interrupt-count: host check of idt_register_interrupt_callback (AddressSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address -fno-omit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/interrupt-count.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 40
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"

echo "interrupt-count: TOTAL_INTERRUPTS values in config.mk"
FIX="$OUT/config"
mkdir -p "$FIX"
: > "$OUT/config.txt"
# config_case accept|reject <tag> <value>: parse the Makefile with that value, with make's default
# shell (/bin/sh) and with bash (/bin/sh on some systems; its arithmetic wraps at 64 bits).
config_case() {
    local want="$1" tag="$2" value="$3" sh log got
    cp Makefile "$FIX/Makefile"
    sed "s/^TOTAL_INTERRUPTS = .*/TOTAL_INTERRUPTS = $value/" config.mk > "$FIX/config.mk"
    grep -qxF "TOTAL_INTERRUPTS = $value" "$FIX/config.mk" || { echo "FAIL $tag: could not set the value" >> "$OUT/config.txt"; return; }
    for sh in /bin/sh /bin/bash; do
        log="$FIX/$tag-$(basename "$sh").log"
        if make -s -C "$FIX" -n SHELL="$sh" build/config/rorthox-config.h > "$log" 2>&1; then got=accept; else got=reject; fi
        if [ "$got" = reject ] && ! grep -qE '\*\*\* .*(TOTAL_INTERRUPTS|numeric config.mk values)' "$log"; then
            got="reject (not by a config check: $(grep -m1 -F '***' "$log" || true))"
        fi
        echo "  TOTAL_INTERRUPTS = $value ($sh): $got"
        [ "$got" = "$want" ] && echo "PASS $tag $sh" >> "$OUT/config.txt" || echo "FAIL $tag $sh" >> "$OUT/config.txt"
    done
}
config_case accept 256         "256"
config_case accept hex         "0x100"
config_case accept comment     "256   # every x86 vector"
config_case reject zero        "0"
config_case reject 255         "255"
config_case reject 257         "257"
config_case reject 512         "512"
config_case reject 65536       "65536"
config_case reject leading-0   "0256"
config_case reject wrap-hex    "0x10000000000000100"
config_case reject wrap-dec    "18446744073709551872"

r="$OUT/objects.txt"
check "object checks ran"                                  "$OUT/objects-status.txt" "exit=0" line
check "idt.asm.o defines int0 to int255"                  "$r" "PASS stubs_0_to_255" line
check "idt.asm.o defines no int256 or above"              "$r" "PASS no_stub_256_or_above" line
check "pointer table has 256 entries"                     "$r" "PASS pointer_table_256" line
check "pointer table entry i is int<i>"                   "$r" "PASS pointer_table_order" line
check "idt_descriptors is 2048 bytes"                     "$r" "PASS idt_2048_bytes" line
check "interrupt_callbacks is 1024 bytes"                 "$r" "PASS callbacks_1024_bytes" line
h="$OUT/host-check.log"
check "host check built and exited with status 0"         "$OUT/host-status.txt" "exit=0" line
check "no AddressSanitizer report"                        "$h" "AddressSanitizer" absent
check "callback array has 256 entries"                    "$h" "PASS count" line
check "callbacks for vectors 0 to 255 are stored"         "$h" "PASS accept" line
check "callbacks for -1, 256 ... INT_MAX are rejected"    "$h" "PASS reject" line
c="$OUT/config.txt"
for t in 256 hex comment; do
    check "config.mk: TOTAL_INTERRUPTS $t accepted (sh)"   "$c" "PASS $t /bin/sh" line
    check "config.mk: TOTAL_INTERRUPTS $t accepted (bash)" "$c" "PASS $t /bin/bash" line
done
for t in zero 255 257 512 65536 leading-0 wrap-hex wrap-dec; do
    check "config.mk: TOTAL_INTERRUPTS $t rejected (sh)"   "$c" "PASS $t /bin/sh" line
    check "config.mk: TOTAL_INTERRUPTS $t rejected (bash)" "$c" "PASS $t /bin/bash" line
done
if [ "$failures" -ne 0 ]; then
    echo "interrupt-count: $failures check(s) failed" >&2
    exit 1
fi
echo "interrupt-count: all checks passed"
