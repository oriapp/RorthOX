#!/usr/bin/env bash
#
# The gdbstub client (scripts/tests/lib/rsp.py) works around what QEMU's gdbstub does under KVM:
# registers and memory come from a copy only "g" refreshes, there are 4 hardware breakpoints, a
# single step after the debugger moved eip runs freely, and a step off a breakpoint with IF set
# can go into an interrupt handler. debugger-step.sh checks the client against a fake gdbstub;
# this test checks the same on the real one, under the accelerator the run uses (RORTHOX_ACCEL).
# scripts/tests/lib/kvm_debugger.py does the debugger side; its docstring lists the cases. Under
# TCG the gdbstub has none of these traps, so there the test checks the client's own guards (the
# packets it sends) and that a step off with a key pending lands on the next instruction without
# taking the key (under KVM an interrupt inside the step would not change where it ends). Under
# KVM the stub's answers are checked too (a fifth breakpoint refused), and the KVM-only behaviour
# is recorded in steps.txt (a stale "p" before "g", a step over a faulting instruction).
# Needs python3. Env: BOOT_TIMEOUT=60

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/kvm-debugger
GDB_DIR=
cleanup() {
    if [ -n "$GDB_DIR" ]; then rm -rf "$GDB_DIR"; fi
}
Q_CLEANUP=cleanup
trap qemu_exit EXIT
trap 'exit 1' TERM INT
trap 'echo "FAIL kvm-debugger: command failed at line $LINENO" >&2' ERR

rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "FAIL kvm-debugger: python3 not found" >&2; exit 1; }
# The kernel's instructions, for the step checks: the same link as bin/kernel.bin, as an ELF file.
i686-elf-ld -L build/config -T kernel/linker.ld --oformat elf32-i386 build/kernelfull.o -o "$OUT/kernel.elf"
i686-elf-objdump -D -j .text -j .asm "$OUT/kernel.elf" > "$OUT/kernel.dis"
cp bin/os.bin "$OUT/os.bin"
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "kvm-debugger: sync, breakpoints and steps on the real gdbstub"
QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/os.bin" "$OUT"
status="not run"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    if timeout 300 python3 -B scripts/tests/lib/kvm_debugger.py "$SOCK" build/kernel.map "$OUT/kernel.dis" \
        programs/shell/build/shell.elf "$OUT/debugcon.log" "$OUT/results.txt" "$RORTHOX_ACCEL" \
        > "$OUT/steps.txt" 2>&1; then
        status=0
    else
        status=$?
    fi
    sed 's/^/  /' "$OUT/steps.txt"
fi
echo "exit=$status" > "$OUT/status.txt"
qemu_stop || true
R="$OUT/results.txt"
touch "$R"
check "debugger session finished without errors"             "$OUT/status.txt" "exit=0" line
check "single steps let interrupts in (as KVM)"               "$R" "PASS sstep_irqs" line
check "sync: stopped at task_return"                          "$R" "PASS sync_stop" line
check "sync: the recorded p eip is the first packet after it" "$R" "PASS sync_raw_p_first" line
check "sync: the next stop reads the guest's ebx"             "$R" "PASS sync_guest_ebx" line
check "sync: a register written at a stop reaches the CPU"    "$R" "PASS sync_write_reached" line
check "sync: g before any register or memory access"          "$R" "PASS sync_g_first" line
check "no single step after eip was moved"                    "$R" "PASS moved_eip_step_refused" line
check "a fifth breakpoint is refused before it is sent"       "$R" "PASS z1_fifth_refused_by_client" line
check "the stub's answer to a fifth breakpoint (KVM: refused)" "$R" "PASS z1_fifth_stub" line
check "200+ ring-0 steps, one instruction each"               "$R" "PASS steps_one_insn" line
check "step-off breakpoint is mov eax, imm32"                 "$R" "PASS stepoff_insn" line
check "IF set, key pending: step-offs land on the next insn"  "$R" "PASS stepoff_lands_next" line
check "a key was pending in each stop and taken after it"     "$R" "PASS stepoff_keys" line
check "the key is still pending right after each step-off"   "$R" "PASS stepoff_key_kept" line
check "no exception in the guest from the step-offs"          "$R" "PASS stepoff_no_guest_db" line
check "a step over ud2 ends, cont() raises"                   "$R" "PASS fault_step_ends" line
finish kvm-debugger
