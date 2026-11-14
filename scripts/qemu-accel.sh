# The accelerator for every QEMU the tests start. Sourced by scripts/qemu-lib.sh and scripts/run-tests.sh.
#
#   RORTHOX_ACCEL=tcg    QEMU's own CPU emulation, TCG (the default)
#   RORTHOX_ACCEL=kvm    KVM with the split irqchip: only the local APIC is in the kernel, the PIC
#                        (isa-i8259), PIT (isa-pit) and I/O APIC are QEMU's own models, as under TCG.
#                        QEMU 8.0 and later refuse kernel-irqchip=off with KVM ("KVM does not support
#                        userspace APIC"). The PIC's interrupts reach the CPU through the local APIC's
#                        LINT0, which SeaBIOS sets to ExtINT, the same path the default in-kernel PIC uses.
#                        If a QEMU can't start with KVM, the run fails; it never falls back to TCG.
#   RORTHOX_ACCEL=auto   KVM if a QEMU started with it works (see accel_probe), else TCG
#
# The choice is made once, before the first test (by the runner) or before a test's first QEMU (a test
# run on its own), and passed on as RORTHOX_ACCEL=tcg|kvm with RORTHOX_ACCEL_REASON. A test that
# fails under KVM is never run again under TCG. QEMU_FLAGS may not choose an accelerator or a CPU;
# QEMU_CPU (default qemu32) is the CPU model.
#
# Needs only $QEMU (default qemu-system-i386), grep and coreutils (timeout, head, tail, cut, sort, tr).

ACCEL_KVM_OPTS="kvm,kernel-irqchip=split"

# accel_probe: 0 if a QEMU with KVM starts, its monitor says "kvm support: enabled" and its device
# tree has QEMU's own PIC and PIT (isa-i8259, isa-pit), not the in-kernel ones (kvm-i8259, kvm-pit).
# Sets accel_why and accel_probe_details (QEMU's output without the device tree, cut to 1200).
accel_probe() {
    local out status rest line devices
    out=$(printf 'info kvm\ninfo qtree\nquit\n' | timeout "${RORTHOX_ACCEL_PROBE_TIMEOUT:-30}" "${QEMU:-qemu-system-i386}" \
        -accel "$ACCEL_KVM_OPTS" -cpu "${QEMU_CPU:-qemu32}" -m 16 -display none -nodefaults -no-user-config \
        -serial none -S -monitor stdio 2>&1)
    status=$?
    # info qtree prints "bus: ..." and indented lines; "dev: <type>, id ..." names each device.
    devices=$(grep -oE 'dev: [a-z0-9-]*(i8259|pit|ioapic),' <<< "$out" | cut -c 6- | tr -d , | LC_ALL=C sort | tr '\n' ' ')
    devices=${devices% }
    rest=$(grep -vE '^(bus: |[[:space:]])' <<< "$out")
    accel_probe_details=${rest:0:1200}
    if [ "$status" -eq 0 ] && grep -q 'kvm support: enabled' <<< "$out"; then
        if [[ " $devices " == *" isa-i8259 "* && " $devices " == *" isa-pit "* &&
              " $devices " != *" kvm-i8259 "* && " $devices " != *" kvm-pit "* ]]; then
            accel_why="a QEMU with -accel $ACCEL_KVM_OPTS reported kvm support: enabled ($devices)"
            return 0
        fi
        accel_why="a QEMU with -accel $ACCEL_KVM_OPTS ran with KVM, but not with QEMU's PIC and PIT (${devices:-none in info qtree})"
        return 1
    fi
    # QEMU's own reason is its last message, e.g. "failed to initialize kvm: Permission denied"; at
    # most 1000 bytes of it, so the reason and the CI annotation made from it stay short.
    line=$(tr -d '\r' <<< "$rest" | grep -v '^(qemu)' | grep . | tail -n 1 | head -c 1000)
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
                if [ "${GITHUB_ACTIONS:-}" = true ]; then
                    # At most 1500 bytes before escaping (QEMU's line in accel_why is already cut to 1000).
                    local message
                    message=$(printf '%s' "RORTHOX_ACCEL=kvm, but KVM doesn't work here: $accel_why" | head -c 1500)
                    message=${message//'%'/'%25'}
                    message=${message//$'\r'/'%0D'}
                    message=${message//$'\n'/'%0A'}
                    printf '::error title=KVM unavailable::%s\n' "$message"
                fi
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
