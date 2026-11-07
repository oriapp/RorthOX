#!/usr/bin/env bash
#
# Backspace at column 0 must go back to the last column of the row above, erase that cell and
# leave the cursor there, so the next character lands on it (K24: the column used to underflow to
# 65535 and the next character went far past the screen).
#
# 1. Host check (scripts/tests/host/tty-screen.c, group "backspace"): the real
#    kernel/arch/i686/tty.c built with the host C compiler, writing to a VGA page mapped at
#    0xB8000 with unmapped pages around it: backspace at (0,0), in a row, at column 0 of a later
#    row, many times across rows, on the last row, and the character after it. Every cell, the
#    row, the column and the VGA cursor are compared with a model.
# 2. QEMU: copy bin/os.bin, add programs/ttytest to the copy only, boot, run "ttytest.elf bs".
#    Once its "armed" line is there, the shell is at its key request after the prompt and the
#    program at its own (scripts/tests/lib/guest-state.sh stops at both). A key makes it print (in
#    one print) a newline, a full row of 80 characters ending in 'Z', a backspace and 'R'; then it
#    waits for a key. The VGA page (raw characters and colours) is read at its key requests before
#    and after: 'R' must be in column 79 of the full row, white on black, and no other cell may
#    have changed. Its last line must start the row below (read at the shell's key request once
#    ttytest has ended).
# bin/os.bin must not change.
#
# Needs python3. Env: HOST_CC=cc   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=build/tests/tty-backspace
PROGRAM=programs/ttytest/build/ttytest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.txt.log "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "tty-backspace: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "tty-backspace: host check of the terminal backspace"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/tty-screen.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" backspace > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log"
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"   "$OUT/host-status.txt" "exit=0" line
check "host check: every backspace case passed"     "$OUT/host-check.log"  "tty-screen backspace host check: 0 of 9 cases failed" line

# The row ttytest prints: "ttytest-bs-", digits (column i shows i % 10), and 'Z' in column 79.
row_start="ttytest-bs-"
row=$row_start
for ((i = ${#row_start}; i < 79; i++)); do row+=$((i % 10)); done

# state <tag> [options]: the guest-state rendezvous at a key request (default: the shell's, the
# shell alone with the keyboard); results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$RUN_TIMEOUT" "$@"
}

IMG="$OUT/tty-backspace.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# The VGA page (characters and colours) is saved at ttytest's key requests: everything it printed
# before is on it, and the shell already sits at its own key request after its prompt, so nothing
# else prints.
echo "tty-backspace: run ttytest.elf bs"
: > "$OUT/steps.txt"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot owner=first save="$OUT/shell.txt"; then
    type_line "ttytest.elf bs"
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:ttytest bs: armed" &&
        state shell at=first tasks=2 owner=last shell="$OUT/shell.txt" &&
        state armed at=last tasks=2 owner=last shell="$OUT/shell.txt" memory="b8000,fa0,$OUT/before.bin"; then
        echo "armed" >> "$OUT/steps.txt"
        qemu_cmd "sendkey a"
        if wait_screen_lines "$RUN_TIMEOUT" "regex:$row.*" &&
            state row at=last tasks=2 owner=last shell="$OUT/shell.txt" memory="b8000,fa0,$OUT/after.bin"; then
            echo "row printed" >> "$OUT/steps.txt"
            qemu_cmd "sendkey a"
            if wait_screen_lines "$RUN_TIMEOUT" "prompt:ttytest bs: done" &&
                state end owner=first shell="$OUT/shell.txt" memory="b8000,fa0,$OUT/end.bin"; then
                echo "done" >> "$OUT/steps.txt"
            fi
        fi
    fi
fi
qemu_stop || true

# Compare the VGA page before and after the row, cell by cell (character and colour).
python3 -B - "$OUT" "$row" > "$OUT/cells.txt" 2>&1 << 'EOF' || true
import os
import sys

out, row = sys.argv[1], sys.argv[2]


def cells(name):
    path = os.path.join(out, name)
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    return [(data[2 * i], data[2 * i + 1]) for i in range(2000)]


def text(screen, r):
    return "".join(chr(c) if 32 <= c < 127 else " " for c, _ in screen[r * 80:(r + 1) * 80])


before, after, end = cells("before.bin"), cells("after.bin"), cells("end.bin")
if before is None or after is None:
    print("no VGA dumps")
    sys.exit(0)
rows = [r for r in range(25) if text(after, r)[:79] == row]
old_rows = [r for r in range(25) if text(before, r)[:79] == row]
print("full rows found: %s (before: %s)" % (rows, old_rows))
if len(rows) != 1 or old_rows:
    sys.exit(0)
r = rows[0]
print("full row %d found once" % r)
if r >= 1:
    print("the full row is below the first row")
c, a = after[r * 80 + 79]
print("column 79 of the full row: %r colour 0x%02x" % (chr(c), a))
if c == ord("R") and a == 0x0F:
    print("R replaced Z in column 79, white on black")
bad = [i for i in range(79) if after[r * 80 + i][1] != 0x0F]
if not bad:
    print("columns 0-78 of the full row are white on black")
changed = [i for i in range(2000) if before[i] != after[i] and i // 80 != r]
for i in changed[:5]:
    print("cell (%d,%d) changed: %r/0x%02x -> %r/0x%02x" % (i % 80, i // 80, chr(before[i][0]), before[i][1],
                                                             chr(after[i][0]), after[i][1]))
if not changed:
    print("no cell outside the full row changed")
if end is not None and r + 1 < 25:
    print("row below at the end: %r" % text(end, r + 1).rstrip())
    if text(end, r + 1).startswith("ttytest bs: done"):
        print("the next line starts the row below")
EOF
sed 's/^/  /' "$OUT/cells.txt"

check "kernel reached the end of init (debugcon)"      "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                      "$OUT/debugcon.log" "PANIC" absent
check "ttytest armed with the shell idle"               "$OUT/steps.txt"    "armed" line
check "ttytest printed the full row"                    "$OUT/steps.txt"    "row printed" line
check "ttytest ended"                                   "$OUT/steps.txt"    "done" line
check "the full row is on the screen once"              "$OUT/cells.txt"    "the full row is below the first row" line
check "backspace + R: R in column 79 of the full row"   "$OUT/cells.txt"    "R replaced Z in column 79, white on black" line
check "the rest of the full row is intact"              "$OUT/cells.txt"    "columns 0-78 of the full row are white on black" line
check "no other cell changed"                           "$OUT/cells.txt"    "no cell outside the full row changed" line
check "the next line starts at column 0 of the row below" "$OUT/cells.txt"  "the next line starts the row below" line
check "kernel symbols for the rendezvous found"         "$OUT/symbols.txt"  "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the task's getkey"                "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"             "$f" "PASS $1_list_consistent" line
    check "$2: number of tasks"                             "$f" "PASS $1_task_count" line
    check "$2: keyboard owner"                              "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot  "boot: the shell alone"
stop_checks shell "armed: the shell after its prompt, ttytest has the keys"
stop_checks armed "armed: ttytest waits for a key"
stop_checks row   "row: ttytest waits for a key"
stop_checks end   "end: ttytest gone, the shell alone"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish tty-backspace
