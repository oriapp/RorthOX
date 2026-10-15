#!/usr/bin/env bash
#
# The terminal uses all 25 rows of the VGA text screen and scrolls: when output reaches the end
# of the last row, every row moves up one (character and colour), the last row is cleared to
# spaces, and output goes on there. Nothing is written past the 80 x 25 page (K7: rows grew
# without limit and output went on below the screen, through the rest of video memory).
#
# 1. Host check (scripts/tests/host/tty-screen.c, group "scroll"): the real kernel/arch/i686/tty.c
#    built with the host C compiler on a VGA page mapped at 0xB8000 with unmapped pages around it:
#    print, printc, terminal_writechar and kernel printf (literal text, newlines, %i, an unknown
#    format character) through many scrolls, colours moving with the text, terminal_putchar off
#    the screen, and 4000 mixed calls, compared cell by cell with a model.
# 2. QEMU: copy bin/os.bin, add programs/ttytest to the copy only, boot, run "ttytest.elf scroll".
#    After its "armed" line and the shell's prompt, each key starts one phase: 64 numbered lines
#    and an end marker without a newline, then 40 blocks of exactly 80 characters (no newlines)
#    and an end marker. After each phase the 4000-byte VGA page is read (raw characters and
#    colours): rows 0-23 must be the last 24 lines (or blocks), row 24 the end marker, every other
#    cell a white-on-black space. The video memory right after the page (0xB8FA0-0xBBFFF) must
#    not change. A wider read of video memory is only used to know that a phase has finished.
# bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh

OUT=build/tests/tty-scroll
PROGRAM=programs/ttytest/build/ttytest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "tty-scroll: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "tty-scroll: host check of terminal scrolling"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/tty-screen.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" scroll > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"   "$OUT/host-status.txt" "exit=0" line
check "host check: every scroll case passed"        "$OUT/host-check.log"  "tty-screen scroll host check: 0 of 9 cases failed" line

# The video memory after the visible page, up to 0xBC000.
ABOVE_ADDRESS=0xb8fa0
ABOVE_BYTES=$((0xbc000 - 0xb8fa0))

prompts() {
    sed -n "/${RORTHOX_NAME} v${RORTHOX_VERSION}/,\$p" "$Q_OUT/screen.txt" | tr -cd '>' | wc -c
}

