# Debugger side of the heap table copies that the out-of-memory test programs count in
# (programs/heapsnapshot/heap_snapshot.h, scripts/tests/lib/heap_snapshot.py). Source it after
# qemu-lib.sh, then:
#   heap_snapshot_setup <outdir>      once: kernel symbols, heap table geometry, gdbstub socket;
#                                     returns 1 when the symbols or settings are missing (stop then)
#   QEMU_FLAGS="$HEAP_QEMU_FLAGS" qemu_start ...; heap_snapshot_trap
#   heap_snapshot_attach <program.elf> <dir> <wrapper checks> [sites]
#                                     while the program waits for its first key: start the debugger,
#                                     wait until the VM runs on (keys sent while it is stopped are lost);
#                                     with "sites", also record where the kernel heap runs out in the
#                                     checked syscalls. Under TCG that slows them down: the breakpoint
#                                     makes QEMU run the heap code's 4 KiB page one instruction at a
#                                     time, and the heap's search loop runs there for every allocation.
#   heap_snapshot_wait <dir>          wait for the debugger to end (the program called heap_snapshot_end)
#   heap_snapshot_detached <seconds>  wait at most <seconds> for the debugger to end, without collecting
#                                     its status; another debugger (guest-state.sh) may attach after it
#   heap_snapshot_checks <label> <dir>
#   heap_snapshot_site <label> <dir> <function>|none
#                                     after an attach with "sites": where the kernel heap ran out
#                                     while the checked syscalls ran (heap_snapshot.py): exactly one
#                                     failed allocation, with <function> among its callers, or none
# Every wait is bounded; without a debugger the program fails its own acknowledgement checks.
. ./scripts/tests/lib/kernel-symbols.sh

heap_snapshot_setup() {
    local out="$1" table bytes block
    HEAP_SYMBOLS="$out/symbols.txt"
    kernel_symbols_or_stop "$out" heap-snapshot || return 1
    config_value() { awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h; }
    table=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
    bytes=$(config_value RORTHOX_HEAP_SIZE_BYTES)
    block=$(config_value RORTHOX_HEAP_BLOCK_SIZE)
    [ -n "$table" ] && [ -n "$bytes" ] && [ -n "$block" ] ||
        { echo "heap settings not found in kernel/config.h" >&2; return 1; }
    enomem=$(awk '$1 == "#define" && $2 == "ENOMEM" { print $3 }' include/rorthox/status.h)
    [ -n "$enomem" ] || { echo "ENOMEM not found in include/rorthox/status.h" >&2; return 1; }
    HEAP_KERNEL_ELF="$out/kernel.elf"
    HEAP_SNAPSHOT_ARGS="current_task=$SYM_CURRENT_TASK task_head=$SYM_TASK_HEAD next=$TASK_NEXT_OFFSET"
    HEAP_SNAPSHOT_ARGS+=" process=$TASK_PROCESS_OFFSET directory=$TASK_DIRECTORY_OFFSET filename=$PROCESS_FILENAME_OFFSET"
    HEAP_SNAPSHOT_ARGS+=" table=$(printf %x $((table))) blocks=$(printf %x $((bytes / block))) enomem=$(printf %x $((enomem)))"
    HEAP_GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
    HEAP_QEMU_FLAGS="-gdb unix:$HEAP_GDB_DIR/gdb,server=on,wait=off"
    trap 'rm -rf "$HEAP_GDB_DIR"' EXIT
}

# qemu_start sets its own EXIT trap (qemu_kill); keep both.
heap_snapshot_trap() { trap 'qemu_kill; rm -rf "$HEAP_GDB_DIR"' EXIT; }

# heap_snapshot_running <seconds>: wait until the monitor says the VM runs. The screen dump after
# "info status" is a monitor round trip: once it is back, the answer is in the log.
heap_snapshot_running() {
    local deadline=$((SECONDS + $1)) lines
    while ((SECONDS < deadline)); do
        lines=$(wc -l < "$Q_OUT/monitor.log")
        qemu_cmd "info status"
        screen_snapshot
        # grep reads the whole tail: with -q it could stop early, tail would then die of SIGPIPE
        # and pipefail would turn a match into a failure.
        tail -n +"$((lines + 1))" "$Q_OUT/monitor.log" | grep "VM status: running" > /dev/null && return 0
        sleep 0.2
    done
    echo "  the VM did not run on after the debugger attached"
    return 1
}

