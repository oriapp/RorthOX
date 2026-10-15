#!/usr/bin/env bash
#
# Every interrupt stub must hand the C code the same frame layout (struct interrupt_frame):
# general registers, then an error code (the CPU's for vectors 8, 10-14, 17, 21, 29 and 30, a 0
# pushed by the stub for every other vector and for the syscall stub), then eip, cs, eflags.
# Only those error-code gates are DPL 0, so ring 3 can't enter them with "int n" (which pushes no
# error code); every other gate stays DPL 3.
#
# QEMU starts stopped with its gdbstub on a Unix socket, on a copy of bin/os.bin. At the first
# task_return, scripts/tests/lib/exceptions.py layout reads the IDT, then enters each of the 256
# stubs and the syscall stub with a frame laid out like the CPU's (same privilege, error code only
# for the error-code vectors) and checks the vector, frame and CR2 that interrupt_handler (or
# isr80h_handler) gets. It also runs the syscall stub, an unused vector and the NMI stub (ring 0:
# no task switch, segment registers kept) back out to check that the error code is popped. This
# also covers stubs whose vectors the test CPU never raises (#CP 21, #VC 29, #SX 30): it checks
# the stub, not CPU support. Afterwards the boot goes on and blank.elf must run.
# bin/os.bin itself must not change.
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)  STEP_TIMEOUT=20

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
. ./scripts/qemu-lib.sh

OUT=build/tests/exception-frames
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "exception-frames: python3 not found" >&2; exit 1; }

# wait_lines <regex> <count> <seconds>: wait until <count> screen lines match <regex>.
wait_lines() {
    local regex="$1" count="$2" deadline=$((SECONDS + $3)) n
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for /$regex/ x$count"; return 1; }
        screen_snapshot
        n=$(grep -cE -- "$regex" "$Q_OUT/screen.txt" 2> /dev/null || true)
        if [ "${n:-0}" -ge "$count" ]; then
            echo "  /$regex/ x$count on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for /$regex/ x$count after $3 s"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/exception-frames.bin"
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

echo "exception-frames: boot, enter every stub"
QEMU_FLAGS="-S -gdb unix:$SOCK,server=on,wait=off" qemu_start "$OUT/exception-frames.bin" "$OUT"
start=$SECONDS
if python3 -B scripts/tests/lib/exceptions.py layout "$SOCK" build/kernel.map "$OUT/layout.txt" \
    > "$OUT/layout.log" 2>&1; then status=0; else status=$?; fi
sed 's/^/  /' "$OUT/layout.log"
echo "  debugger session took $((SECONDS - start)) s"
echo "exit=$status" > "$OUT/status.txt"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "blank.elf frames"
    wait_lines '^frames$' 1 "$STEP_TIMEOUT" && wait_lines 'Process was terminated' 1 "$STEP_TIMEOUT" || true
fi
qemu_stop || true
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"

r="$OUT/layout.txt"
check "debugger session finished without errors"                 "$OUT/status.txt" "exit=0" line
check "every IDT gate points at its stub, kernel code selector"  "$r" "PASS idt_targets" line
check "DPL 0 only on gates 8, 10-14, 17, 21, 29, 30"             "$r" "PASS idt_gate_dpl" line
check "all 256 stubs: vector, frame, error code, cr2 for C"      "$r" "PASS stub_frames" line
check "syscall stub: command and frame (error code 0) for C"     "$r" "PASS syscall_frame" line
check "syscall stub returns with the error code popped"          "$r" "PASS syscall_return" line
check "unused vector returns with the error code popped"         "$r" "PASS unused_vector_return" line
check "ring-0 NMI returns: no save, no switch, segments kept"    "$r" "PASS kernel_nmi_return" line
check "ring-0 NMI leaves CR3 alone"                              "$r" "PASS kernel_nmi_cr3" line
check "shell started afterwards"                                 "$OUT/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "blank.elf ran afterwards"                                 "$OUT/screen.txt" "frames" line
check "no kernel panic (debugcon)"                               "$OUT/debugcon.log" "PANIC" absent
check "bin/os.bin not changed by the test"                       "$OUT/image.txt" "unchanged" line
finish exception-frames
