#!/usr/bin/env bash
#
# The kernel builds "0:/" + program name in a RORTHOX_MAX_PATH byte buffer when it starts a program.
# A name that doesn't fit must be refused before anything is written, and must not start a program.
# Copy bin/os.bin, add programs/pathtest to the copy only, and boot it. pathtest.elf asks
# syscall 7 and syscall 6 to start "blank.elf" followed by slashes (the path parser stops at the
# first empty part, so such a name opens blank.elf if the kernel takes it), 1, 2, 3, 4 characters
# too long and much longer. Syscall 7 must return -EINVARG, a name that just fits must still reach
# the loader, and blank.elf must not run. Then the shell starts blank.elf by a name that just
# fits (blank.elf and slashes), and by its plain name. bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for pathtest.elf)
#      EXIT_TIMEOUT=30 (for each process to end, and for blank.elf to start)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh

OUT=build/tests/syscall-path
PROGRAM=programs/pathtest/build/pathtest.elf
# Longest name that fits after "0:/" with its terminator (RORTHOX_MAX_PATH is 108).
FITS=104
mkdir -p "$OUT"
[ -f "$PROGRAM" ] || { echo "syscall-path: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
grep -q '^#define RORTHOX_MAX_PATH 108$' kernel/config.h ||
    { echo "syscall-path: RORTHOX_MAX_PATH changed, update FITS" >&2; exit 1; }

# wait_lines <text> <count> <seconds>: wait until <count> screen lines contain <text>.
wait_lines() {
    local text="$1" count="$2" deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for \"$text\" x$count"; return 1; }
        screen_snapshot
        n=$(grep -cF -- "$text" "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "${n:-0}" -ge "$count" ]; then
            echo "  \"$text\" x$count on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$text\" x$count after $3 s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/syscall-path.bin"
mcopy -i "$OUT/syscall-path.bin" "$PROGRAM" ::

# blank.elf followed by slashes, FITS characters in all.
fits_name="blank.elf"
while [ "${#fits_name}" -lt "$FITS" ]; do fits_name="$fits_name/"; done

qemu_start "$OUT/syscall-path.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "pathtest.elf"
    if wait_screen "pathtest: done" "$RUN_TIMEOUT" && wait_lines "Process was terminated" 1 "$EXIT_TIMEOUT"; then
        # Anything blank.elf would print by now is on screen: pathtest has ended.
        cp "$OUT/screen.txt" "$OUT/screen-pathtest.txt"
        type_line "$fits_name fits"
        if wait_lines "Process was terminated" 2 "$EXIT_TIMEOUT"; then
            type_line "blank.elf after"
            wait_lines "Process was terminated" 3 "$EXIT_TIMEOUT" || true
        fi
    fi
fi
qemu_stop || true
[ -f "$OUT/screen-pathtest.txt" ] || : > "$OUT/screen-pathtest.txt"
loaded=$(grep -cF "Blank loaded" "$OUT/screen.txt" 2> /dev/null || true)
echo "loaded=${loaded:-0}" > "$OUT/loaded.txt"

check "kernel reached the end of init (debugcon)"        "$OUT/debugcon.log"        "Thats cool"
check "no kernel panic (debugcon)"                        "$OUT/debugcon.log"        "PANIC" absent
check "shell started (banner on screen)"                  "$OUT/screen.txt"          "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "pathtest.elf ran"                                  "$OUT/screen-pathtest.txt" "> pathtest.elf"
check "too long names refused, a fitting one loaded"      "$OUT/screen-pathtest.txt" "pathtest: FAIL" absent
check "pathtest passed"                                   "$OUT/screen-pathtest.txt" "pathtest: PASS"
check "pathtest finished"                                 "$OUT/screen-pathtest.txt" "pathtest: done"
check "too long names did not start blank.elf"            "$OUT/screen-pathtest.txt" "Blank loaded" absent
check "blank.elf started by a name of $FITS characters"   "$OUT/screen.txt"          "fits" line
check "blank.elf started by its plain name afterwards"    "$OUT/screen.txt"          "after" line
check "blank.elf ran exactly twice"                       "$OUT/loaded.txt"          "loaded=2" line

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish syscall-path