heap_snapshot_attach() {
    local elf="$1" dir="$2" checks="$3" i sites=()
    rm -f "$dir/heap-ready" "$dir/heap-result.txt" "$dir/heap-sites.txt"
    echo "not run" > "$dir/heap-status.txt"
    [ "${4:-}" != sites ] || sites=("kernel=$HEAP_KERNEL_ELF" "sites=$dir/heap-sites.txt")
    # shellcheck disable=SC2086
    timeout 300 python3 -B scripts/tests/lib/heap_snapshot.py "$HEAP_GDB_DIR/gdb" "$elf" "$dir/heap-result.txt" \
        "$dir/heap-ready" "$checks" $HEAP_SNAPSHOT_ARGS "${sites[@]}" > "$dir/heap-debugger.log" 2>&1 &
    HEAP_DEBUGGER=$!
    for ((i = 0; i < 600; i++)); do
        [ -f "$dir/heap-ready" ] || ! kill -0 "$HEAP_DEBUGGER" 2> /dev/null && break
        sleep 0.1
    done
    [ -f "$dir/heap-ready" ] && heap_snapshot_running 60
}

heap_snapshot_wait() {
    local dir="$1" status
    [ -n "${HEAP_DEBUGGER:-}" ] || return 0
    if wait "$HEAP_DEBUGGER"; then status=0; else status=$?; fi
    HEAP_DEBUGGER=
    echo "exit=$status" > "$dir/heap-status.txt"
    sed 's/^/  /' "$dir/heap-debugger.log"
}

heap_snapshot_detached() {
    local deadline=$((SECONDS + $1))
    [ -n "${HEAP_DEBUGGER:-}" ] || return 0
    while kill -0 "$HEAP_DEBUGGER" 2> /dev/null; do
        ((SECONDS < deadline)) || { echo "  the heap debugger did not end within $1 s"; return 1; }
        sleep 0.1
    done
}

heap_snapshot_checks() {
    local label="$1" dir="$2"
    [ -f "$dir/heap-result.txt" ] || : > "$dir/heap-result.txt"
    check "${label}kernel symbols found for the debugger"        "$HEAP_SYMBOLS"           "found" line
    check "${label}heap debugger finished without errors"        "$dir/heap-status.txt"    "exit=0" line
    check "${label}heap debugger attached"                       "$dir/heap-result.txt"    "PASS attached" line
    check "${label}stops only in the program's own task"         "$dir/heap-result.txt"    "PASS identity" line
    check "${label}stops in ring 3 with the task's page directory" "$dir/heap-result.txt"  "PASS task_context" line
    check "${label}every copy request well formed"               "$dir/heap-result.txt"    "PASS requests" line
    check "${label}few stops in other tasks"                     "$dir/heap-result.txt"    "PASS other_stops" line
    check "${label}every copy the program asked for was given"   "$dir/heap-result.txt"    "PASS every_copy" line
    check "${label}heap unchanged from the last copy to the syscall" "$dir/heap-result.txt" "PASS unchanged" line
    check "${label}every syscall checked against its copy"       "$dir/heap-result.txt"    "PASS wrapper_checks" line
}

heap_snapshot_site() {
    local label="$1" dir="$2" site="$3"
    [ -f "$dir/heap-sites.txt" ] || : > "$dir/heap-sites.txt"
    if [ "$site" = none ]; then
        check "${label}no kernel heap allocation failed in the syscalls" "$dir/heap-sites.txt" "failures=0" line
    else
        check "${label}one kernel heap allocation failed in the syscall"  "$dir/heap-sites.txt" "failures=1" line
        check "${label}the allocation that failed was made through $site" "$dir/heap-sites.txt" "failure 1 caller $site" line
    fi
}
