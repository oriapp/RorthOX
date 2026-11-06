# The guest-state rendezvous (scripts/tests/lib/guest_state.py) for tests that must know more than
# the screen shows: that a program has really ended, that the shell is still the same task and
# which process gets the keys. Source it after qemu-lib.sh, start QEMU with
# "-gdb unix:<socket>,server=on,wait=off", then:
#   guest_state_setup <outdir>     once: kernel symbols and offsets (kernel-symbols.sh); writes
#                                  <outdir>/symbols.txt ("found" or "missing")
#   guest_state <socket> <results> <tag> [option=value...]
#                                  stop at the next getkey syscall of the shell (at=first) or of the
#                                  last task (at=last) with the expected number of tasks (tasks=1),
#                                  check the task list, slots and keyboard owner, then let the guest
#                                  run on. Writes "PASS/FAIL <tag>_..." lines to <results> and its
#                                  output to <results>.log; returns 1 if a check failed.
# Every wait is bounded (timeout=, default 20 s). Don't call it while another debugger (a test's own
# python script) is attached to the same QEMU: one connection at a time. Keys must be sent after it
# returns (QEMU drops key presses while the guest is stopped).
. ./scripts/tests/lib/kernel-symbols.sh

guest_state_setup() {
    if kernel_symbols "$1"; then echo "found" > "$1/symbols.txt"; else echo "missing" > "$1/symbols.txt"; fi
    GUEST_STATE_ARGS="isr80h_handler=$SYM_ISR80H_HANDLER current_task=$SYM_CURRENT_TASK task_head=$SYM_TASK_HEAD"
    GUEST_STATE_ARGS+=" task_tail=$SYM_TASK_TAIL current_process=$SYM_CURRENT_PROCESS processes=$SYM_PROCESSES"
    GUEST_STATE_ARGS+=" max=$MAX_PROCESSES next=$TASK_NEXT_OFFSET prev=$TASK_PREV_OFFSET tid=$TASK_ID_OFFSET"
    GUEST_STATE_ARGS+=" process=$TASK_PROCESS_OFFSET"
    GUEST_STATE_ARGS+=" directory=$TASK_DIRECTORY_OFFSET id=$PROCESS_ID_OFFSET task=$PROCESS_TASK_OFFSET"
    GUEST_STATE_ARGS+=" filename=$PROCESS_FILENAME_OFFSET cs=$FRAME_CS_OFFSET"
}

guest_state() {
    local sock="$1" results="$2" status
    shift 2
    # shellcheck disable=SC2086
    if timeout 120 python3 -B scripts/tests/lib/guest_state.py "$sock" "$results" "$1" $GUEST_STATE_ARGS "${@:2}" \
        > "$results.log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$results.log"
    return "$status"
}
