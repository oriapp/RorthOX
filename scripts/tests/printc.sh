#!/usr/bin/env bash
#
# printc(str, colour) must print str in the given foreground colour (K19: the user wrapper
# passed the string twice, and the kernel copied the second argument as a string and ran atoi()
# on it, so the colour came from the text).
#
# 1. Host check (scripts/tests/host/printc.c): the real syscall handler in kernel/isr80h/io.c with
#    stubbed task stack, string copy and terminal. Colours 0-15 copy the message once (never the
#    colour) and print it in that colour; colours 16, 255, 256, 0x7fffffff, 0x80000000 and
#    0xffffffff give -EINVARG and copy and print nothing; a failing copy gives its error back
#    and prints nothing. The same file's getkey handler returns every key value whole (bytes and
#    the special keys above 255, zero-extended), and the cursor-move handler passes stack item 0
#    once as a signed int and returns the terminal's result, or the read error without calling
#    the terminal.
# 2. QEMU: copy bin/os.bin, add programs/libtest to the copy only, boot, run
#    "libtest.elf printc". Once its "armed" line is there, the shell is at its key request after
#    the prompt and the program at its own (scripts/tests/lib/guest-state.sh stops at both). A
#    key makes it print one row: "lt-c:" (print), "#0" ... "#f" through the real printc wrapper
#    in colours 0-15, printc "XX" with the invalid colours, then "|end" (print). The VGA page (raw
#    characters and colours) is read at the program's next key request, and the program and the
#    same shell must still be the only tasks: every "#n" must be in colour n on black,
#    the print() text around it white on black, no "X" printed and "|end" right after "#f" (the
#    refused calls didn't move the cursor). The program checks every printc result (0, or
#    -EINVARG for the invalid colours) and prints a FAIL line for each wrong one. Every call
#    runs with marker values in ebx, esi, edi and ebp (cdecl callee-saved registers), and the
#    program prints "libtest printc: ebx esi edi ebp kept" only if every call gave them back.
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

OUT=${RORTHOX_TEST_ROOT:-build/tests}/printc
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.txt.log "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "printc: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "printc: host check of the printc syscall handler"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/printc.c kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 40
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: getkey cases ran"               "$OUT/host-check.log"  "getkey: every key value comes back whole" line
check "host check: cursor move cases ran"          "$OUT/host-check.log"  "cursor move: item 0 passed once as a signed int, an unreadable item calls nothing" line
check "host check: every handler case passed"      "$OUT/host-check.log"  "printc host check: 0 of 50 cases failed" line

# state <tag> [options]: the guest-state rendezvous at a key request (default: the shell's, the
# shell alone with the keyboard); results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$RUN_TIMEOUT" "$@"
}

IMG="$OUT/printc.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# The VGA page (characters and colours) is saved at libtest's key requests: everything it printed
# before is on it, and the shell already sits at its own key request after its prompt, so nothing
# else prints.
echo "printc: run libtest.elf printc"
: > "$OUT/steps.txt"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot owner=first save="$OUT/shell.txt"; then
    type_line "libtest.elf printc"
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:libtest printc: armed" &&
        state shell at=first tasks=2 owner=last shell="$OUT/shell.txt" &&
        state armed at=last tasks=2 owner=last shell="$OUT/shell.txt" memory="b8000,fa0,$OUT/before.bin"; then
        echo "armed" >> "$OUT/steps.txt"
        qemu_cmd "sendkey a"
        if wait_screen_lines "$RUN_TIMEOUT" "prompt:libtest printc: done" &&
            state printed at=last tasks=2 owner=last shell="$OUT/shell.txt" memory="b8000,fa0,$OUT/after.bin"; then
            echo "printed" >> "$OUT/steps.txt"
            qemu_cmd "sendkey a"
            if state end owner=first shell="$OUT/shell.txt"; then
                echo "ended" >> "$OUT/steps.txt"
            fi
        fi
    fi
fi
qemu_stop || true

# Check the colour row cell by cell (character and colour).
python3 -B - "$OUT" > "$OUT/cells.txt" 2>&1 << 'EOF' || true
import os
import sys

out = sys.argv[1]


def cells(name):
    path = os.path.join(out, name)
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    return [(data[2 * i], data[2 * i + 1]) for i in range(2000)]


def text(screen, r):
    return "".join(chr(c) if 32 <= c < 127 else " " for c, _ in screen[r * 80:(r + 1) * 80])


