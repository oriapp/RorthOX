#!/usr/bin/env bash
#
# The gdbstub client (scripts/tests/lib/rsp.py) steps off a breakpoint before it continues. QEMU
# can report a single step that did not run the instruction (eip unchanged); the client then
# stopped at the same breakpoint again, and a heap copy test saw one request twice. Host check
# only, no QEMU: scripts/tests/lib/rsp_step.py runs Rsp.cont() against a fake gdbstub where the
# first step stalls (one stop per breakpoint, both breakpoints back in place), where steps always
# move, and where no step moves (cont() fails within a few steps instead of continuing).

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check()

OUT=build/tests/debugger-step
rm -rf "$OUT"
mkdir -p "$OUT"
RESULTS="$OUT/results.txt"
: > "$RESULTS"

echo "debugger-step: Rsp.cont() against a fake gdbstub"
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
if [ "$failures" -ne 0 ]; then
    echo "debugger-step: $failures check(s) failed" >&2
    exit 1
fi
echo "debugger-step: all checks passed"
