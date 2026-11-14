# The accelerator for every QEMU the tests start. Sourced by scripts/qemu-lib.sh and scripts/run-tests.sh.
#
#   RORTHOX_ACCEL=tcg    QEMU's own CPU emulation, TCG (the default)
#   RORTHOX_ACCEL=kvm    KVM, with QEMU's PIC and PIT (kernel-irqchip=off: the same device models as TCG).
#                        If a QEMU can't start with KVM, the run fails; it never falls back to TCG.
#   RORTHOX_ACCEL=auto   KVM if a QEMU started with it reports "kvm support: enabled", else TCG
#
# The choice is made once, before the first test (by the runner) or before a test's first QEMU (a test
# run on its own), and passed on as RORTHOX_ACCEL=tcg|kvm with RORTHOX_ACCEL_REASON. A test that
# fails under KVM is never run again under TCG. QEMU_FLAGS may not choose an accelerator or a CPU;
# QEMU_CPU (default qemu32) is the CPU model.
#
# Needs only $QEMU (default qemu-system-i386), timeout and grep.

ACCEL_KVM_OPTS="kvm,kernel-irqchip=off"

# accel_probe: 0 if a QEMU with KVM starts and its monitor says "kvm support: enabled". Sets accel_why.
accel_probe() {
    local out status line
    out=$(printf 'info kvm\nquit\n' | timeout "${RORTHOX_ACCEL_PROBE_TIMEOUT:-30}" "${QEMU:-qemu-system-i386}" \
        -accel "$ACCEL_KVM_OPTS" -cpu "${QEMU_CPU:-qemu32}" -m 16 -display none -nodefaults -no-user-config \
        -serial none -S -monitor stdio 2>&1)
    status=$?
    if [ "$status" -eq 0 ] && grep -q 'kvm support: enabled' <<< "$out"; then
        accel_why="a QEMU with -accel $ACCEL_KVM_OPTS reported kvm support: enabled"
        return 0
    fi
    # QEMU's own reason is its last message, e.g. "failed to initialize kvm: Permission denied".
    line=$(tr -d '\r' <<< "$out" | grep -v '^(qemu)' | grep . | tail -n 1)
    accel_why="a QEMU with -accel $ACCEL_KVM_OPTS did not run with KVM (exit $status${line:+: $line})"
    return 1
}

# accel_select: turn RORTHOX_ACCEL into tcg or kvm and set RORTHOX_ACCEL_REASON (both exported).
# 1 with a message on stderr when kvm is asked for and doesn't work, or the value is not one of the three.
accel_select() {
    case "${RORTHOX_ACCEL:-tcg}" in
        tcg)
            if [ -n "${RORTHOX_ACCEL:-}" ]; then RORTHOX_ACCEL_REASON="RORTHOX_ACCEL=tcg"
            else RORTHOX_ACCEL_REASON="TCG is the default"; fi
            RORTHOX_ACCEL=tcg ;;
        kvm)
            if ! accel_probe; then
                echo "RORTHOX_ACCEL=kvm, but KVM doesn't work here: $accel_why" >&2
                return 1
            fi
            RORTHOX_ACCEL=kvm
            RORTHOX_ACCEL_REASON="RORTHOX_ACCEL=kvm: $accel_why" ;;
        auto)
            if accel_probe; then RORTHOX_ACCEL=kvm; else RORTHOX_ACCEL=tcg; fi
            RORTHOX_ACCEL_REASON="RORTHOX_ACCEL=auto: $accel_why" ;;
        *)
            echo "RORTHOX_ACCEL must be auto, tcg or kvm, not '$RORTHOX_ACCEL'" >&2
            return 1 ;;
    esac
    export RORTHOX_ACCEL RORTHOX_ACCEL_REASON
}

# accel_flags_ok <flags>: 1 with a message if the flags choose an accelerator or a CPU themselves.
accel_flags_ok() {
    if [[ " $1 " =~ [[:space:]]--?(accel|enable-kvm|cpu)([[:space:]=]) || $1 == *accel=* ]]; then
        echo "QEMU_FLAGS may not choose the accelerator or the CPU ('$1'): use RORTHOX_ACCEL and QEMU_CPU" >&2
        return 1
    fi
    return 0
}