before, after = cells("before.bin"), cells("after.bin")
if before is None or after is None:
    print("no VGA dumps")
    sys.exit(0)
with open(os.path.join(out, "after.txt"), "w") as f:
    f.write("\n".join(text(after, r).rstrip() for r in range(25)) + "\n")

rows = [r for r in range(25) if text(after, r).startswith("lt-c:")]
old_rows = [r for r in range(25) if text(before, r).startswith("lt-c:")]
print("colour rows found: %s (before: %s)" % (rows, old_rows))
if len(rows) != 1 or old_rows:
    sys.exit(0)
r = rows[0]
row = after[r * 80:(r + 1) * 80]
print("the colour row is on the screen once")
print("row %d: %r" % (r, text(after, r).rstrip()))

# What the row must hold: (column, character, colour).
want = [(i, ch, 0x0F) for i, ch in enumerate("lt-c:")]
for c in range(16):
    want.append((5 + 2 * c, "#", c))
    want.append((6 + 2 * c, "0123456789abcdef"[c], c))
want += [(37 + i, ch, 0x0F) for i, ch in enumerate("|end")]
bad = [(i, ch, a, chr(row[i][0]), row[i][1]) for i, ch, a in want if row[i] != (ord(ch), a)]
for i, ch, a, got_ch, got_a in bad[:8]:
    print("column %d: %r/0x%02x, expected %r/0x%02x" % (i, got_ch, got_a, ch, a))
if not bad:
    print("every #n in colour n on black, the print() text white on black")
colours = sorted(set(row[5 + 2 * c][1] for c in range(16)))
print("colours used for #0-#f: %s" % " ".join("0x%02x" % a for a in colours))
rest = [i for i in range(41, 80) if row[i] != before[r * 80 + i]]
if not rest:
    print("nothing written after |end on the row")
x_before = sum(1 for ch, _ in before if ch == ord("X"))
x_after = sum(1 for ch, _ in after if ch == ord("X"))
print("X characters on the screen: %d before, %d after" % (x_before, x_after))
if x_after == x_before:
    print("no XX printed for the invalid colours")
lines = [k for k in range(r + 1, 25) if text(after, k).startswith("libtest printc: results checked")]
if lines and all(after[lines[0] * 80 + i][1] == 0x0F for i in range(31)):
    print("the next print() line is white on black")
EOF
sed 's/^/  /' "$OUT/cells.txt"

check "kernel reached the end of init (debugcon)"        "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                        "$OUT/debugcon.log" "PANIC" absent
check "libtest armed with the shell idle"                 "$OUT/steps.txt"    "armed" line
check "libtest printed the colour row and its results"    "$OUT/steps.txt"    "printed" line
check "libtest ended"                                     "$OUT/steps.txt"    "ended" line
check "kernel symbols for the rendezvous found"           "$OUT/symbols.txt"  "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the task's getkey"                "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"             "$f" "PASS $1_list_consistent" line
    check "$2: number of tasks"                             "$f" "PASS $1_task_count" line
    check "$2: keyboard owner"                              "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot    "boot: the shell alone"
stop_checks shell   "armed: the shell after its prompt, libtest has the keys"
stop_checks armed   "armed: libtest waits for a key"
stop_checks printed "printed: libtest waits for a key"
stop_checks end     "end: libtest gone, the shell alone"
check "the colour row is on the screen once"              "$OUT/cells.txt"    "the colour row is on the screen once" line
check "#0-#f in colours 0-15, print() text white"         "$OUT/cells.txt"    "every #n in colour n on black, the print() text white on black" line
check "invalid colours printed nothing, cursor unchanged" "$OUT/cells.txt"    "no XX printed for the invalid colours" line
check "nothing else on the colour row"                    "$OUT/cells.txt"    "nothing written after |end on the row" line
check "the next print() line is white on black"           "$OUT/cells.txt"    "the next print() line is white on black" line
check "every printc result was checked"                   "$OUT/after.txt"    "libtest printc: results checked"
check "printc returned 0 for 0-15 and -EINVARG for the invalid colours" "$OUT/after.txt" "libtest printc: FAIL" absent
check "printc kept ebx, esi, edi and ebp (cdecl)"         "$OUT/after.txt"    "libtest printc: ebx esi edi ebp kept"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish printc
