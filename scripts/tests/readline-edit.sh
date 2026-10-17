#!/usr/bin/env bash
#
# Line editing: the cursor syscall (11) and the user library's readline with the arrow keys,
# Home, End, Delete and Backspace in the middle of a line.
#
# 1. Host check (scripts/tests/host/tty-screen.c, group "cursor"): the real kernel tty's
#    terminal_move_cursor() on a VGA page at 0xB8000. A move changes no cell, sets the row,
#    the column and the CRT cursor; a move off the page (also INT_MIN/INT_MAX) gives -EINVARG
#    and changes nothing; the next output lands at the new place, also after a scroll.
# 2. QEMU, one boot of a copy of bin/os.bin with programs/libtest added to the copy only:
#    a. At the shell's first prompt: type "lank.elf ac"; Home, b; End, Left, b; Left, Delete;
#       Right, Backspace; Home, Left, Backspace (nothing at the start of the line); End, Right,
#       b, c; then Up, Down, Insert, Page Up, Page Down (ignored). After each step the prompt row
#       must read exactly what the line is ("> blank.elf abc" at the end) and the prompt's cells
#       and everything above must be unchanged. Enter then runs blank.elf with the argument abc.
#    b. "libtest.elf cursor": prints "lt-m:ABCDE", moves -3, "x", +1, "y", four refused moves,
#       "|", a newline, moves -1 to column 79 of the row above, "RS". The rows must read
#       "lt-m:ABxDy|" with R in column 79, then "S"; every result right (0 for the four moves,
#       -EINVARG for the refused ones), ebx/esi/edi/ebp kept by the wrapper.
#    c. "libtest.elf readline": a 72-character prompt on the bottom row, then 85 letters typed
#       across the row edge (the screen scrolls), so Home moves back over more than one row,
#       and edits at both ends and in the middle of the second row.
#       Before Enter the two bottom rows must show exactly the prompt and the edited line with
#       nothing left behind it; after Enter the program prints "|" where the cursor is (right
#       after the line) and the line it read.
# bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=30 (for each
#      step)   STEP_TIMEOUT=10 (for each editing step to show its text)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
STEP_TIMEOUT="${STEP_TIMEOUT:-10}"
. ./scripts/qemu-lib.sh

OUT=build/tests/readline-edit
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "readline-edit: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "readline-edit: host check of terminal_move_cursor"
host_status="not built"
# Signed overflow stops the check: a range check that adds the delta first must not pass by
# wrapping around.
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -fsanitize=signed-integer-overflow -fno-sanitize-recover=all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/tty-screen.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" cursor > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"   "$OUT/host-status.txt" "exit=0" line
check "host check: every cursor case passed"        "$OUT/host-check.log"  "tty-screen cursor host check: 0 of 5 cases failed" line

# press <key>...: QEMU key names, one after the other. The sleep only spaces the presses; the
# screen says when they arrived.
press() {
    local key
    for key in "$@"; do
        qemu_cmd "sendkey $key"
        sleep 0.1
    done
}

# vga_dump <file>: the 4000-byte VGA page, read until two reads agree (nothing is still printing).
vga_dump() {
    local i
    rm -f "$1" "$OUT/dump-a.bin"
    for ((i = 0; i < 20; i++)); do
        rm -f "$OUT/dump-b.bin"
        qemu_cmd "pmemsave 0xb8000 4000 \"$OUT/dump-b.bin\""
        wait_file "$OUT/dump-b.bin" 4000 || return 1
        if [ -f "$OUT/dump-a.bin" ] && cmp -s "$OUT/dump-a.bin" "$OUT/dump-b.bin"; then
            mv "$OUT/dump-b.bin" "$1"
            rm -f "$OUT/dump-a.bin"
            return 0
        fi
        mv "$OUT/dump-b.bin" "$OUT/dump-a.bin"
        sleep 0.3
    done
    return 1
}

