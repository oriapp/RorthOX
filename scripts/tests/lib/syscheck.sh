# Shared ring-3 probe session. The caller sets OUT and prepares IMAGE.
# Each invocation of a QEMU-bearing test must run under timeout 120.
. ./scripts/tests/lib/guest-state.sh

# syscheck_setup: once per test, the kernel symbols for the guest-state rendezvous and a directory
# for the debugger sockets (removed by the EXIT trap).
SYSCHECK_GDB=
syscheck_cleanup() { [ -z "$SYSCHECK_GDB" ] || rm -rf "$SYSCHECK_GDB"; }
syscheck_setup() {
    [ -z "$SYSCHECK_GDB" ] || return 0
    guest_state_setup "$OUT"
    Q_CLEANUP=syscheck_cleanup
    trap qemu_exit EXIT
    SYSCHECK_GDB=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
}
# wait_exits <dir> <mode>: until syscheck and any program it started have ended: the guest-state
# rendezvous stops at the shell's key request with the shell alone in the task list, the same
# shell as at boot, owning the keyboard. Exit rows on the screen are no proof: while the tty
# scrolls, one row can show up twice for a moment.
wait_exits() {
    guest_state "$SYSCHECK_SOCK" "$1/state-exits.txt" exits timeout=15 owner=first shell="$1/shell.txt" || :
    check "$2 syscheck and its children ended, the same shell alone" "$1/state-exits.txt" "PASS exits_task_count" line
    check "$2 the shell owns the keyboard again" "$1/state-exits.txt" "PASS exits_keyboard_owner" line
    check "$2 the same shell task, process and id" "$1/state-exits.txt" "PASS exits_same_shell" line
    grep -qx "PASS exits_task_count" "$1/state-exits.txt" 2> /dev/null && ! grep -q '^FAIL' "$1/state-exits.txt"
}
# wait_recovery: until one snapshot shows the recovery run itself: "Blank loaded", the argv line
# "recovery" (the echoed "blank.elf recovery" doesn't match) and an exit below it. Counting exit
# rows is not enough: while the tty scrolls, one row can show up twice for a moment.
wait_recovery() {
    local deadline=$((SECONDS + 15))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        if grep -qF 'Blank loaded' "$Q_OUT/screen.txt" &&
            awk '$0 == "recovery" { r = 1 } r && /Process was terminated/ { f = 1 } END { exit !f }' "$Q_OUT/screen.txt"; then
            return 0
        fi
        sleep 0.2
    done
    echo "FAIL waiting for the recovery program"
    return 1
}
# outside_bytes <file>: the 16 physical bytes at 0x800000, memory no program owns, read by the
# monitor. The screen dump after "xp" is a monitor round trip: once it is back, the bytes are in the log.
outside_bytes() {
    local lines
    lines=$(wc -l < "$Q_OUT/monitor.log")
    qemu_cmd 'xp /16xb 0x800000'
    screen_snapshot
    { tail -n +"$((lines + 1))" "$Q_OUT/monitor.log" | tr -d '\r' | grep -aE '^0*80000[08]: ' | cut -d: -f2- || true; } > "$1"
    [ "$(wc -w < "$1")" = 16 ] || echo "missing" >> "$1"
}
run_syscheck() {
    local mode="$1" expected="$2" out="$OUT/session-$1" flags="${QEMU_FLAGS:-}"
    # "get" passes 0x800000 to syscall 8. QEMU's loader device puts known bytes there at reset.
    [ "$mode" != get ] || flags="$flags -device loader,addr=0x800000,data=0x8877665544332211,data-len=8"
    syscheck_setup
    SYSCHECK_SOCK="$SYSCHECK_GDB/gdb-$mode"
    QEMU_FLAGS="$flags -gdb unix:$SYSCHECK_SOCK,server=on,wait=off" qemu_start "$IMAGE" "$out"
    rm -f "$out"/state-*.txt "$out"/state-*.txt.log "$out/shell.txt"
    wait_screen "$RORTHOX_NAME v$RORTHOX_VERSION" 20 || return 1
    guest_state "$SYSCHECK_SOCK" "$out/state-boot.txt" boot owner=first save="$out/shell.txt" || return 1
    type_line "syscheck.elf $mode"
    wait_screen "syscheck: armed" 20 || return 1
    # The host compares the bytes at 0x800000 before and after.
    [ "$mode" != get ] || outside_bytes "$out/outside-before.txt"
    qemu_cmd 'sendkey a'
    case "$mode" in
        cmdbad|cmdself|cmdtwo|cmdfree*)
            wait_screen 'syscheck: key' 120 || return 1
            check "$mode no child" "$out/screen.txt" 'syscheck: child' absent
            qemu_cmd 'sendkey k'
            ;;
    esac
    wait_screen 'syscheck: continued' 120 || return 1
    if [ "$mode" = get ]; then
        outside_bytes "$out/outside-after.txt"
        echo "syscheck: outside $(cat "$out/outside-before.txt")" > "$out/outside.txt"
        grep -q missing "$out/outside-after.txt" && echo "after missing" >> "$out/outside.txt"
        cmp -s "$out/outside-before.txt" "$out/outside-after.txt" && echo unchanged >> "$out/outside.txt"
        check "$mode refused unowned memory" "$out/screen.txt" 'syscheck: outside -2' line
        check "$mode bytes at 0x800000 read before and after" "$out/outside.txt" 'missing' absent
        check "$mode bytes at 0x800000 are the loader's" "$out/outside.txt" 'syscheck: outside  0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88'
        check "$mode bytes at 0x800000 unchanged by syscall 8" "$out/outside.txt" 'unchanged' line
    fi
    case "$expected" in
        'syscheck: child PASS'|'raw: ran') wait_screen "$expected" 20 || return 1 ;;
    esac
    check "$mode result" "$out/screen.txt" "$expected"
    case "$mode" in
        free*|cmdfree*) check "$mode rejection" "$out/screen.txt" 'syscheck: freed result -2' ;;
    esac
    check "$mode no panic" "$out/debugcon.log" '[PANIC]' absent
    wait_exits "$out" "$mode" || return 1
    # A fresh positive control proves the shell/keyboard survived, not just the caller.
    type_line 'blank.elf recovery'
    wait_recovery || return 1
    check "$mode recovery argument" "$out/screen.txt" 'recovery' line
    check "$mode recovery program" "$out/screen.txt" 'Blank loaded'
    qemu_stop
}
