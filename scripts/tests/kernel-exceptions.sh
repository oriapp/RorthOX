#!/usr/bin/env bash
#
# A CPU exception in kernel code, and a double fault from anywhere, must stop the machine with
# one line on debugcon:
#   exception: vector=0xVVVVVVVV error=0xEEEEEEEE[ cr2=0xAAAAAAAA for a page fault]
# then PANIC("Kernel exception") and a halt with interrupts off. No program may be ended or
# scheduled for it. A second exception or an NMI after that must go straight back to the halt
# without printing anything.
#
# Each case boots a fresh copy of bin/os.bin (with programs/faulttest added as fault.elf) with
# QEMU's gdbstub on a Unix socket; scripts/tests/lib/exceptions.py does the debugger side.
# bin/os.bin itself must not change.
#   early   right after idt_load (paging off, no task yet) the debugger puts a ud2 in the kernel's
#           path: #UD in ring 0, before kernel_page()/task state exist
#   nested  the same, and a ud2 at PANIC's first instruction: the second #UD must halt directly
#   kcopy   fault.elf passes a freed (unmapped) pointer to print: #PF in the kernel's copy, with
#           the program's page directory loaded. Then an NMI is injected into the halted kernel.
#   df      the debugger clears the present bit of the #GP gate, then fault.elf runs "cli" in
#           ring 3: #GP can't be delivered, which makes a double fault from ring 3
#
# Needs python3. Env: BOOT_TIMEOUT=60 (seconds to wait for the shell)  STEP_TIMEOUT=20
#      KERNEL_EXCEPTION_CASES="early nested kcopy df"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
STEP_TIMEOUT="${STEP_TIMEOUT:-20}"
CASES="${KERNEL_EXCEPTION_CASES:-early nested kcopy df}"
. ./scripts/qemu-lib.sh

OUT=build/tests/kernel-exceptions
PROGRAM=programs/faulttest/build/faulttest.elf
rm -rf "$OUT"
mkdir -p "$OUT"
command -v python3 > /dev/null || { echo "kernel-exceptions: python3 not found" >&2; exit 1; }
[ -f "$PROGRAM" ] || { echo "kernel-exceptions: $PROGRAM not found, run 'make all' first" >&2; exit 1; }

# debugger <log> <args...>: run exceptions.py, keep its output, return its status.
debugger() {
    local log="$1" status
    shift
    if python3 -B scripts/tests/lib/exceptions.py "$@" > "$log" 2>&1; then status=0; else status=$?; fi
    sed 's/^/  /' "$log"
    return "$status"
}