# wait_row <row> <text>: wait until screen row <row> (0-24) reads <text> (trailing blanks cut).
wait_row() {
    local deadline=$((SECONDS + STEP_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        [ "$(sed -n "$(($1 + 1))p" "$Q_OUT/screen.txt")" = "$2" ] && return 0
        sleep 0.3
    done
    echo "  row $1 never read \"$2\""
    return 1
}

# wait_after <marker> <text>: wait until <text> shows up on a line below <marker>.
wait_after() {
    local deadline=$((SECONDS + RUN_TIMEOUT))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        awk -v m="$1" -v t="$2" 'index($0, m) { seen = 1; next } seen && index($0, t) { found = 1 } END { exit !found }' \
            "$Q_OUT/screen.txt" && return 0
        sleep 0.3
    done
    echo "  \"$2\" never came after \"$1\""
    return 1
}

# step <name> <line> <key>...: press the keys, wait for the prompt row to read "> <line>", keep
# the VGA page. If the row never reads that, keep the page anyway (the check below shows what
# it reads) and skip the later steps: they build on this one.
steps_ok=1
step() {
    local name="$1" line="$2"
    shift 2
    ((steps_ok)) || return 0
    press "$@"
    if wait_row "$prompt_row" "> $line"; then
        vga_dump "$OUT/step-$name.bin" && echo "$name" >> "$OUT/steps.txt"
    else
        steps_ok=0
        vga_dump "$OUT/step-$name.bin" || true
    fi
}

IMG="$OUT/readline-edit.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::
PROMPT="lt-r$(printf '%.0s.' $(seq 66))>"
LETTERS=abcdefghijklmnopqrstuvwxyz
# The line after those 85 letters and Home x, End, 9x Left, y, 2x Left, Delete, Backspace,
# End z, Home Delete (85 characters, the longest on the way is 87: no extra scroll).
EDITED=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvyyzabcdefgz

: > "$OUT/steps.txt"
qemu_start "$IMG" "$OUT"
prompt_row=
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    # The shell's first prompt: a row that is only ">" (trailing blanks cut).
    deadline=$((SECONDS + RUN_TIMEOUT))
    while ((SECONDS < deadline)) && qemu_running; do
        screen_snapshot
        grep -qx '>' "$Q_OUT/screen.txt" && break
        sleep 0.3
    done
    if vga_dump "$OUT/start.bin"; then
        vga_to_text "$OUT/start.bin" > "$OUT/start.txt"
        prompt_row=$(awk '$0 == ">" { r = NR - 1 } END { if (r != "") print r }' "$OUT/start.txt")
    fi
fi
if [ -n "$prompt_row" ]; then
    echo "readline-edit: shell prompt on row $prompt_row"
    echo "prompt" >> "$OUT/steps.txt"
    step s1 "lank.elf ac" l a n k dot e l f spc a c
    step s2 "blank.elf ac" home b
    step s3 "blank.elf abc" end left b
    step s4 "blank.elf ac" left delete
    step s5 "blank.elf a" right backspace
    step s6 "blank.elf a" home left backspace
    step s7 "blank.elf abc" end right b c
    step s8 "blank.elf abc" up down insert pgup pgdn
    press ret
    if ((steps_ok)) && wait_after "> blank.elf" "Process was terminated"; then
        echo "ran" >> "$OUT/steps.txt"
        vga_dump "$OUT/ran.bin" || true
    elif ! ((steps_ok)); then
        # The line is wrong, so no program runs: wait until the shell has printed its prompt.
        vga_dump "$OUT/ran.bin" || true
    fi

    type_line "libtest.elf cursor"
    if wait_screen "libtest cursor: armed" "$RUN_TIMEOUT" && vga_dump "$OUT/cursor-armed.bin"; then
        press a
        if wait_screen "libtest cursor: done" "$RUN_TIMEOUT" && vga_dump "$OUT/cursor.bin"; then
            echo "cursor" >> "$OUT/steps.txt"
        fi
    fi
    wait_after "libtest cursor: done" "Process was terminated" || true

    type_line "libtest.elf readline"
    if wait_screen "libtest readline: armed" "$RUN_TIMEOUT" && vga_dump "$OUT/readline-armed.bin"; then
        press a
        if wait_screen "$PROMPT" "$RUN_TIMEOUT"; then
            # 85 letters: the line wraps after 8 of them and Home moves back over more than a row.
            for ((i = 0; i < 85; i++)); do
                press "${LETTERS:i % 26:1}"
            done
            press home x
            press end left left left left left left left left left y
            press left left delete
            press backspace
            press end z
            press home delete
            if wait_row 24 "${EDITED:8}"; then
                vga_dump "$OUT/readline-typed.bin" && echo "typed" >> "$OUT/steps.txt"
            else
                vga_dump "$OUT/readline-typed.bin" || true
            fi
            press ret
            if wait_screen "libtest readline: done" "$RUN_TIMEOUT" && vga_dump "$OUT/readline-done.bin"; then
                echo "read" >> "$OUT/steps.txt"
            fi
        fi
    fi
    wait_after "libtest readline: done" "Process was terminated" || true
fi
qemu_stop || true

python3 -B - "$OUT" "${prompt_row:--1}" "$PROMPT" "$EDITED" > "$OUT/cells.txt" 2>&1 << 'EOF' || true
import os
import sys

out, prompt_row, prompt = sys.argv[1], int(sys.argv[2]), sys.argv[3] + " "


def cells(name):
    path = os.path.join(out, name)
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    return [(data[2 * i], data[2 * i + 1]) for i in range(2000)]


def text(screen, r):
    return "".join(chr(c) if 32 <= c < 127 else " " for c, _ in screen[r * 80:(r + 1) * 80])


# a. The shell: every step's prompt row, the prompt's cells and the rows above it.
start = cells("start.bin")
steps = [("s1", "lank.elf ac"), ("s2", "blank.elf ac"), ("s3", "blank.elf abc"), ("s4", "blank.elf ac"),
         ("s5", "blank.elf a"), ("s6", "blank.elf a"), ("s7", "blank.elf abc"), ("s8", "blank.elf abc")]
good = 0
for name, line in steps:
    screen = cells("step-%s.bin" % name)
    if start is None or screen is None or prompt_row < 0:
        print("step %s: no VGA page (not run after a wrong step)" % name)
        continue
    p = prompt_row
    row = text(screen, p)
    if row != ("> " + line).ljust(80):
        print("step %s: prompt row reads %r" % (name, row.rstrip()))
        continue
    if screen[p * 80] != start[p * 80] or screen[p * 80 + 1] != start[p * 80 + 1]:
        print("step %s: the prompt's cells changed" % name)
        continue
    if screen[:p * 80] != start[:p * 80]:
        print("step %s: a row above the prompt changed" % name)
        continue
    good += 1
    print("step %s: > %s" % (name, line))
if good == len(steps):
    print("every editing step showed the right line")

# b. Cursor moves: "lt-m:ABxDy|" with R in column 79, then "S".
screen = cells("cursor.bin")
if screen is not None:
    rows = [r for r in range(24) if text(screen, r).startswith("lt-m:A")]
    if rows:
        r = rows[-1]
        print("cursor rows: %r / %r" % (text(screen, r).rstrip(), text(screen, r + 1).rstrip()))
        if text(screen, r) == "lt-m:ABxDy|".ljust(79) + "R" and text(screen, r + 1).rstrip() == "S":
            print("cursor moves placed every character")
    else:
        print("cursor: no lt-m row")

# c. Readline on the bottom row: before Enter, then the "|" right after the line, and the line
# the program read ("lt-r: [<line>]", two rows).
line = sys.argv[4]
screen = cells("readline-typed.bin")
if screen is not None:
    print("typed rows: %r / %r" % (text(screen, 23).rstrip(), text(screen, 24).rstrip()))
    if text(screen, 23) == prompt[:72] + line[:8] and text(screen, 24) == line[8:].ljust(80):
        print("bottom rows show the prompt and the edited line")
screen = cells("readline-done.bin")
if screen is not None:
    read = "lt-r: [%s]" % line
    rows = [r for r in range(24) if (text(screen, r) + text(screen, r + 1)).rstrip() == read]
    if rows:
        print("the program read the edited line")
    if rows and rows[-1] >= 2:
        r = rows[-1]
        print("done rows: %r / %r" % (text(screen, r - 2).rstrip(), text(screen, r - 1).rstrip()))
        if text(screen, r - 2) == prompt[:72] + line[:8] and text(screen, r - 1).rstrip() == line[8:] + "|":
            print("the cursor was right after the line when readline returned")
EOF
sed 's/^/  /' "$OUT/cells.txt"
# The libtest readline step scrolls the cursor step off the screen: use the page kept after it.
if [ -f "$OUT/cursor.bin" ]; then vga_to_text "$OUT/cursor.bin" | sed "s/^> //" > "$OUT/cursor.txt"; else : > "$OUT/cursor.txt"; fi

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell: the first prompt was found"           "$OUT/steps.txt"    "prompt" line
check "shell: every editing step showed the right line" "$OUT/cells.txt" "every editing step showed the right line" line
check "shell: the edited command ran"               "$OUT/steps.txt"    "ran" line
check "cursor: the program finished"                "$OUT/steps.txt"    "cursor" line
check "cursor: every character in its place"        "$OUT/cells.txt"    "cursor moves placed every character" line
check "cursor: every move result right"             "$OUT/cursor.txt"    "lt-m: results 0 0 0 0 -2 -2 -2 -2" line
check "cursor: ebx esi edi ebp kept"                "$OUT/cursor.txt"    "libtest cursor: ebx esi edi ebp kept" line
check "readline: bottom rows before Enter"          "$OUT/cells.txt"    "bottom rows show the prompt and the edited line" line
check "readline: the program read the edited line"  "$OUT/cells.txt"    "the program read the edited line" line
check "readline: cursor at the line's end on return" "$OUT/cells.txt"   "the cursor was right after the line when readline returned" line

# The blank.elf run from step a has scrolled off by now: its checks use the page kept after it.
if [ -f "$OUT/ran.bin" ]; then vga_to_text "$OUT/ran.bin" | sed 's/^> //' > "$OUT/ran.txt"; else : > "$OUT/ran.txt"; fi
check "shell: blank.elf started"                    "$OUT/ran.txt"      "Blank loaded"
check "shell: blank.elf got the argument abc"       "$OUT/ran.txt"      "abc" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish readline-edit
