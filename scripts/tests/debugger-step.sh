#!/usr/bin/env bash
#
# The gdbstub client (scripts/tests/lib/rsp.py) steps off a breakpoint before it continues. QEMU
# can report a single step that did not run the instruction (eip unchanged); the client then
# stopped at the same breakpoint again, and a heap copy test saw one request twice. Host check
# only, no QEMU: scripts/tests/lib/rsp_step.py runs Rsp.cont() against a fake gdbstub where the
# first step stalls (one stop per breakpoint, both breakpoints back in place), where steps always
# move, and where no step moves (cont() fails within a few steps instead of continuing).
# The same client has to work under KVM, where QEMU's gdbstub behaves differently: registers and
# memory are read from a copy only "g" refreshes (a register write before it is lost, memory goes
# through an old CR3), there are 4 hardware breakpoints, and a step after the debugger moved eip
# runs freely. A fake with those rules checks that the client syncs after every kind of stop,
# refuses a fifth breakpoint (or a second one at the same address) and a step after an eip change,
# and steps off a breakpoint with IF masked (refusing the instructions where that would change what
# runs).

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check()

OUT=${RORTHOX_TEST_ROOT:-build/tests}/debugger-step
rm -rf "$OUT"
mkdir -p "$OUT"
RESULTS="$OUT/results.txt"
: > "$RESULTS"

echo "debugger-step: the gdbstub client against a fake gdbstub"
if timeout 60 python3 -B scripts/tests/lib/rsp_step.py "$RESULTS" > "$OUT/check.log" 2>&1; then
    status=0
else
    status=$?
fi
sed 's/^/  /' "$OUT/check.log"
echo "exit=$status" > "$OUT/status.txt"

check "fake gdbstub check exited with status 0"                 "$OUT/status.txt" "exit=0" line
check "steps that move: one stop per breakpoint, one step"      "$RESULTS" "PASS normal" line
check "first step stalls: the breakpoint is not reported twice" "$RESULTS" "PASS stall_one_delivery" line
check "first step stalls: stepped again once"                   "$RESULTS" "PASS stall_steps" line
check "first step stalls: breakpoint put back before continuing" "$RESULTS" "PASS stall_breakpoints_back" line
check "no step moves: cont() fails naming the breakpoint"       "$RESULTS" "PASS stuck_fails" line
check "no step moves: gives up within a few steps"              "$RESULTS" "PASS stuck_bounded" line
check "no step moves: the guest is not continued"               "$RESULTS" "PASS stuck_not_continued" line
check "KVM copy: each stop's eip is read after a sync"           "$RESULTS" "PASS kvm_stale_read" line
check "KVM copy: a register write at a stop reaches the guest"   "$RESULTS" "PASS kvm_lost_write" line
check "KVM copy: memory goes through the stop's own CR3"         "$RESULTS" "PASS kvm_memory_cr3" line
check "KVM copy: a test's own continue and break-in sync too"    "$RESULTS" "PASS kvm_raw_stop" line
check "KVM copy: only info registers counts as a sync"          "$RESULTS" "PASS kvm_monitor" line
check "KVM copy: info registers first, eip unchanged: may step" "$RESULTS" "PASS kvm_monitor_first_step" line
check "a fifth hardware breakpoint is refused before the stub"   "$RESULTS" "PASS bp_limit_fifth_refused" line
check "a removed breakpoint frees its slot, step-off with four"  "$RESULTS" "PASS bp_limit_slot_reused" line
check "the KVM fake refuses a fifth breakpoint itself"           "$RESULTS" "PASS kvm_bp_limit" line
check "a second breakpoint at the same address is refused"      "$RESULTS" "PASS bp_duplicate_refused" line
check "no single step after the debugger moved eip"             "$RESULTS" "PASS moved_eip_step_refused" line
check "continuing from a moved eip still works"                 "$RESULTS" "PASS moved_eip_cont_runs" line
check "eip moved back to the stop: stepping works again"         "$RESULTS" "PASS moved_back_steps" line
check "IF set: the step off a breakpoint runs with IF clear"     "$RESULTS" "PASS if_masked_in_step" line
check "IF set: IF is back when the guest continues"             "$RESULTS" "PASS if_restored" line
check "IF set: no step off an instruction that uses IF"          "$RESULTS" "PASS if_opcodes_refused" line
check "IF set: mov ds is stepped off with IF masked"            "$RESULTS" "PASS if_mov_ds_steps" line
check "IF set: a step that ends in another CS fails"            "$RESULTS" "PASS if_cs_change_refused" line
check "IF set: a failed step off puts IF back"                  "$RESULTS" "PASS if_restored_after_failure" line
check "IF set: step off the last mapped byte"                   "$RESULTS" "PASS if_last_byte_steps" line
check "IF clear: a pushf is stepped off as usual"                "$RESULTS" "PASS if_clear_steps" line
if [ "$failures" -ne 0 ]; then
    echo "debugger-step: $failures check(s) failed" >&2
    exit 1
fi
echo "debugger-step: all checks passed"
