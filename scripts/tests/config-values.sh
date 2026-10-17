#!/usr/bin/env bash
#
# KEY_* and SYSCALL_* values in config.mk. Build checks only, no QEMU.
#
# A copy of Makefile and config.mk in build/tests gets the values below.
#   accepted  make (with /bin/sh and with SHELL=/bin/bash) parses them, and the rorthox-config.h it
#             writes has the expected #define lines.
#   rejected  make stops with the error of the check that owns the value (not a later build error):
#             key codes in the byte range (0, 0xFF, 255), above 0xFFFF, too long to fit 32 bits
#             (so they can't wrap back into range), two keys with the same code (also when spelled
#             differently), two syscalls with the same number (also 11 and 0xb). A non-numeric or
#             zero-padded key code stops at the numeric spelling check.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/config-values
rm -rf "$OUT"
mkdir -p "$OUT"
FIX="$OUT/config"
RESULTS="$OUT/results.txt"
: > "$RESULTS"

KEY_ERROR='*** key values must be distinct and between 0x100 and 0xFFFF'
SYSCALL_ERROR='*** syscall numbers must be distinct and at most 10 digits long'
NUMERIC_ERROR='*** numeric config.mk values must be hexadecimal (0x...) or decimal without leading zeros'

# set_values <NAME=value>...: the fixture's config.mk with those lines replaced.
set_values() {
    local pair
    rm -rf "$FIX"
    mkdir -p "$FIX"
    cp Makefile "$FIX/Makefile"
    cp config.mk "$FIX/config.mk"
    for pair in "$@"; do
        NAME="${pair%%=*}" VALUE="${pair#*=}" awk \
            '$0 ~ "^" ENVIRON["NAME"] " = " { print ENVIRON["NAME"] " = " ENVIRON["VALUE"]; next } { print }' \
            "$FIX/config.mk" > "$FIX/config.mk.tmp"
        mv "$FIX/config.mk.tmp" "$FIX/config.mk"
        grep -qxF "${pair%%=*} = ${pair#*=}" "$FIX/config.mk" || return 1
    done
}

# accept <tag> <header line> <NAME=value>...: parsed by both shells, the header has the line.
accept() {
    local tag="$1" want="$2" sh
    shift 2
    set_values "$@" || { echo "FAIL $tag: could not set the values" >> "$RESULTS"; return; }
    for sh in /bin/sh /bin/bash; do
        if make -s -C "$FIX" -n SHELL="$sh" build/config/rorthox-config.h > "$FIX/$tag-$(basename "$sh").log" 2>&1; then
            echo "PASS $tag parsed $sh" >> "$RESULTS"
        else
            echo "  $tag: make $sh refused it: $(grep -m1 -F '***' "$FIX/$tag-$(basename "$sh").log" || true)"
        fi
    done
    if make -s -C "$FIX" build/config/rorthox-config.h > "$FIX/$tag-emit.log" 2>&1 &&
            grep -qxF -- "$want" "$FIX/build/config/rorthox-config.h"; then
        echo "PASS $tag emitted" >> "$RESULTS"
    else
        echo "  $tag: expected '$want' in rorthox-config.h"
    fi
    echo "  accept $tag: $*"
}

# reject <tag> <error> <NAME=value>...: both shells stop with that error.
reject() {
    local tag="$1" error="$2" sh log
    shift 2
    set_values "$@" || { echo "FAIL $tag: could not set the values" >> "$RESULTS"; return; }
    for sh in /bin/sh /bin/bash; do
        log="$FIX/$tag-$(basename "$sh").log"
        if make -s -C "$FIX" -n SHELL="$sh" build/config/rorthox-config.h > "$log" 2>&1; then
            echo "  $tag: make $sh accepted it"
        elif grep -qF -- "$error" "$log"; then
            echo "PASS $tag rejected $sh" >> "$RESULTS"
        else
            echo "  $tag: make $sh failed, but not at the check: $(grep -m1 -F '***' "$log" || head -n1 "$log")"
        fi
    done
    echo "  reject $tag: $*"
}

ACCEPTED="defaults key-low key-high key-padded-hex syscalls-reordered"
echo "config-values: accepted values"
accept defaults           "#define RORTHOX_KEY_PAGE_DOWN 0x109"
accept key-low            "#define RORTHOX_KEY_UP 0x100"            KEY_UP=0x100
accept key-high           "#define RORTHOX_KEY_DOWN 65535"          KEY_DOWN=65535
accept key-padded-hex     "#define RORTHOX_KEY_HOME 0x0200"         KEY_HOME=0x0200
accept syscalls-reordered "#define SYSCALL_TERMINAL_MOVE_CURSOR 0"  SYSCALL_TERMINAL_MOVE_CURSOR=0 SYSCALL_SUM=11

REJECTED="key-zero key-0xff key-255 key-0x10000 key-65536 key-wrap-hex key-wrap-dec key-dup key-dup-spelling \
key-word key-zero-padded syscall-dup syscall-dup-spelling"
echo "config-values: rejected values"
reject key-zero             "$KEY_ERROR"     KEY_UP=0
reject key-0xff             "$KEY_ERROR"     KEY_LEFT=0xFF
reject key-255              "$KEY_ERROR"     KEY_PAGE_DOWN=255
reject key-0x10000          "$KEY_ERROR"     KEY_RIGHT=0x10000
reject key-65536            "$KEY_ERROR"     KEY_END=65536
# 2^64 + 0x200: bash arithmetic wraps it to 0x200, a free key code (dash saturates it).
reject key-wrap-hex         "$KEY_ERROR"     KEY_INSERT=0x10000000000000200
reject key-wrap-dec         "$KEY_ERROR"     KEY_INSERT=18446744073709552128
reject key-dup              "$KEY_ERROR"     KEY_DELETE=0x101
reject key-dup-spelling     "$KEY_ERROR"     KEY_PAGE_UP=257
reject key-word             "$NUMERIC_ERROR" KEY_UP=abc
reject key-zero-padded      "$NUMERIC_ERROR" KEY_UP=0400
reject syscall-dup          "$SYSCALL_ERROR" SYSCALL_PRINTC=9
reject syscall-dup-spelling "$SYSCALL_ERROR" SYSCALL_PRINTC=0xb

for t in $ACCEPTED; do
    check "$t: parsed with /bin/sh"     "$RESULTS" "PASS $t parsed /bin/sh" line
    check "$t: parsed with /bin/bash"   "$RESULTS" "PASS $t parsed /bin/bash" line
    check "$t: header line written"     "$RESULTS" "PASS $t emitted" line
done
for t in $REJECTED; do
    check "$t: rejected with /bin/sh"   "$RESULTS" "PASS $t rejected /bin/sh" line
    check "$t: rejected with /bin/bash" "$RESULTS" "PASS $t rejected /bin/bash" line
done
if [ "$failures" -ne 0 ]; then
    echo "config-values: $failures check(s) failed" >&2
    exit 1
fi
echo "config-values: all checks passed"
