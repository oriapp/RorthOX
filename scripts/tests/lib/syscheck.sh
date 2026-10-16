# Shared ring-3 probe session. The caller sets OUT and prepares IMAGE.
# Each invocation of a QEMU-bearing test must run under timeout 120.
wait_exits() {
    local count="$1" deadline=$((SECONDS + 15))
    while ((SECONDS < deadline)); do
        qemu_running || return 1
        screen_snapshot
        if [ "$(grep -cF 'Process was terminated' "$Q_OUT/screen.txt" || true)" -ge "$count" ]; then return 0; fi
        sleep 0.2
    done
    echo "FAIL waiting for $count process exits"
    return 1
}
run_syscheck() {
    local mode="$1" expected="$2" out="$OUT/session-$1"
    qemu_start "$IMAGE" "$out"
    wait_screen "$RORTHOX_NAME v$RORTHOX_VERSION" 20 || return 1
    type_line "syscheck.elf $mode"
    wait_screen "syscheck: armed" 20 || return 1
    qemu_cmd 'sendkey a'
    case "$mode" in
        cmdbad|cmdself|cmdtwo|cmdfree*)
            wait_screen 'syscheck: key' 120 || return 1
            check "$mode no child" "$out/screen.txt" 'syscheck: child' absent
            qemu_cmd 'sendkey k'
            ;;
    esac
    wait_screen 'syscheck: continued' 120 || return 1
    case "$expected" in
        'syscheck: child PASS'|'raw: ran') wait_screen "$expected" 20 || return 1 ;;
    esac
    check "$mode result" "$out/screen.txt" "$expected"
    case "$mode" in
        free*|cmdfree*) check "$mode rejection" "$out/screen.txt" 'syscheck: freed result -2' ;;
    esac
    check "$mode no panic" "$out/debugcon.log" '[PANIC]' absent
    local exits=1
    case "$expected" in 'syscheck: child PASS'|'raw: ran') exits=2;; esac
    wait_exits "$exits" || return 1
    # A fresh positive control proves the shell/keyboard survived, not just the caller.
    type_line 'blank.elf recovery'
    wait_exits "$((exits+1))" || return 1
    check "$mode recovery argument" "$out/screen.txt" 'recovery' line
    check "$mode recovery program" "$out/screen.txt" 'Blank loaded'
    qemu_stop
}
