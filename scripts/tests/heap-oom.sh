#!/usr/bin/env bash
#
# When the kernel heap runs out while it starts a program, the syscall must return an error and
# give back what the failed step allocated, and the system must keep working. Copy bin/os.bin,
# add programs/oomtest to the copy only, and boot it once per case. oomtest.elf leaves an exact
# number of free heap blocks, so one chosen allocation fails while the kernel loads blank.elf
# (a page table in the middle, the task, the page directory, the first page table, the paging
# chunk, the ELF object, the ELF file, the process). It checks the syscall's result and how
# many blocks are still taken afterwards. No program may start from the failed syscall, and then
# blank.elf must start from the shell.
# The script stops after the first failing case. bin/os.bin itself must not change.
#
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for each oomtest run)
#      EXIT_TIMEOUT=15 (for each process to end, and for blank.elf to start)
#      OOM_CASES="table task ..." (run only these cases, in this order)
#      OOM_SUITE=heap-oom (name used for the output directory and the messages; open-oom.sh
#      runs the open cases through this script)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh

SUITE="${OOM_SUITE:-heap-oom}"
OUT="build/tests/$SUITE"
PROGRAM=programs/oomtest/build/oomtest.elf
BLANK_ELF=programs/blank/build/blank.elf
SHELL_ELF=programs/shell/build/shell.elf
# The later page table first: it is the case that crashed before.
CASES="${OOM_CASES:-table task directory first wrapper elfobject elfdata process}"
mkdir -p "$OUT"

# All ELF files must fit in one FAT cluster (64 KiB). oomtest.elf works out the block budget
# from the real size of blank.elf.
[ -f "$PROGRAM" ] || { echo "$SUITE: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
for elf in "$SHELL_ELF" "$BLANK_ELF" "$PROGRAM"; do
    size=$(wc -c < "$elf")
    echo "  $elf: $size bytes"
    [ "$size" -le 65536 ] || { echo "$SUITE: $elf is bigger than 64 KiB" >&2; exit 1; }
done
blank_size=$(wc -c < "$BLANK_ELF")
[ "$QEMU_RAM_MIB" -ge 128 ] || { echo "$SUITE: needs QEMU_RAM_MIB >= 128 (is $QEMU_RAM_MIB)" >&2; exit 1; }

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
cp bin/os.bin "$OUT/$SUITE.bin"
mcopy -i "$OUT/$SUITE.bin" "$PROGRAM" ::

# run_case <case>: one fresh boot. Adds to $failures.
run_case() {
    local name="$1" dir="$OUT/$1" returned=0
    mkdir -p "$dir"
    : > "$dir/screen-before-blank.txt"
    echo "$SUITE: case $name (blank.elf is $blank_size bytes)"
    qemu_start "$OUT/$SUITE.bin" "$dir"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
        type_line "oomtest.elf $name $blank_size"
        # The heap gets nearly full only after a key, once the shell is back at its prompt
        # (a second line starting with ">", the first one is the typed command) and idle.
        if wait_screen "oomtest $name: ready" "$RUN_TIMEOUT" && wait_lines '^>' 2 "$EXIT_TIMEOUT"; then
            qemu_cmd "sendkey g"
            # "armed": the free blocks are set up. It waits for a second key before the syscall,
            # so the line is on screen even if the kernel crashes in the syscall.
            if wait_lines "oomtest $name: (armed|done)" 1 "$RUN_TIMEOUT" &&
                ! grep -qF "oomtest $name: done" "$dir/screen.txt"; then
                qemu_cmd "sendkey r"
            fi
            # The "arg..." cases wait for one more key after the syscall: the failed launch must
            # leave the keyboard with oomtest.
            if wait_lines "oomtest $name: (press a key|done)" 1 "$RUN_TIMEOUT" &&
                ! grep -qF "oomtest $name: done" "$dir/screen.txt"; then
                qemu_cmd "sendkey k"
            fi
            if wait_screen "oomtest $name: done" "$RUN_TIMEOUT"; then
                returned=1
                if grep -qF "oomtest $name: PASS" "$dir/screen.txt" &&
                    wait_lines 'Process was terminated' 1 "$EXIT_TIMEOUT"; then
                    # No other program may have started before the shell starts blank.elf.
                    cp "$dir/screen.txt" "$dir/screen-before-blank.txt"
                    type_line "blank.elf after-$name"
                    if wait_screen "Blank loaded" "$EXIT_TIMEOUT"; then
                        wait_lines 'Process was terminated' 2 "$EXIT_TIMEOUT" || true
                    fi
                fi
            fi
        fi
    fi
    if [ "$returned" = 0 ]; then
        echo "  $SUITE: $name did not return; possible allocation-path crash or hang"
        failures=$((failures + 1))
    fi
    qemu_stop || true

    terminated=$(grep -cF "Process was terminated" "$dir/screen.txt" 2> /dev/null || true)
    echo "terminated=${terminated:-0}" > "$dir/terminated.txt"

    check "$name: kernel reached the end of init (debugcon)"  "$dir/debugcon.log"   "Thats cool"
    check "$name: no kernel panic (debugcon)"                  "$dir/debugcon.log"   "PANIC" absent
    check "$name: shell started (banner on screen)"            "$dir/screen.txt"     "${RORTHOX_NAME} v${RORTHOX_VERSION}"
    check "$name: command echoed by the shell"                 "$dir/screen.txt"     "> oomtest.elf $name $blank_size"
    check "$name: oomtest waited for the key"                  "$dir/screen.txt"     "oomtest $name: ready"
    check "$name: free blocks set up exactly"                  "$dir/screen.txt"     "oomtest $name: armed with free blocks"
    check "$name: no setup failure"                            "$dir/screen.txt"     "oomtest $name: FAIL setup" absent
    check "$name: syscall returned the right error"            "$dir/screen.txt"     "oomtest $name: FAIL result" absent
    check "$name: failed step gave its blocks back"            "$dir/screen.txt"     "oomtest $name: FAIL blocks kept" absent
    check "$name: oomtest passed"                              "$dir/screen.txt"     "oomtest $name: PASS"
    check "$name: oomtest finished"                            "$dir/screen.txt"     "oomtest $name: done"
    check "$name: no program started by the failed syscall"    "$dir/screen-before-blank.txt" "Blank loaded" absent
    case "$name" in
        arg*) check "$name: oomtest still got the keyboard"   "$dir/screen.txt"     "oomtest $name: got the key" ;;
    esac
    check "$name: blank.elf started from the shell afterwards" "$dir/screen.txt"     "Blank loaded"
    check "$name: blank.elf got its argument"                  "$dir/screen.txt"     "after-$name" line
    check "$name: both processes ended"                        "$dir/terminated.txt" "terminated=2" line
}

for name in $CASES; do
    run_case "$name"
    if [ "$failures" -ne 0 ]; then
        echo "$SUITE: stopping after the first failing case ($name)"
        break
    fi
done

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish "$SUITE"
