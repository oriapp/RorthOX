#!/usr/bin/env bash
#
# heap_free() must ignore pointers that don't start a taken run of blocks (NULL, outside the heap,
# unaligned, the middle of a run, a second free). Copy bin/os.bin, add programs/heapfree to the
# copy only, boot it and run heapfree.elf twice from the shell. heapfree.elf runs the kernel's
# own heap.c on a private table with guard bytes around it, and also calls the real free(NULL)
# syscall. bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for each run)
#      EXIT_TIMEOUT=15 (for the process to end after its last line)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

OUT=build/tests/heap-free
PROGRAM=programs/heapfree/build/heapfree.elf
SHELL_ELF=programs/shell/build/shell.elf
mkdir -p "$OUT"

# Both ELF files must fit in one FAT cluster (64 KiB).
[ -f "$PROGRAM" ] || { echo "heap-free: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
for elf in "$SHELL_ELF" "$PROGRAM"; do
    size=$(wc -c < "$elf")
    echo "  $elf: $size bytes"
    [ "$size" -le 65536 ] || { echo "heap-free: $elf is bigger than 64 KiB" >&2; exit 1; }
done

# wait_count <text> <count> <seconds>: like wait_screen, until <text> is on <count> screen lines.
wait_count() {
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
cp bin/os.bin "$OUT/heap-free.bin"
mcopy -i "$OUT/heap-free.bin" "$PROGRAM" ::

echo "heap-free: boot a copy of bin/os.bin with heapfree.elf and run it twice"
qemu_start "$OUT/heap-free.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "heapfree.elf one"
    if wait_screen "heapfree one: done" "$RUN_TIMEOUT"; then
        # A second run only after a clean first one: failure lines would fill the screen.
        if grep -qF "heapfree one: PASS" "$OUT/screen.txt" &&
            wait_count "Process was terminated" 1 "$EXIT_TIMEOUT"; then
            type_line "heapfree.elf two"
            if wait_screen "heapfree two: done" "$RUN_TIMEOUT"; then
                wait_count "Process was terminated" 2 "$EXIT_TIMEOUT" || true
            else
                echo "  second run did not complete"
                failures=$((failures + 1))
            fi
        fi
    else
        echo "  first run did not complete; possible heap corruption or hang"
        failures=$((failures + 1))
    fi
fi
qemu_stop || true

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
terminated=$(grep -cF "Process was terminated" "$OUT/screen.txt" 2> /dev/null || true)
echo "terminated=${terminated:-0}" > "$OUT/terminated.txt"

check "kernel reached the end of init (debugcon)"  "$OUT/debugcon.log"  "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log"  "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen.txt"    "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "first command echoed by the shell"          "$OUT/screen.txt"    "> heapfree.elf one"
check "first run: free(NULL) syscall returned"      "$OUT/screen.txt"    "heapfree one: free(NULL) returned"
check "heap: NULL free ignored"                     "$OUT/screen.txt"    "heapfree: FAIL NULL free changed guard" absent
check "heap: free below the heap ignored"           "$OUT/screen.txt"    "heapfree: FAIL free below heap" absent
check "heap: free past the heap end ignored"        "$OUT/screen.txt"    "heapfree: FAIL free past heap end" absent
check "heap: unaligned free ignored"                "$OUT/screen.txt"    "heapfree: FAIL unaligned free" absent
check "heap: middle block free ignored"             "$OUT/screen.txt"    "heapfree: FAIL middle block free" absent
check "heap: valid free releases only its run"      "$OUT/screen.txt"    "heapfree: FAIL valid free" absent
check "heap: double free ignored"                   "$OUT/screen.txt"    "heapfree: FAIL double free" absent
check "heap: last block and exact fit"              "$OUT/screen.txt"    "heapfree: FAIL last block" absent
check "heap: exact fit"                             "$OUT/screen.txt"    "heapfree: FAIL exact fit" absent
check "heap: zero and huge sizes refused"           "$OUT/screen.txt"    "heapfree: FAIL zero or huge size" absent
check "heapfree reported no failure"                "$OUT/screen.txt"    "heapfree: FAIL" absent
check "first run passed"                            "$OUT/screen.txt"    "heapfree one: PASS"
check "first run finished"                          "$OUT/screen.txt"    "heapfree one: done"
check "second command echoed by the shell"         "$OUT/screen.txt"    "heapfree.elf two"
check "second run: free(NULL) syscall returned"     "$OUT/screen.txt"    "heapfree two: free(NULL) returned"
check "second run passed"                           "$OUT/screen.txt"    "heapfree two: PASS"
check "second run finished"                         "$OUT/screen.txt"    "heapfree two: done"
check "both processes ended"                        "$OUT/terminated.txt" "terminated=2" line
check "bin/os.bin not changed by the test"          "$OUT/image.txt"     "unchanged" line
finish heap-free
