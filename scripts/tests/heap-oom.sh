#!/usr/bin/env bash
#
# When the kernel heap runs out while it starts a program, the syscall must return an error and
# give back what the failed step allocated, and the system must keep working. Copy bin/os.bin,
# add programs/oomtest to the copy only, and boot it once per case. oomtest.elf leaves an exact
# number of free heap blocks, so one chosen allocation fails while the kernel loads blank.elf
# (a page table in the middle, the second page table, the paging chunk, the ELF object, the ELF
# file, the process). It checks the syscall's result and how many blocks are still taken
# afterwards. No program may start from the failed syscall, and then blank.elf must start from
# the shell.
# The heap table is kernel memory, so oomtest.elf must not read it. It counts free blocks in copies
# of the table that a debugger
# writes into the program through QEMU's gdbstub (scripts/tests/lib/heap-snapshot.sh). The debugger
# only answers in oomtest's own task (ring 3, its own page directory), and it checks that the heap
# table is still the last copy when oomtest makes the syscall. Every copy must be acknowledged, so a
# case can't pass without the debugger.
# Between the steps scripts/tests/lib/guest-state.sh stops the guest at key requests (never while the
# heap debugger is attached): the heap gets nearly full only once the shell is back at its own key
# request (done printing, nothing more to allocate) and oomtest has the keyboard; an "arg..." case
# must still be oomtest's after the failed syscall with no new task; after oomtest and after blank.elf
# the shell is alone, the same task, and has the keyboard.
# The script stops after the first failing case. bin/os.bin itself must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)   RUN_TIMEOUT=60 (for each oomtest run)
#      EXIT_TIMEOUT=15 (for each process to end, for blank.elf to start, and for each rendezvous)
#      OOM_CASES="table second ..." (run only these cases, in this order)
#      OOM_SUITE=heap-oom (name used for the output directory and the messages; open-oom.sh
#      runs the open cases through this script)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
EXIT_TIMEOUT="${EXIT_TIMEOUT:-15}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/heap-snapshot.sh
. ./scripts/tests/lib/guest-state.sh

SUITE="${OOM_SUITE:-heap-oom}"
OUT="${RORTHOX_TEST_ROOT:-build/tests}/$SUITE"
PROGRAM=programs/oomtest/build/oomtest.elf
BLANK_ELF=programs/blank/build/blank.elf
SHELL_ELF=programs/shell/build/shell.elf
# The later page table first: it is the case that crashed before.
CASES="${OOM_CASES:-table second wrapper elfobject elfdata process}"
mkdir -p "$OUT"

# All ELF files must fit in one FAT cluster (64 KiB). oomtest.elf works out the block budget
# from the real size of blank.elf and the blocks of its segment memory (D).
[ -f "$PROGRAM" ] || { echo "$SUITE: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
for elf in "$SHELL_ELF" "$BLANK_ELF" "$PROGRAM"; do
    size=$(wc -c < "$elf")
    echo "  $elf: $size bytes"
    [ "$size" -le 65536 ] || { echo "$SUITE: $elf is bigger than 64 KiB" >&2; exit 1; }
done
blank_size=$(wc -c < "$BLANK_ELF")
# E, D and H as the comment at the top of programs/oomtest/oomtest.c defines them.
blank_blocks=$(python3 -B scripts/tests/lib/elfblocks.py "$BLANK_ELF") ||
    { echo "$SUITE: cannot read the program headers of $BLANK_ELF" >&2; exit 1; }
echo "  $BLANK_ELF: $blank_blocks"
blank_d=""
blank_h=""
for field in $blank_blocks; do
    case "$field" in
        D=*) blank_d="${field#D=}" ;;
        H=*) blank_h="${field#H=}" ;;
    esac
