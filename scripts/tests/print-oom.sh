#!/usr/bin/env bash
#
# The print and printc syscalls copy the user's text through a checked copy that needs no kernel
# heap, so with the heap full they must still print it and return 0, and never print what was
# left in a kernel buffer. Copy bin/os.bin, add programs/printoom to the
# copy only, and boot it. printoom.elf leaves one free block, prints a line, takes the last block,
# calls print and printc, and checks they return 0 and their text shows. No second copy of the
# line before may reach the screen. Then blank.elf must start from the shell.
# bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for printoom.elf)
#      EXIT_TIMEOUT=30 (for each process to end, and for blank.elf to start)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-30}"
. ./scripts/qemu-lib.sh

OUT=build/tests/print-oom
PROGRAM=programs/printoom/build/printoom.elf
mkdir -p "$OUT"
[ -f "$PROGRAM" ] || { echo "print-oom: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
[ "$QEMU_RAM_MIB" -ge 128 ] || { echo "print-oom: needs QEMU_RAM_MIB >= 128 (is $QEMU_RAM_MIB)" >&2; exit 1; }

# wait_lines <regex> <count> <seconds>: wait until <count> screen lines match <regex>.
wait_lines() {
    local regex="$1" count="$2" deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for /$regex/ x$count"; return 1; }
        screen_snapshot
        n=$(grep -cE -- "$regex" "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "${n:-0}" -ge "$count" ]; then
            echo "  /$regex/ x$count on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for /$regex/ x$count after $3 s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/print-oom.bin"
mcopy -i "$OUT/print-oom.bin" "$PROGRAM" ::

qemu_start "$OUT/print-oom.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "printoom.elf"
    # The heap gets full only after a key, once the shell is back at its prompt (a second line
    # starting with ">", the first one is the typed command) and idle.
    if wait_screen "printoom: ready" "$RUN_TIMEOUT" && wait_lines '^>' 2 "$EXIT_TIMEOUT"; then
        qemu_cmd "sendkey g"
        if wait_screen "printoom: done" "$RUN_TIMEOUT" && wait_lines 'Process was terminated' 1 "$EXIT_TIMEOUT"; then
            cp "$OUT/screen.txt" "$OUT/screen-printoom.txt"
            type_line "blank.elf after"
            wait_lines 'Process was terminated' 2 "$EXIT_TIMEOUT" || true
        fi
    fi
fi
qemu_stop || true
[ -f "$OUT/screen-printoom.txt" ] || : > "$OUT/screen-printoom.txt"
good=$(grep -cF "printoom: last good line" "$OUT/screen-printoom.txt" 2> /dev/null || true)
echo "good=${good:-0}" > "$OUT/good.txt"

check "kernel reached the end of init (debugcon)"    "$OUT/debugcon.log"        "Thats cool"
check "no kernel panic (debugcon)"                    "$OUT/debugcon.log"        "PANIC" absent
check "shell started (banner on screen)"              "$OUT/screen.txt"          "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "printoom.elf waited for the key"               "$OUT/screen-printoom.txt" "printoom: ready"
check "no setup failure"                              "$OUT/screen-printoom.txt" "printoom: FAIL setup" absent
check "print returned 0 with the heap full"           "$OUT/screen-printoom.txt" "printoom: FAIL print result" absent
check "printc returned 0 with the heap full"          "$OUT/screen-printoom.txt" "printoom: FAIL printc result" absent
check "print text shown"                              "$OUT/screen-printoom.txt" "print got through"
check "printc text shown"                             "$OUT/screen-printoom.txt" "printc got through"
check "old buffer contents not printed again"         "$OUT/good.txt"            "good=1" line
check "printoom passed"                               "$OUT/screen-printoom.txt" "printoom: PASS"
check "printoom finished"                             "$OUT/screen-printoom.txt" "printoom: done"
check "blank.elf started from the shell afterwards"   "$OUT/screen.txt"          "Blank loaded"
check "blank.elf got its argument"                    "$OUT/screen.txt"          "after" line

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish print-oom
