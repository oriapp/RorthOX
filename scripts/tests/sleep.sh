#!/usr/bin/env bash
#
# Sleep (syscall 12): sleep(ms) blocks only the caller for at least ceil(ms / 10) + 2 timer ticks,
# while other tasks run, and then returns 0 with the caller's registers and stack as they were.
#
# 1. Host check (scripts/tests/host/sleep.c, ASan + UBSan): the real syscall handler reads its
#    operand once through the checked stack copy, returns 0 at once for 0, -EINVARG for negative
#    values and everything above INT32_MAX, a failed read's error, and for 1..INT32_MAX puts the
#    task to sleep until now + ticks (also across the 2^32 wrap) with its saved eax already 0.
#    Refused and zero requests don't sleep, don't switch tasks and change nothing in the task.
# 2. QEMU with its gdbstub on a Unix socket: copy bin/os.bin, add programs/libtest to the copy
#    only, and run "libtest.elf sleep" from the shell. The program times its sleeps with the tick
#    syscall (19) and reads no kernel memory:
#      - sleep(0) is 0, sleep(-1) and sleep(0x80000000) are -2;
#      - two sleep(50) return 0 after at least 7 ticks each;
#      - it starts "libtest.elf sleeppeer" and runs sleep(1000): that returns 0 after at least
#        102 ticks;
#      - every sleep keeps ebx, esi, edi, ebp, esp and the stack word above its argument;
#      - both programs end (the shell is then the only task and owns the keyboard, seen from the
#        host as below), and the shell then still runs blank.elf.
#    The peer runs 250 ticks and calls lt_peer_sample once per new tick. scripts/tests/lib/
#    sleep_peer.py, attached before the key that starts the sleeps, stops there and looks at the
#    sleeper's task from the host (task list, task identity, CR3: see that file). At least 10
#    distinct ticks must show the peer running while the sleeper's state is sleeping with its
#    deadline still ahead, all inside the sleeper's own window (the ticks before and after its
#    sleep(1000)); so the sleeper was really blocked and the peer ran. Right after its sleep(1000)
#    the sleeper stops once at lt_sleeper_woke: it must be runnable again and past its deadline.
#    The peer must report every sample acknowledged by the debugger, so the test can't pass
#    without one. After that debugger has detached, scripts/tests/lib/guest-state.sh stops the
#    guest when the shell asks for a key: only the shell may be left, owning the keyboard, and the
#    screen saved there (never halfway through a print or a scroll) is the one the lines are read from.
# bin/os.bin itself must not change.
#
# Needs python3. Env: HOST_CC=cc   BOOT_TIMEOUT=60   STEP_TIMEOUT=60

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-60}"
HOST_CC="${HOST_CC:-cc}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=build/tests/sleep
PROGRAM=programs/libtest/build/libtest.elf
mkdir -p "$OUT"
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT/peer-ready"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f build/kernelfull.o ] || { echo "sleep: run 'make all' first" >&2; exit 1; }

echo "sleep: host check of the syscall handler (AddressSanitizer, UndefinedBehaviorSanitizer)"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w -fsanitize=address,undefined \
        -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/sleep.c -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if ASAN_OPTIONS=detect_leaks=0 "$OUT/host-check" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    sed 's/^/  /' "$OUT/host-check.log" | head -n 20
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0" "$OUT/host-status.txt" "exit=0" line
check "no sanitizer report"                       "$OUT/host-check.log" "runtime error" absent
check "no AddressSanitizer report"                "$OUT/host-check.log" "AddressSanitizer" absent
check "host check: every handler case passed"     "$OUT/host-check.log" "sleep host check: 0 of 67 cases failed" line

echo "sleep: kernel symbols"
guest_state_setup "$OUT"
check "tick counter, current_task and struct task offsets found" "$OUT/symbols.txt" "found" line

hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
cp bin/os.bin "$OUT/sleep.bin"
mcopy -i "$OUT/sleep.bin" "$PROGRAM" ::libtest.elf
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# vm_running <seconds>: wait until the monitor says the VM runs (the debugger's continue is in).
# QEMU drops key presses while a debugger has the VM stopped. The screen dump after "info status"
# is a monitor round trip: once it is back, the answer is in the log.
vm_running() {
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

# keep: add the current screen to screens.txt, so lines that scroll off later still count.
keep() { screen_snapshot; cat "$OUT/screen.txt" >> "$OUT/screens.txt"; }

echo "sleep: boot"
: > "$OUT/screens.txt"
: > "$OUT/samples.txt"
debugger_status="not run"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/sleep.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "libtest.elf sleep"
    if wait_screen "libtest sleep: armed" "$STEP_TIMEOUT"; then
        timeout 300 python3 -B scripts/tests/lib/sleep_peer.py "$SOCK" "$PROGRAM" "$OUT/peer-result.txt" \
            "$OUT/samples.txt" "$OUT/peer-ready" current_task="$SYM_CURRENT_TASK" task_head="$SYM_TASK_HEAD" \
            jiffies="$SYM_TICKS" state="$TASK_STATE_OFFSET" wake="$TASK_WAKE_OFFSET" next="$TASK_NEXT_OFFSET" \
            process="$TASK_PROCESS_OFFSET" directory="$TASK_DIRECTORY_OFFSET" filename="$PROCESS_FILENAME_OFFSET" \
            > "$OUT/peer.log" 2>&1 &
        debugger=$!
        for ((i = 0; i < STEP_TIMEOUT * 10; i++)); do
            [ -f "$OUT/peer-ready" ] || ! kill -0 "$debugger" 2> /dev/null && break
            sleep 0.1
        done
        if [ -f "$OUT/peer-ready" ] && vm_running "$STEP_TIMEOUT"; then
            qemu_cmd "sendkey a"
            wait_screen "lt-s: short sleeps" "$STEP_TIMEOUT" && keep
            # The sleeper wakes while the peer runs on; then both programs end. Their last lines
            # on screen don't mean they have ended: the rendezvous (after sleep_peer.py has
            # detached, one debugger at a time) must find the shell alone.
            if wait_screen_lines "$STEP_TIMEOUT" "regex:(> )?lt-s: slept .*" "regex:(> )?lt-p: ran from .*"; then
                if wait "$debugger"; then debugger_status=0; else debugger_status=$?; fi
                guest_state "$SOCK" "$OUT/state-end.txt" end timeout="$STEP_TIMEOUT" owner=first \
                    screen="$OUT/screen-end.txt" || true
                cat "$OUT/screen-end.txt" >> "$OUT/screens.txt" 2> /dev/null || true
                type_line "blank.elf after"
                wait_screen_lines "$STEP_TIMEOUT" "line:after" || true
            fi
        fi
        if [ "$debugger_status" = "not run" ]; then
            if wait "$debugger"; then debugger_status=0; else debugger_status=$?; fi
        fi
        sed 's/^/  /' "$OUT/peer.log"
    fi
fi
keep
qemu_stop || true
echo "exit=$debugger_status" > "$OUT/peer-status.txt"

S="$OUT/screens.txt"
# Ticks at which the debugger saw the peer run while the sleeper slept with its deadline ahead,
# inside the sleeper's own window; at least 10 distinct ones.
window=$(sed -n 's/.*lt-s: slept r 0 ticks \([0-9]*\) from \([0-9]*\) to \([0-9]*\).*/\2 \3/p' "$S" | head -n 1)
peer=$(sed -n 's/.*lt-p: ran from \([0-9]*\) to \([0-9]*\) samples \([0-9]*\) acknowledged \([0-9]*\).*/\1 \2 \3 \4/p' "$S" |
    head -n 1)
echo "  sleeper window: ${window:-none}; peer first, last, samples, acknowledged: ${peer:-none}"
if [ -n "$window" ]; then
    read -r start end <<< "$window"
    asleep=$(awk -v s="$start" -v e="$end" '
        $1 == "sample" && $5 == "1" {
            left = ($6 - $4 + 4294967296) % 4294967296
            if ($4 >= s && $4 <= e && left > 0 && left < 2147483648) seen[$4] = 1
        }
        END { n = 0; for (t in seen) n++; print n }' "$OUT/samples.txt")
    echo "  ticks with the peer running and the sleeper asleep: $asleep ($(grep -c '^sample' "$OUT/samples.txt") samples)"
    [ "$asleep" -ge 10 ] && echo "inside" > "$OUT/peer.txt" || echo "outside" > "$OUT/peer.txt"
else
    echo "no lines" > "$OUT/peer.txt"
fi
if [ -n "$peer" ]; then
    read -r first last samples acknowledged <<< "$peer"
    [ "$samples" -ge 10 ] && [ "$acknowledged" = "$samples" ] && echo "acknowledged" > "$OUT/acks.txt"
fi
[ -f "$OUT/acks.txt" ] || echo "not acknowledged" > "$OUT/acks.txt"
[ "$hash_before" = "$(sha256sum bin/os.bin | cut -d' ' -f1)" ] && echo "unchanged" > "$OUT/image.txt" ||
    echo "changed" > "$OUT/image.txt"

P="$OUT/peer-result.txt"
check "no kernel panic (debugcon)"                         "$OUT/debugcon.log" "PANIC" absent
check "sleep(0) 0, sleep(-1) -2, sleep(0x80000000) -2"     "$S" "lt-s: zero 0 negative -2 high -2"
check "two sleep(50): 0 after at least 7 ticks each"       "$S" "lt-s: short sleeps ok"
check "sleep(1000): 0 after at least 102 ticks"            "$S" "lt-s: long sleep ok"
check "registers and stack kept by every sleep"            "$S" "lt-s: ebx esi edi ebp esp and stack kept"
check "the peer ran and ended"                             "$S" "lt-p: ran from"
check "debugger session finished without errors"           "$OUT/peer-status.txt" "exit=0" line
check "debugger attached before the sleeps"                "$P" "PASS attached" line
check "no debugger stops in other tasks"                    "$P" "PASS other_stops" line
check "samples in the peer, the sleeper is the other task" "$P" "PASS identity" line
check "stops in ring 3 with the task's own page directory" "$P" "PASS task_context" line
check "sleeper runnable and past its deadline after sleep" "$P" "PASS sleeper_woke" line
check "debugger saw every sample of the peer"              "$P" "PASS every_sample" line
check "the peer got every sample acknowledged"             "$OUT/acks.txt" "acknowledged" line
check "the peer ran on 10+ ticks inside the sleep"         "$OUT/peer.txt" "inside" line
check "the sleeper finished"                               "$S" "libtest sleep: done"
check "both programs ended: only the shell left"           "$OUT/state-end.txt" "PASS end_task_count" line
check "stopped at the shell's getkey"                      "$OUT/state-end.txt" "PASS end_rendezvous" line
check "task list and slots consistent at that stop"        "$OUT/state-end.txt" "PASS end_list_consistent" line
check "the shell owns the keyboard again"                  "$OUT/state-end.txt" "PASS end_keyboard_owner" line
check "no FAIL line"                                       "$S" "FAIL" absent
check "blank.elf started from the shell afterwards"        "$OUT/screen.txt" "after" line
check "bin/os.bin not changed by the test"                 "$OUT/image.txt" "unchanged" line
finish sleep
