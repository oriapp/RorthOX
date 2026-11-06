# Debugger side of the heap table copies that the out-of-memory test programs count in
# (programs/heapsnapshot/heap_snapshot.h, scripts/tests/lib/heap_snapshot.py). Source it after
# qemu-lib.sh, then:
#   heap_snapshot_setup <outdir>      once: kernel symbols, heap table geometry, gdbstub socket
#   QEMU_FLAGS="$HEAP_QEMU_FLAGS" qemu_start ...; heap_snapshot_trap
#   heap_snapshot_attach <program.elf> <dir> <wrapper checks>
#                                     while the program waits for its first key: start the debugger,
#                                     wait until the VM runs on (keys sent while it is stopped are lost)
#   heap_snapshot_wait <dir>          wait for the debugger to end (the program called heap_snapshot_end)
#   heap_snapshot_checks <label> <dir>
# Every wait is bounded; without a debugger the program fails its own acknowledgement checks.
. ./scripts/tests/lib/kernel-symbols.sh

heap_snapshot_setup() {
    local out="$1" table bytes block
    HEAP_SYMBOLS="$out/symbols.txt"
    if kernel_symbols "$out"; then echo "found" > "$HEAP_SYMBOLS"; else echo "missing" > "$HEAP_SYMBOLS"; fi
    config_value() { awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/config.h; }
    table=$(config_value RORTHOX_HEAP_TABLE_ADDRESS)
    bytes=$(config_value RORTHOX_HEAP_SIZE_BYTES)
    block=$(config_value RORTHOX_HEAP_BLOCK_SIZE)
    [ -n "$table" ] && [ -n "$bytes" ] && [ -n "$block" ] ||
        { echo "heap settings not found in kernel/config.h" >&2; return 1; }
    HEAP_SNAPSHOT_ARGS="current_task=$SYM_CURRENT_TASK task_head=$SYM_TASK_HEAD next=$TASK_NEXT_OFFSET"
    HEAP_SNAPSHOT_ARGS+=" process=$TASK_PROCESS_OFFSET directory=$TASK_DIRECTORY_OFFSET filename=$PROCESS_FILENAME_OFFSET"
    HEAP_SNAPSHOT_ARGS+=" table=$(printf %x $((table))) blocks=$(printf %x $((bytes / block)))"
    HEAP_GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
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
        tail -n +"$((lines + 1))" "$Q_OUT/monitor.log" | grep -q "VM status: running" && return 0
        sleep 0.2
    done
    echo "  the VM did not run on after the debugger attached"
    return 1
}

heap_snapshot_attach() {
    local elf="$1" dir="$2" checks="$3" i
    rm -f "$dir/heap-ready" "$dir/heap-result.txt"
    echo "not run" > "$dir/heap-status.txt"
    # shellcheck disable=SC2086
    timeout 300 python3 -B scripts/tests/lib/heap_snapshot.py "$HEAP_GDB_DIR/gdb" "$elf" "$dir/heap-result.txt" \
        "$dir/heap-ready" "$checks" $HEAP_SNAPSHOT_ARGS > "$dir/heap-debugger.log" 2>&1 &
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