# wait_prompts <n>: wait until the shell printed its prompt <n> times (it is waiting for keys).
wait_prompts() {
    local deadline=$((SECONDS + RUN_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        [ "$(prompts)" -ge "$1" ] && return 0
        sleep 0.5
    done
    echo "  gave up waiting for shell prompt $1"
    return 1
}

# mem_dump <address> <bytes> <file>: read guest memory until two reads agree (nothing is printing).
mem_dump() {
    local i
    rm -f "$3" "$OUT/dump-a.bin"
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/dump-b.bin"
        qemu_cmd "pmemsave $1 $2 \"$OUT/dump-b.bin\""
        wait_file "$OUT/dump-b.bin" "$2" || return 1
        if [ -f "$OUT/dump-a.bin" ] && cmp -s "$OUT/dump-a.bin" "$OUT/dump-b.bin"; then
            mv "$OUT/dump-b.bin" "$3"
            rm -f "$OUT/dump-a.bin"
            return 0
        fi
        mv "$OUT/dump-b.bin" "$OUT/dump-a.bin"
        sleep 0.3
    done
    return 1
}

# wait_wide <text>: wait until <text> is anywhere in the first 32000 bytes of video memory, so a
# phase counts as finished even if its output went below the screen.
wait_wide() {
    local deadline=$((SECONDS + RUN_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for \"$1\""; return 1; }
        rm -f "$OUT/wide.bin"
        qemu_cmd "pmemsave 0xb8000 32000 \"$OUT/wide.bin\""
        if wait_file "$OUT/wide.bin" 32000 && vga_to_text "$OUT/wide.bin" | grep -qF -- "$1"; then
            echo "  \"$1\" in video memory after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$1\" after $RUN_TIMEOUT s"
    return 1
}

# phase <n> <end marker>: wait for the marker, then save the page and the memory after it.
phase() {
    wait_wide "$2" && mem_dump 0xb8000 4000 "$OUT/page$1.bin" &&
        mem_dump "$ABOVE_ADDRESS" "$ABOVE_BYTES" "$OUT/above$1.bin" && echo "phase $1" >> "$OUT/steps.txt"
}

IMG="$OUT/tty-scroll.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::

echo "tty-scroll: run ttytest.elf scroll"
: > "$OUT/steps.txt"
qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && wait_prompts 1; then
    type_line "ttytest.elf scroll"
    if wait_screen "ttytest scroll: armed" "$RUN_TIMEOUT" && wait_prompts 2 &&
            mem_dump "$ABOVE_ADDRESS" "$ABOVE_BYTES" "$OUT/above0.bin"; then
        echo "armed" >> "$OUT/steps.txt"
        qemu_cmd "sendkey a"
        if phase 1 "ttytest scroll: lines end"; then
            qemu_cmd "sendkey a"
            if phase 2 "ttytest scroll: wrap end"; then
                qemu_cmd "sendkey a"
                wait_wide "Process was terminated" && echo "done" >> "$OUT/steps.txt" || true
            fi
        fi
    fi
fi
qemu_stop || true

python3 -B - "$OUT" > "$OUT/cells.txt" 2>&1 << 'EOF' || true
import os
import sys

out = sys.argv[1]
CLEAR = (0x20, 0x0F)


def read(name):
    path = os.path.join(out, name)
    return open(path, "rb").read() if os.path.exists(path) else None


def scroll_line(n):
    s = "scroll line %02d " % n
    return s + "".join(chr(ord("a") + (n + i) % 26) for i in range(len(s), 20 + (7 * n) % 50))


def wrap_block(n):
    s = "wrap block %02d " % n
    return s + "".join(chr(ord("A") + (n + i) % 26) for i in range(len(s), 80))


def check_page(k, texts):
    data = read("page%d.bin" % k)
    if data is None:
        print("phase %d: no VGA dump" % k)
        return
    cells = [(data[2 * i], data[2 * i + 1]) for i in range(2000)]
    wrong = 0
    for r, t in enumerate(texts):
        want = [(ord(c), 0x0F) for c in t] + [CLEAR] * (80 - len(t))
        got = cells[r * 80:(r + 1) * 80]
        if got != want:
            wrong += 1
            if wrong > 4 and r != 24:
                continue
            shown = "".join(chr(c) if 32 <= c < 127 else " " for c, _ in got).rstrip()
            bad = next(i for i in range(80) if got[i] != want[i])
            print("phase %d: row %d is %r, column %d is %r/0x%02x, expected %r/0x%02x" % (
                k, r, shown, bad, chr(got[bad][0]), got[bad][1], chr(want[bad][0]), want[bad][1]))
    if wrong:
        print("phase %d: %d of 25 rows differ" % (k, wrong))
    else:
        print("phase %d: page is exactly the last 24 rows and the end marker" % k)
    if all(cells[24 * 80 + i] == (ord(c), 0x0F) for i, c in enumerate(texts[24])):
        print("phase %d: end marker on row 24" % k)


check_page(1, [scroll_line(n) for n in range(41, 65)] + ["ttytest scroll: lines end"])
check_page(2, [wrap_block(n) for n in range(17, 41)] + ["ttytest scroll: wrap end"])

base = read("above0.bin")
for k in (1, 2):
    data = read("above%d.bin" % k)
    if base is None or data is None:
        print("phase %d: no dump after the page" % k)
    elif data == base:
        print("phase %d: memory after the page unchanged" % k)
    else:
        first = next(i for i in range(len(data)) if data[i] != base[i])
        print("phase %d: memory after the page changed, first at 0x%x" % (k, 0xb8fa0 + first))
EOF
sed 's/^/  /' "$OUT/cells.txt"

check "kernel reached the end of init (debugcon)"        "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                        "$OUT/debugcon.log" "PANIC" absent
check "ttytest armed with the shell idle"                 "$OUT/steps.txt"    "armed" line
check "phase 1 (64 lines) finished"                       "$OUT/steps.txt"    "phase 1" line
check "phase 2 (40 blocks of 80) finished"                "$OUT/steps.txt"    "phase 2" line
check "ttytest ended"                                     "$OUT/steps.txt"    "done" line
check "lines: end marker on the last row"                 "$OUT/cells.txt"    "phase 1: end marker on row 24" line
check "lines: rows 0-23 are lines 41-64, rest cleared"    "$OUT/cells.txt"    "phase 1: page is exactly the last 24 rows and the end marker" line
check "lines: nothing written after the page"             "$OUT/cells.txt"    "phase 1: memory after the page unchanged" line
check "blocks: end marker on the last row"                "$OUT/cells.txt"    "phase 2: end marker on row 24" line
check "blocks: rows 0-23 are blocks 17-40, rest cleared"  "$OUT/cells.txt"    "phase 2: page is exactly the last 24 rows and the end marker" line
check "blocks: nothing written after the page"            "$OUT/cells.txt"    "phase 2: memory after the page unchanged" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish tty-scroll