# halted <file>: wait until "info registers" shows the CPU halted, then write "halted" and
# "interrupts off" (or not) and the CR3 value into <file>.
halted() {
    local i regs= eflags
    for ((i = 0; i < 40; i++)); do
        qemu_running || break
        qemu_cmd "info registers"
        screen_snapshot      # a monitor round trip: the registers are in monitor.log after it
        regs=$(grep -a -o 'EFL=[0-9a-f]* .*HLT=[01]' "$Q_OUT/monitor.log" | tail -1 || true)
        echo "$regs" | grep -q 'HLT=1' && break
        sleep 0.25
    done
    eflags=$(echo "$regs" | sed -n 's/^EFL=\([0-9a-f]*\).*/\1/p')
    {
        echo "$regs" | grep -q 'HLT=1' && echo "halted"
        [ -n "$eflags" ] && (( (16#$eflags & 0x200) == 0 )) && echo "interrupts off"
        echo "cr3=$(grep -a -o 'CR3=[0-9a-f]*' "$Q_OUT/monitor.log" | tail -1 | cut -d= -f2)"
    } > "$1" || true
    echo "  registers: ${regs:-none}"
}

# counts <file>: number of exception lines and panics on debugcon.
counts() {
    {
        echo "exceptions=$(grep -a -c '^exception: ' "$D/debugcon.log" || true)"
        echo "panics=$(grep -a -c -F '[PANIC]' "$D/debugcon.log" || true)"
        echo "terminated=$(grep -cF "Process was terminated" "$D/screen.txt" 2> /dev/null || true)"
    } > "$1"
}

# key_when_running: send a key once the monitor reports the guest running again. The debugger
# has just continued it, and QEMU drops key presses that arrive while the guest is stopped.
key_when_running() {
    local i seen
    seen=$(grep -a -c 'VM status' "$Q_OUT/monitor.log" || true)
    for ((i = 0; i < 50; i++)); do
        qemu_cmd "info status"
        screen_snapshot      # a monitor round trip: the status line is in monitor.log after it
        if grep -a 'VM status' "$Q_OUT/monitor.log" | tail -n +$((seen + 1)) | grep -q 'VM status: running'; then
            qemu_cmd "sendkey k"
            return 0
        fi
        sleep 0.1
    done
    echo "  the guest did not run again after the debugger continued it"
    return 1
}

image_hash() { sha256sum bin/os.bin | cut -d' ' -f1; }
hash_before=$(image_hash)
cp bin/os.bin "$OUT/kernel-exceptions.bin"
mcopy -i "$OUT/kernel-exceptions.bin" "$PROGRAM" ::fault.elf
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

case_start() {
    D="$OUT/$1"
    mkdir -p "$D"
    echo "kernel-exceptions: $1"
    rm -f "$SOCK"
    QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off ${2:-}" qemu_start "$OUT/kernel-exceptions.bin" "$D"
}

# early / nested: #UD in ring 0 before paging and before any task.
run_early() {
    local c="$1" status
    case_start "$c" "-S"
    if debugger "$D/early.log" early "$SOCK" build/kernel.map "$D/early.txt" $([ "$c" = nested ] && echo nested); then
        status=0
    else
        status=$?
    fi
    echo "exit=$status" > "$D/status.txt"
    [ "$c" = early ] && { wait_screen "A problem has been detected" "$STEP_TIMEOUT" || true; }
    halted "$D/halt.txt"
    qemu_stop || true
    counts "$D/counts.txt"
    local r="$D/early.txt"
    check "$c: debugger session finished without errors"           "$D/status.txt" "exit=0" line
    check "$c: stopped after idt_load: paging off, IF=0, no task"   "$r" "PASS after_idt_load" line
    check "$c: ud2 entered the #UD stub from ring 0"                "$r" "PASS ud2_stub" line
    check "$c: interrupt_handler got vector 6, error 0"             "$r" "PASS ud2_handler_frame" line
    check "$c: PANIC(\"Kernel exception\")"                         "$r" "PASS ud2_panic" line
    check "$c: CR3 untouched by the exception path"                 "$r" "PASS cr3_untouched" line
    check "$c: CPU halted"                                          "$D/halt.txt" "halted" line
    check "$c: interrupts off"                                      "$D/halt.txt" "interrupts off" line
    if [ "$c" = early ]; then
        check "$c: exception line on debugcon"     "$D/debugcon.log" "exception: vector=0x00000006 error=0x00000000" line
        check "$c: one exception line"             "$D/counts.txt" "exceptions=1" line
        check "$c: panic message on debugcon"      "$D/debugcon.log" "[PANIC]: Kernel exception at ./kernel/idt/idt.c:"
        check "$c: one panic"                      "$D/counts.txt" "panics=1" line
        check "$c: panic screen shown"             "$D/screen.txt" "A problem has been detected"
        check "$c: halted in PAUSE"                "$r" "PASS halted_in_pause" line
    else
        check "$c: second ud2 (at PANIC) entered the #UD stub"         "$r" "PASS nested_stub" line
        check "$c: second exception went straight to PAUSE"            "$r" "PASS nested_straight_to_pause" line
        check "$c: one exception line only"        "$D/counts.txt" "exceptions=1" line
        check "$c: no panic printed (PANIC never ran)" "$D/counts.txt" "panics=0" line
    fi
}

# kcopy: #PF in the kernel while it copies a user string, then an NMI into the halted kernel.
run_kcopy() {
    local status nmi_status="not run"
    case_start kcopy
    rm -f "$D/ready"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
        type_line "fault.elf kcopy"
        if wait_screen "faulttest: armed" "$STEP_TIMEOUT"; then
            python3 -B scripts/tests/lib/exceptions.py catch "$SOCK" build/kernel.map "$D/catch.txt" "$D/ready" kcopy \
                > "$D/catch.log" 2>&1 &
            local py=$! i
            for ((i = 0; i < 100; i++)); do [ -f "$D/ready" ] && break; sleep 0.1; done
            [ -f "$D/ready" ] && { key_when_running || true; }
            if wait "$py"; then status=0; else status=$?; fi
            sed 's/^/  /' "$D/catch.log"
            echo "exit=$status" > "$D/status.txt"
            if [ "$status" = 0 ] && wait_screen "A problem has been detected" "$STEP_TIMEOUT"; then
                halted "$D/halt.txt"
                counts "$D/counts-before-nmi.txt"
                if debugger "$D/nmi.log" nmi "$SOCK" build/kernel.map "$D/nmi.txt"; then nmi_status=0; else nmi_status=$?; fi
                halted "$D/halt-after-nmi.txt"
            fi
        fi
    fi
    echo "exit=$nmi_status" > "$D/nmi-status.txt"
    qemu_stop || true
    counts "$D/counts.txt"
    local r="$D/catch.txt" n="$D/nmi.txt"
    local cpu_cr2
    cpu_cr2=$(sed -n 's/^ *cpu cr2 \(0x[0-9a-f]\{8\}\)$/\1/p' "$D/catch.log" | head -1)
    check "kcopy: debugger session finished without errors"         "$D/status.txt" "exit=0" line
    check "kcopy: entered the #PF stub"                              "$r" "PASS stub_vector" line
    check "kcopy: fault came from ring 0"                            "$r" "PASS cpu_ring0" line
    check "kcopy: error 0 (kernel read, page not present)"          "$r" "PASS cpu_error_code" line
    check "kcopy: CR2 is the pointer passed to print"               "$r" "PASS cpu_cr2" line
    check "kcopy: interrupt_handler got vector 14, error, cr2"      "$r" "PASS handler_frame" line
    check "kcopy: PANIC(\"Kernel exception\"), no process ended"    "$r" "PASS kernel_panic" line
    check "kcopy: halted in PAUSE, process_terminate never called"  "$r" "PASS halted_in_pause" line
    # The exact line, with the CR2 the CPU had at the stub.
    check "kcopy: exception line with vector 14, error 0 and cr2"   "$D/debugcon.log" \
        "exception: vector=0x0000000e error=0x00000000 cr2=${cpu_cr2:-unknown}" line
    check "kcopy: panic message on debugcon"                        "$D/debugcon.log" "[PANIC]: Kernel exception at ./kernel/idt/idt.c:"
    check "kcopy: panic screen shown"                               "$D/screen.txt" "A problem has been detected"
    check "kcopy: program not ended"                                "$D/counts.txt" "terminated=0" line
    check "kcopy: program not resumed"                              "$D/screen.txt" "faulttest: survived" absent
    check "kcopy: CPU halted"                                       "$D/halt.txt" "halted" line
    check "kcopy: interrupts off"                                   "$D/halt.txt" "interrupts off" line
    check "nmi: debugger session finished without errors"           "$D/nmi-status.txt" "exit=0" line
    check "nmi: halted before the NMI"                              "$n" "PASS halted_before_nmi" line
    check "nmi: NMI entered its stub from the halt loop"            "$n" "PASS nmi_stub" line
    check "nmi: went straight back to PAUSE, no other exception"    "$n" "PASS nmi_straight_to_pause" line
    check "nmi: CPU halted again"                                   "$D/halt-after-nmi.txt" "halted" line
    check "nmi: interrupts still off"                               "$D/halt-after-nmi.txt" "interrupts off" line
    check "nmi: no second exception line"                           "$D/counts.txt" "exceptions=1" line
    check "nmi: no second panic"                                    "$D/counts.txt" "panics=1" line
}

# df: #GP gate not present, ring-3 cli: double fault.
run_df() {
    local status
    case_start df
    rm -f "$D/ready"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
        type_line "fault.elf cli"
        if wait_screen "faulttest: armed" "$STEP_TIMEOUT"; then
            python3 -B scripts/tests/lib/exceptions.py catch "$SOCK" build/kernel.map "$D/catch.txt" "$D/ready" df \
                > "$D/catch.log" 2>&1 &
            local py=$! i
            for ((i = 0; i < 100; i++)); do [ -f "$D/ready" ] && break; sleep 0.1; done
            [ -f "$D/ready" ] && { key_when_running || true; }
            if wait "$py"; then status=0; else status=$?; fi
            sed 's/^/  /' "$D/catch.log"
            echo "exit=$status" > "$D/status.txt"
            wait_screen "A problem has been detected" "$STEP_TIMEOUT" || true
            halted "$D/halt.txt"
        fi
    fi
    qemu_stop || true
    counts "$D/counts.txt"
    local r="$D/catch.txt"
    check "df: debugger session finished without errors"            "$D/status.txt" "exit=0" line
    check "df: #GP gate made not present"                           "$r" "PASS gp_gate_cleared" line
    check "df: entered the #DF stub (not #GP or #NP)"               "$r" "PASS stub_vector" line
    check "df: error code 0"                                        "$r" "PASS cpu_error_code" line
    check "df: saved CS is ring 3"                                  "$r" "PASS cpu_ring3" line
    check "df: interrupt_handler got vector 8, error 0"             "$r" "PASS handler_frame" line
    check "df: PANIC(\"Kernel exception\") although from ring 3"    "$r" "PASS kernel_panic" line
    check "df: halted in PAUSE, process_terminate never called"     "$r" "PASS halted_in_pause" line
    check "df: exception line on debugcon"          "$D/debugcon.log" "exception: vector=0x00000008 error=0x00000000" line
    check "df: one exception line"                  "$D/counts.txt" "exceptions=1" line
    check "df: panic message on debugcon"           "$D/debugcon.log" "[PANIC]: Kernel exception at ./kernel/idt/idt.c:"
    check "df: one panic"                           "$D/counts.txt" "panics=1" line
    check "df: program not ended"                   "$D/counts.txt" "terminated=0" line
    check "df: CPU halted"                          "$D/halt.txt" "halted" line
    check "df: interrupts off"                      "$D/halt.txt" "interrupts off" line
}

for c in $CASES; do
    case "$c" in
        early | nested) run_early "$c" ;;
        kcopy) run_kcopy ;;
        df) run_df ;;
        *) echo "kernel-exceptions: unknown case $c" >&2; exit 1 ;;
    esac
done
rm -rf "$GDB_DIR"

hash_after=$(image_hash)
[ "$hash_before" = "$hash_after" ] && echo "unchanged" > "$OUT/image.txt" || echo "changed" > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
finish kernel-exceptions
