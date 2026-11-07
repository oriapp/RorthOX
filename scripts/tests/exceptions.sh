#!/usr/bin/env bash
#
# A CPU exception in a user program must end only that program (the same way as a normal exit,
# so "Process was terminated" shows and the shell keeps working), never resume it.
#
# Each case boots a fresh copy of bin/os.bin with programs/faulttest added as fault.elf (the
# FAT16 driver has no long names), with QEMU's gdbstub on a Unix socket. bin/os.bin must not change.
#   ud2    #UD, no error code             int3   #BP, a trap (must not resume after the int3)
#   cli    #GP, error code 0              pf     #PF on a freed page, error 4, CR2 = the pointer
#   int14  software "int 14" from ring 3: the gate is DPL 0, so #GP with error 0x72
#   int18  software "int 18" from ring 3: the #MC gate is DPL 0, so #GP with error 0x92 (18*8+2)
#   in     "in al, 0x80" and "out 0x80, al" from ring 3: IOPL is 0 and the TSS has no I/O
#   out    permission bitmap, so #GP with error 0 (not in the default cases: tss-io.sh runs them)
#   in21 out20 inffff   the same for "in al, 0x21", "out 0x20, al" and "in al, dx" with dx 0xffff
#          (tss-io.sh too)
# fault.elf prints "faulttest: armed" and waits for a key. scripts/tests/lib/exceptions.py catch
# then breaks at the exception stubs and the test sends the key. It checks the CPU's frame at the
# stub, the frame interrupt_handler gets and that process_terminate gets the faulting task's process.
# Then scripts/tests/lib/guest-state.sh stops the guest when the shell asks for a key: the task list
# and process slots must hold only the shell, the same task (same task id: ids only go up, so a
# shell started again would have a new one) and process as before the fault, and the shell must
# own the keyboard. So fault.elf is really gone, not just quiet on the screen, and the screen
# saved at that stop (never halfway through a print or a scroll) must show its end once. The same
# after "blank.elf recover" has printed "recover", and the kernel heap must be back where it was,
# apart from the one block per typed word that the shell keeps.
# Last case, owner: "fault.elf parent" starts "fault.elf child", which owns the keyboard, and then
# faults itself. When the child asks for its key, only the shell and the child may be left (the
# parent gone while the child lives) and the child must own the keyboard; then the child must get
# its key and finish, and the same shell must own the keyboard again.
#
# Needs python3 and the cross tools (lib/kernel-symbols.sh links an ELF copy of the kernel for the
# address of the process slots and compiles the struct offsets).
# Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)  STEP_TIMEOUT=20 (each step after)
#      EXCEPTION_CASES="ud2 int3 cli pf int14 int18 owner"
#      EXCEPTIONS_NAME=exceptions (test name and output directory name)

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
CASES="${EXCEPTION_CASES:-ud2 int3 cli pf int14 int18 owner}"
NAME="${EXCEPTIONS_NAME:-exceptions}"
# The name becomes a directory that is removed and made again: keep it one plain path component.
[[ $NAME =~ ^[a-z0-9-]+$ ]] || { echo "EXCEPTIONS_NAME must match ^[a-z0-9-]+\$" >&2; exit 2; }
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/$NAME
PROGRAM=programs/faulttest/build/faulttest.elf
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "$NAME: python3 not found" >&2; exit 1; }
[ -f "$PROGRAM" ] || { echo "$NAME: $PROGRAM not found, run 'make all' first" >&2; exit 1; }
guest_state_setup "$OUT"

# The kernel heap table: one byte per block, 0 = free.
config_value() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h
}
table_address=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
table_bytes=$(( $(config_value RORTHOX_HEAP_SIZE_BYTES) / $(config_value RORTHOX_HEAP_BLOCK_SIZE) ))
# Each typed word stays allocated in the shell (the user library never frees command nodes).
WORDS=4   # "fault.elf <case>" and "blank.elf recover"

