#!/usr/bin/env bash
#
# The kernel builds "0:/" + program name in a RORTHOX_MAX_PATH byte buffer when it starts a program.
# A name that doesn't fit must be refused before anything is written, and must not start a program.
# Copy bin/os.bin, add programs/pathtest to the copy only, and boot it. pathtest.elf asks
# syscall 7 and syscall 6 to start "blank.elf" followed by slashes (the path parser stops at the
# first empty part, so such a name opens blank.elf if the kernel takes it), 1, 2, 3, 4 characters
# too long and much longer. Syscalls 7 and 6 must return -EINVARG, a name that just fits must still reach
# the loader, and blank.elf must not run. Then the shell starts blank.elf by a name that just
# fits (blank.elf and slashes), and by its plain name. After pathtest.elf and after each blank.elf,
# scripts/tests/lib/guest-state.sh stops the guest at the shell's next key request: the shell is
# alone in the task list (so no program pathtest started is still running) and is the same task
# with the keyboard. bin/os.bin itself must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for pathtest.elf)
#      EXIT_TIMEOUT=30 (for each process to end, and for blank.elf to start)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=build/tests/syscall-path
PROGRAM=programs/pathtest/build/pathtest.elf
# Longest name that fits after "0:/" with its terminator (RORTHOX_MAX_PATH is 108).
FITS=104
mkdir -p "$OUT"
rm -f "$OUT"/state-*.txt "$OUT"/state-*.txt.log "$OUT"/shell.txt "$OUT"/screen-*.txt
[ -f "$PROGRAM" ] || { echo "syscall-path: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
grep -q '^#define RORTHOX_MAX_PATH 108$' kernel/config.h ||
    { echo "syscall-path: RORTHOX_MAX_PATH changed, update FITS" >&2; exit 1; }

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone
# with the keyboard; results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$EXIT_TIMEOUT" owner=first "$@"
}

echo "syscall-path: kernel symbols"
guest_state_setup "$OUT"
image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/syscall-path.bin"
mcopy -i "$OUT/syscall-path.bin" "$PROGRAM" ::
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# blank.elf followed by slashes, FITS characters in all.
fits_name="blank.elf"
while [ "${#fits_name}" -lt "$FITS" ]; do fits_name="$fits_name/"; done

QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/syscall-path.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot save="$OUT/shell.txt" screen="$OUT/screen-boot.txt"; then
    type_line "pathtest.elf"
    # The shell alone: pathtest has ended and no blank.elf it might have started is still running,
    # so anything such a blank.elf printed is on this screen.
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:pathtest: done" &&
        state pathtest shell="$OUT/shell.txt" screen="$OUT/screen-pathtest.txt"; then
        type_line "$fits_name fits"
        if wait_screen_lines "$EXIT_TIMEOUT" "prompt:fits" && state fits shell="$OUT/shell.txt" screen="$OUT/screen-fits.txt"; then
            type_line "blank.elf after"
            wait_screen_lines "$EXIT_TIMEOUT" "prompt:after" &&
                state after shell="$OUT/shell.txt" screen="$OUT/screen-end.txt" || true
        fi
    fi
fi
qemu_stop || true
[ -f "$OUT/screen-pathtest.txt" ] || : > "$OUT/screen-pathtest.txt"
# Counted on the screen saved at the last rendezvous: nothing is printing or scrolling there.
loaded=$(grep -cF "Blank loaded" "$OUT/screen-end.txt" 2> /dev/null || true)
echo "loaded=${loaded:-0}" > "$OUT/loaded.txt"

check "kernel reached the end of init (debugcon)"        "$OUT/debugcon.log"        "Thats cool"
check "no kernel panic (debugcon)"                        "$OUT/debugcon.log"        "PANIC" absent
check "shell started (banner on screen)"                  "$OUT/screen-boot.txt"     "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "pathtest.elf ran"                                  "$OUT/screen-pathtest.txt" "> pathtest.elf"
check "too long names refused, a fitting one loaded"      "$OUT/screen-pathtest.txt" "pathtest: FAIL" absent
check "pathtest passed"                                   "$OUT/screen-pathtest.txt" "pathtest: PASS"
check "pathtest finished"                                 "$OUT/screen-pathtest.txt" "pathtest: done"
check "too long names did not start blank.elf"            "$OUT/screen-pathtest.txt" "Blank loaded" absent
check "blank.elf started by a name of $FITS characters"   "$OUT/screen-fits.txt"     "fits" line
check "blank.elf started by its plain name afterwards"    "$OUT/screen-end.txt"      "after" line
check "blank.elf ran exactly twice"                       "$OUT/loaded.txt"          "loaded=2" line
check "kernel symbols for the rendezvous found"           "$OUT/symbols.txt"         "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"          "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                        "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"           "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot     "boot"
stop_checks pathtest "pathtest ended, nothing it started still runs"
stop_checks fits     "blank.elf by the $FITS-character name ended"
stop_checks after    "blank.elf by its plain name ended"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish syscall-path
