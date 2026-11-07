#!/usr/bin/env bash
#
# strtok(str, ...) must start over when it gets a new string, even when the last string isn't
# finished (K25: the user library's strtok kept going through the old string).
#
# 1. Host check (scripts/tests/host/strtok.c): the real programs/stdlib/src/string.c built with
#    the host C compiler, every symbol prefixed with "user_" so it cannot clash with the host C
#    library. Fixed steps (a new string after an unfinished one, after a one-token string, after
#    an empty string, leading/repeated/trailing delimiters, calls after the end) and many random
#    calls compared with a model of strtok: token start and the bytes of every string.
# 2. QEMU: copy bin/os.bin, add programs/libtest to the copy only, boot, run
#    "libtest.elf strtok": the same fixed steps in a user program, under the task's paging. Its
#    summary row is read at the shell's next key request with libtest gone and the same shell
#    alone (scripts/tests/lib/guest-state.sh).
# bin/os.bin must not change.
#
# Needs python3. Env: HOST_CC=cc   OBJCOPY=objcopy   BOOT_TIMEOUT=60 (seconds to wait for the shell)
#      RUN_TIMEOUT=30 (for each step)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
OBJCOPY="${OBJCOPY:-objcopy}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=build/tests/strtok
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.o "$OUT"/*.txt "$OUT"/*.txt.log "$OUT"/*.bin "$OUT"/screen.ppm "$OUT"/debugcon.log "$OUT"/monitor.log
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "strtok: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)

echo "strtok: host check of the user library's strtok"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -ffreestanding -fno-builtin -fno-stack-protector -w \
        -I programs/stdlib/src -I build/config \
        -c programs/stdlib/src/string.c -o "$OUT/user-string.o" > "$OUT/host-build.log" 2>&1 &&
    "$OBJCOPY" --prefix-symbols=user_ "$OUT/user-string.o" "$OUT/user-string-prefixed.o" >> "$OUT/host-build.log" 2>&1 &&
    "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -w -I scripts/tests/host \
        scripts/tests/host/strtok.c "$OUT/user-string-prefixed.o" \
        -o "$OUT/host-check" >> "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 30
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: fixed steps ran"                "$OUT/host-check.log"  "fixed steps: 35 checks" line
check "host check: random calls ran"               "$OUT/host-check.log"  "random calls: 19200 calls, 38400 checks" line
check "host check: every check passed"             "$OUT/host-check.log"  "strtok host check: 0 of 38435 checks failed" line

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone
# with the keyboard; results in $OUT/state-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$RUN_TIMEOUT" owner=first "$@"
}

IMG="$OUT/strtok.bin"
cp bin/os.bin "$IMG"
mcopy -i "$IMG" "$PROGRAM" ::
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "strtok: run libtest.elf strtok"
: > "$OUT/steps.txt"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot save="$OUT/shell.txt"; then
    type_line "libtest.elf strtok"
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:libtest strtok: start"; then
        echo "started" >> "$OUT/steps.txt"
        # The summary row, then the shell alone again: libtest has ended and printed nothing more.
        if wait_screen_lines "$RUN_TIMEOUT" "regex:(> )?libtest strtok: [0-9]+ of [0-9]+ checks failed" &&
            state end shell="$OUT/shell.txt" screen="$OUT/screen-end.txt"; then
            echo "ended" >> "$OUT/steps.txt"
        fi
    fi
fi
qemu_stop || true

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "libtest strtok started"                      "$OUT/steps.txt"    "started" line
check "libtest strtok ended"                        "$OUT/steps.txt"    "ended" line
[ -f "$OUT/screen-end.txt" ] || : > "$OUT/screen-end.txt"
check "user strtok: every step passed"              "$OUT/screen-end.txt" "libtest strtok: 0 of 27 checks failed"
check "user strtok: no failed step"                 "$OUT/screen-end.txt" "libtest strtok: FAIL" absent
check "kernel symbols for the rendezvous found"     "$OUT/symbols.txt"  "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"          "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"        "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                        "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"           "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot "boot"
stop_checks end  "libtest ended"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish strtok