done
[ -n "$blank_d" ] && [ -n "$blank_h" ] || { echo "$SUITE: bad elfblocks.py output" >&2; exit 1; }
# The counts in oomtest.c assume the stack and the task still fit in the hole fclose leaves.
[ "$blank_h" -le 2 ] ||
    { echo "$SUITE: $BLANK_ELF puts $blank_h blocks in the parsed path's hole; oomtest.c counts need H <= 2" >&2; exit 1; }
[ "$QEMU_RAM_MIB" -ge 128 ] || { echo "$SUITE: needs QEMU_RAM_MIB >= 128 (is $QEMU_RAM_MIB)" >&2; exit 1; }

echo "$SUITE: kernel symbols for the debugger"
heap_snapshot_setup "$OUT" || { echo "$SUITE: cannot read the heap settings" >&2; exit 1; }
guest_state_setup "$OUT"

# state <dir> <tag> [options]: the guest-state rendezvous, results in <dir>/state-<tag>.txt.
state() {
    local dir="$1" tag="$2"
    shift 2
    guest_state "$HEAP_GDB_DIR/gdb" "$dir/state-$tag.txt" "$tag" timeout="$EXIT_TIMEOUT" "$@"
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/$SUITE.bin"
mcopy -i "$OUT/$SUITE.bin" "$PROGRAM" ::

# stop_checks <dir> <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$1/state-$2.txt"
    check "$3: stopped at the task's getkey"           "$f" "PASS $2_rendezvous" line
    check "$3: task list and slots consistent"        "$f" "PASS $2_list_consistent" line
    check "$3: number of tasks"                        "$f" "PASS $2_task_count" line
    check "$3: keyboard owner"                         "$f" "PASS $2_keyboard_owner" line
    [ "$2" = boot ] || check "$3: the same shell task, process and id" "$f" "PASS $2_same_shell" line
}