# free_blocks: number of free heap blocks, read twice with the guest stopped until two reads agree.
free_blocks() {
    local i n last=
    for ((i = 0; i < 20; i++)); do
        rm -f "$Q_OUT/heap.bin"
        qemu_cmd "stop"
        qemu_cmd "pmemsave $table_address $table_bytes \"$Q_OUT/heap.bin\""
        qemu_cmd "cont"
        wait_file "$Q_OUT/heap.bin" "$table_bytes" || return 1
        n=$(LC_ALL=C tr -d '\001-\377' < "$Q_OUT/heap.bin" | wc -c)
        [ "$n" = "$last" ] && { echo "$n"; return 0; }
        last=$n
        sleep 0.3
    done
    return 1
}

# state <tag> [options]: the guest-state rendezvous (lib/guest-state.sh), results in $D/list-<tag>.txt.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$D/list-$tag.txt" "$tag" timeout="$STEP_TIMEOUT" "$@"
}

# ended <tag>...: how many of these rendezvous saw a program gone (their task count was right).
ended() {
    local tag n=0
    for tag in "$@"; do
        grep -qx "PASS ${tag}_task_count" "$D/list-$tag.txt" 2> /dev/null && n=$((n + 1))
    done
    echo "$n"
}

# key_when_running: send a key once the monitor reports the guest running again. The debugger
# has just continued it, and QEMU drops key presses that arrive while the guest is stopped.
key_when_running() {
    local i seen
    seen=$(grep -a -c 'VM status' "$Q_OUT/monitor.log" || true)
    for ((i = 0; i < 50; i++)); do
        qemu_cmd "info status"
        screen_snapshot      # a monitor round trip: the status line is in monitor.log after it
        # grep reads everything: with -q it could stop early, tail would then die of SIGPIPE and
        # pipefail would turn a match into a failure.
        if grep -a 'VM status' "$Q_OUT/monitor.log" | tail -n +$((seen + 1)) | grep 'VM status: running' > /dev/null; then
            qemu_cmd "sendkey k"
            return 0
        fi
        sleep 0.1
    done
    echo "  the guest did not run again after the debugger continued it"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/exceptions.bin"
mcopy -i "$OUT/exceptions.bin" "$PROGRAM" ::fault.elf
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# One user fault: boot, launch, arm the debugger, send the key, recover.
run_fault() {
    local c="$1" before= after= status="not run"
    D="$OUT/$c"
    mkdir -p "$D"
    echo "$NAME: $c"
    rm -f "$SOCK" "$D/ready"
    QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/exceptions.bin" "$D"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
        state idle save="$D/shell.txt" owner=first && before=$(free_blocks); then
        type_line "fault.elf $c"
        if wait_screen_lines "$STEP_TIMEOUT" "prompt:faulttest: armed"; then
            python3 -B scripts/tests/lib/exceptions.py catch "$SOCK" build/kernel.map "$D/catch.txt" "$D/ready" "$c" \
                > "$D/catch.log" 2>&1 &
            local py=$! i
            for ((i = 0; i < 100; i++)); do [ -f "$D/ready" ] && break; sleep 0.1; done
            # Only key presses that arrive while the guest runs reach it.
            [ -f "$D/ready" ] && { key_when_running || true; }
            if wait "$py"; then status=0; else status=$?; fi
            sed 's/^/  /' "$D/catch.log"
            # fault.elf gone: only the same shell left, and it gets the keys again.
            if state fault shell="$D/shell.txt" owner=first screen="$D/screen-fault.txt"; then
                type_line "blank.elf recover"
                if wait_screen_lines "$STEP_TIMEOUT" "line:recover" &&
                    state after shell="$D/shell.txt" owner=first; then
                    after=$(free_blocks) || after=
                fi
            fi
        fi
    fi
    qemu_stop || true
    echo "exit=$status" > "$D/catch-status.txt"
    {
        # Counted on the screen saved at the rendezvous: nothing is printing or scrolling there.
        echo "fault terminated=$(grep -cF "Process was terminated" "$D/screen-fault.txt" 2> /dev/null || true)"
        echo "ended=$(ended fault after)"
        [ -n "$before" ] && [ -n "$after" ] && echo "heap kept=$((before - after))"
    } > "$D/state.txt"
    echo "  free blocks before ${before:-?}, after ${after:-?} ($WORDS kept for typed words)"

    local r="$D/catch.txt" s="$D/state.txt"
    check "$c: debugger session finished without errors"     "$D/catch-status.txt" "exit=0" line
    case "$c" in
        int14 | int18) check "$c: entered the #GP stub (vector 13)"  "$r" "PASS stub_vector" line ;;
        *)     check "$c: entered the expected exception stub"         "$r" "PASS stub_vector" line ;;
    esac
    case "$c" in
        cli | pf | int14 | int18 | in | out | in21 | out20 | inffff) check "$c: CPU error code as expected"     "$r" "PASS cpu_error_code" line ;;
    esac
    [ "$c" = pf ] && check "$c: CR2 is the freed pointer"             "$r" "PASS cpu_cr2" line
    [ "$c" = inffff ] && check "$c: DX held port 0xffff at the fault"   "$r" "PASS io_port" line
    check "$c: fault came from ring 3"                              "$r" "PASS cpu_ring3" line
    check "$c: at the fault.elf instruction for this case"          "$r" "PASS faulting_instruction" line
    check "$c: interrupt_handler got the right vector and frame"    "$r" "PASS handler_frame" line
    check "$c: process_terminate on the faulting task's process"    "$r" "PASS process_terminated" line
    check "$c: fault: stopped at the same shell's getkey"           "$D/list-fault.txt" "PASS fault_rendezvous" line
    check "$c: fault: task list and slots consistent"               "$D/list-fault.txt" "PASS fault_list_consistent" line
    check "$c: fault: fault.elf gone, only one task left"           "$D/list-fault.txt" "PASS fault_task_count" line
    check "$c: fault: the shell owns the keyboard"                  "$D/list-fault.txt" "PASS fault_keyboard_owner" line
    check "$c: fault.elf ended once"                                "$s" "fault terminated=1" line
    check "$c: fault.elf was not resumed"                           "$D/screen.txt" "faulttest: survived" absent
    check "$c: blank.elf ran from the shell afterwards"             "$D/screen.txt" "recover" line
    check "$c: two programs ended in total (each seen gone)"        "$s" "ended=2" line
    check "$c: shell not started again by the fault (same task id)" "$D/list-fault.txt" "PASS fault_same_shell" line
    check "$c: idle: stopped at the shell's getkey"                 "$D/list-idle.txt" "PASS idle_rendezvous" line
    check "$c: idle: task list and slots consistent"                "$D/list-idle.txt" "PASS idle_list_consistent" line
    check "$c: idle: only the shell"                               "$D/list-idle.txt" "PASS idle_task_count" line
    check "$c: idle: the shell owns the keyboard"                   "$D/list-idle.txt" "PASS idle_keyboard_owner" line
    check "$c: after: stopped at the same shell's getkey"           "$D/list-after.txt" "PASS after_rendezvous" line
    check "$c: after: task list and slots consistent"               "$D/list-after.txt" "PASS after_list_consistent" line
    check "$c: after: only one task left"                           "$D/list-after.txt" "PASS after_task_count" line
    check "$c: after: it is the same shell task and process"        "$D/list-after.txt" "PASS after_same_shell" line
    check "$c: after: the shell owns the keyboard"                  "$D/list-after.txt" "PASS after_keyboard_owner" line
    check "$c: kernel heap back apart from $WORDS command nodes"    "$s" "heap kept=$WORDS" line
    check "$c: no kernel panic (debugcon)"                          "$D/debugcon.log" "PANIC" absent
}

# The faulting task is not the keyboard owner: shell -> parent (faults) -> child (owns the keyboard).
run_owner() {
    local before= after=
    D="$OUT/owner"
    mkdir -p "$D"
    echo "$NAME: owner"
    rm -f "$SOCK"
    QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/exceptions.bin" "$D"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
        state idle save="$D/shell.txt" owner=first && before=$(free_blocks); then
        type_line "fault.elf parent"
        # The child asks for its key: the parent must be gone by then, the child still there.
        if wait_screen_lines "$STEP_TIMEOUT" "prompt:faulttest child: ready" "prompt:faulttest parent: faulting" &&
            state child at=last tasks=2 shell="$D/shell.txt" owner=last screen="$D/screen-parent-gone.txt"; then
            qemu_cmd "sendkey k"
            if wait_screen_lines "$STEP_TIMEOUT" "prompt:faulttest child: done" &&
                state back shell="$D/shell.txt" owner=first; then
                type_line "blank.elf recover"
                if wait_screen_lines "$STEP_TIMEOUT" "line:recover" &&
                    state after shell="$D/shell.txt" owner=first; then
                    after=$(free_blocks) || after=
                fi
            fi
        fi
    fi
    qemu_stop || true
    {
        echo "ended=$(ended child back after)"
        echo "parent gone terminated=$(grep -cF "Process was terminated" "$D/screen-parent-gone.txt" 2> /dev/null || true)"
        echo "parent gone done=$(grep -cF "faulttest child: done" "$D/screen-parent-gone.txt" 2> /dev/null || true)"
        [ -n "$before" ] && [ -n "$after" ] && echo "heap kept=$((before - after))"
    } > "$D/state.txt"
    echo "  free blocks before ${before:-?}, after ${after:-?} ($WORDS kept for typed words)"

    local s="$D/state.txt"
    check "owner: idle: stopped at the shell's getkey"              "$D/list-idle.txt" "PASS idle_rendezvous" line
    check "owner: idle: task list and slots consistent"            "$D/list-idle.txt" "PASS idle_list_consistent" line
    check "owner: idle: only the shell"                            "$D/list-idle.txt" "PASS idle_task_count" line
    check "owner: idle: the shell owns the keyboard"               "$D/list-idle.txt" "PASS idle_keyboard_owner" line
    check "owner: child started and is waiting"                     "$D/screen-parent-gone.txt" "faulttest child: ready"
    check "owner: parent reached its fault"                         "$D/screen-parent-gone.txt" "faulttest parent: faulting"
    check "owner: parent ended while the child waited"              "$s" "parent gone terminated=1" line
    check "owner: (the child had not finished yet)"                 "$s" "parent gone done=0" line
    check "owner: parent was not resumed"                           "$D/screen.txt" "faulttest: survived" absent
    check "owner: stopped at the child's getkey"                    "$D/list-child.txt" "PASS child_rendezvous" line
    check "owner: list and slots hold shell and child"              "$D/list-child.txt" "PASS child_list_consistent" line
    check "owner: two tasks left after the parent's fault"          "$D/list-child.txt" "PASS child_task_count" line
    check "owner: the shell is still the first task"                "$D/list-child.txt" "PASS child_same_shell" line
    check "owner: the child still owns the keyboard"                "$D/list-child.txt" "PASS child_keyboard_owner" line
    check "owner: child got its key and finished"                   "$D/screen.txt" "faulttest child: done"
    check "owner: back: stopped at the same shell's getkey"         "$D/list-back.txt" "PASS back_rendezvous" line
    check "owner: back: task list and slots consistent"            "$D/list-back.txt" "PASS back_list_consistent" line
    check "owner: back: child gone, the shell owns the keyboard"    "$D/list-back.txt" "PASS back_keyboard_owner" line
    check "owner: back: it is the same shell task and process"      "$D/list-back.txt" "PASS back_same_shell" line
    check "owner: back: only one task left"                         "$D/list-back.txt" "PASS back_task_count" line
    check "owner: blank.elf ran from the shell afterwards"          "$D/screen.txt" "recover" line
    check "owner: three programs ended in total (each seen gone)"  "$s" "ended=3" line
    check "owner: after: stopped at the same shell's getkey"        "$D/list-after.txt" "PASS after_rendezvous" line
    check "owner: after: task list and slots consistent"            "$D/list-after.txt" "PASS after_list_consistent" line
    check "owner: after: only the same shell left"                  "$D/list-after.txt" "PASS after_same_shell" line
    check "owner: after: only one task left"                        "$D/list-after.txt" "PASS after_task_count" line
    check "owner: after: the shell owns the keyboard"               "$D/list-after.txt" "PASS after_keyboard_owner" line
    check "owner: kernel heap back apart from $WORDS command nodes" "$s" "heap kept=$WORDS" line
    check "owner: no kernel panic (debugcon)"                       "$D/debugcon.log" "PANIC" absent
}

for c in $CASES; do
    case "$c" in
        owner) run_owner ;;
        *) run_fault "$c" ;;
    esac
done

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "kernel symbols for the rendezvous found" "$OUT/symbols.txt" "found" line
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
finish "$NAME"