# run_case <case>: one fresh boot. Adds to $failures.
run_case() {
    local name="$1" dir="$OUT/$1" returned=0
    mkdir -p "$dir"
    rm -f "$dir"/state-*.txt "$dir"/state-*.txt.log "$dir/shell.txt" "$dir"/screen-*.txt
    : > "$dir/screen-before-blank.txt"
    echo "$SUITE: case $name (blank.elf is $blank_size bytes, $blank_d blocks of segment memory)"
    QEMU_FLAGS="$HEAP_QEMU_FLAGS" qemu_start "$OUT/$SUITE.bin" "$dir"
    heap_snapshot_trap
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
        state "$dir" boot save="$dir/shell.txt" owner=first screen="$dir/screen-boot.txt"; then
        type_line "oomtest.elf $name $blank_size $blank_d"
        # The heap gets nearly full only after a key, once the shell is back at its own key request
        # (its prompt printed, nothing left to allocate) with oomtest owning the keyboard, and the
        # heap debugger is attached. One syscall is checked against the last copy.
        if wait_screen_lines "$RUN_TIMEOUT" "prompt:oomtest $name: ready" &&
            state "$dir" ready tasks=2 shell="$dir/shell.txt" owner=last &&
            heap_snapshot_attach "$PROGRAM" "$dir" 1; then
            qemu_cmd "sendkey g"
            # "armed": the free blocks are set up. It waits for a second key before the syscall,
            # so the line is on screen even if the kernel crashes in the syscall.
            if wait_screen_lines "$RUN_TIMEOUT" "regex:(> )?oomtest $name: (armed with free blocks -?[0-9]+|done)" &&
                ! grep -qxE "(> )?oomtest $name: done" "$dir/screen.txt"; then
                qemu_cmd "sendkey r"
            fi
            # The "arg..." cases wait for one more key after the syscall: the failed launch must
            # leave the keyboard with oomtest, and no new task.
            if wait_screen_lines "$RUN_TIMEOUT" "regex:(> )?oomtest $name: (press a key|done)" &&
                ! grep -qxE "(> )?oomtest $name: done" "$dir/screen.txt" &&
                heap_snapshot_detached "$EXIT_TIMEOUT" &&
                state "$dir" press at=last tasks=2 shell="$dir/shell.txt" owner=last; then
                qemu_cmd "sendkey k"
            fi
            if wait_screen_lines "$RUN_TIMEOUT" "prompt:oomtest $name: done"; then
                returned=1
                # The shell alone again: oomtest has ended and the failed syscall left no program
                # running, so anything such a program printed is on the saved screen.
                if grep -qxE "(> )?oomtest $name: PASS" "$dir/screen.txt" &&
                    heap_snapshot_detached "$EXIT_TIMEOUT" &&
                    state "$dir" ended shell="$dir/shell.txt" owner=first screen="$dir/screen-before-blank.txt"; then
                    type_line "blank.elf after-$name"
                    wait_screen_lines "$EXIT_TIMEOUT" "prompt:after-$name" &&
                        state "$dir" after shell="$dir/shell.txt" owner=first || true
                fi
            fi
        fi
    fi
    if [ "$returned" = 0 ]; then
        echo "  $SUITE: $name did not return; possible allocation-path crash or hang"
        failures=$((failures + 1))
    fi
    qemu_stop || true
    heap_snapshot_wait "$dir"

    # Each process was seen gone at a rendezvous (the shell alone in the task list).
    ended=$(cat "$dir"/state-ended.txt "$dir"/state-after.txt 2> /dev/null | grep -cxE 'PASS (ended|after)_task_count' || true)
    echo "ended=${ended:-0}" > "$dir/ended.txt"

    check "$name: kernel reached the end of init (debugcon)"  "$dir/debugcon.log"   "Thats cool"
    check "$name: no kernel panic (debugcon)"                  "$dir/debugcon.log"   "PANIC" absent
    check "$name: shell started (banner on screen)"            "$dir/screen-boot.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
    check "$name: command echoed by the shell"                 "$dir/screen.txt"     "> oomtest.elf $name $blank_size $blank_d"
    check "$name: oomtest waited for the key"                  "$dir/screen.txt"     "oomtest $name: ready"
    check "$name: free blocks set up exactly"                  "$dir/screen.txt"     "oomtest $name: armed with free blocks"
    check "$name: no setup failure"                            "$dir/screen.txt"     "oomtest $name: FAIL setup" absent
    check "$name: syscall returned the right error"            "$dir/screen.txt"     "oomtest $name: FAIL result" absent
    check "$name: failed step gave its blocks back"            "$dir/screen.txt"     "oomtest $name: FAIL blocks kept" absent
    check "$name: oomtest passed"                              "$dir/screen.txt"     "oomtest $name: PASS"
    check "$name: oomtest finished"                            "$dir/screen.txt"     "oomtest $name: done"
    check "$name: no program started by the failed syscall"    "$dir/screen-before-blank.txt" "Blank loaded" absent
    case "$name" in
        arg*) check "$name: oomtest still got the keyboard"   "$dir/screen.txt"     "oomtest $name: got the key"
              stop_checks "$dir" press "$name: after the failed syscall (no new task, oomtest owns the keyboard)" ;;
    esac
    check "$name: blank.elf started from the shell afterwards" "$dir/screen.txt"     "Blank loaded"
    check "$name: blank.elf got its argument"                  "$dir/screen.txt"     "after-$name" line
    check "$name: kernel symbols for the rendezvous found"     "$OUT/symbols.txt"    "found" line
    stop_checks "$dir" boot  "$name: boot (the shell alone)"
    stop_checks "$dir" ready "$name: shell back at its key request before the heap fills, oomtest owns the keyboard"
    stop_checks "$dir" ended "$name: oomtest ended (the shell alone, owns the keyboard)"
    stop_checks "$dir" after "$name: blank.elf ended (the shell alone)"
    check "$name: both processes ended (each seen gone)"       "$dir/ended.txt"      "ended=2" line
    heap_snapshot_checks "$name: " "$dir"
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
