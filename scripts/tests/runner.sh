#!/usr/bin/env bash
#
# The test runner itself (scripts/run-tests.sh), with small fixture tests instead of QEMU:
# 1. pass, skip, fail, two crashes (the test, and the shell the runner started for it), a test that
#    hangs and a test that leaves processes running, at -j 3: each exit status is recorded as the
#    test gave it, the hang is stopped at --timeout, the test that left processes fails, and after
#    the runner returns nothing either test started is left, also not a child in its own process
#    group (timeout), one that ignores TERM or one in a session of its own with the test's TMPDIR in
#    its command line. Only the runner writes CI error annotations: one per
#    failing test, and no line a test prints (::error, a carriage return and then ::error,
#    ::stop-commands, ::add-mask) starts a line of the runner's output.
# 2. -j 3 runs three tests at once and never four; -j 1 (and no -j) runs one after the other in
#    the given order, a test named twice twice.
# 3. TERM or INT to the runner stops every running test and everything it started (a test that
#    exits 0 on TERM still fails) and starts no more; the runner exits 1.
# 4. A bad command line (-j 0, -j x, --timeout 0, an unknown option, a missing test) exits 2
#    and runs nothing; --list prints the tests and runs nothing.

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
runner="$PWD/scripts/run-tests.sh"
OUT=${RORTHOX_TEST_ROOT:-build/tests}/runner
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fx="$tmp/fixtures"
mkdir -p "$fx"
export RUNNER_STATE="$tmp/state" GITHUB_STEP_SUMMARY="$tmp/summary.md"
failures=0
cr=$'\r'
nl=$'\n'

ok() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; failures=$((failures + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

# run <case> <runner arguments>...: run the runner; its output goes to $OUT/<case>.log, its runs to $OUT/<case>/.
run() {
    local name="$1"
    shift
    mkdir -p "$OUT/$name" "$RUNNER_STATE"
    if RORTHOX_TEST_ROOT="$OUT/$name" timeout -k 5 120 "$runner" "$@" > "$OUT/$name.log" 2>&1; then status=0; else status=$?; fi
    durations=$(cat "$OUT/$name"/runs/run.*/durations.txt 2> /dev/null || true)
}

# Nothing of the pids in the files given is still there (not even as a zombie): no such process,
# and no process in a process group or session of that number.
all_gone() {
    local f p
    for f in "$@"; do
        while read -r p; do
            [ -n "$p" ] || continue
            if kill -0 "$p" 2> /dev/null || pgrep -g "$p" > /dev/null || pgrep -s "$p" > /dev/null; then
                echo "  left: $(ps -o pid=,pgid=,sid=,args= -g "$p" -s "$p" -p "$p")"
                return 1
            fi
        done < "$f"
    done
    return 0
}

cat > "$fx/pass.sh" <<'EOF'
touch "$RUNNER_STATE/ran.$$"
echo pass
EOF
printf 'touch "$RUNNER_STATE/ran.$$"\necho skip\nexit 77\n' > "$fx/skip.sh"
printf 'echo "  FAIL thing"\nexit 3\n' > "$fx/fail.sh"
printf 'echo before the crash\nkill -KILL $$\n' > "$fx/crash.sh"
# kill-parent: the shell the runner started for the test dies of KILL, not the test.
printf 'echo killing\nkill -KILL $PPID\nsleep 1\n' > "$fx/kill-parent.sh"
# hang: runs until it is stopped; one child in its own process group (timeout does that), and with
# RUNNER_STUBBORN=1 one that ignores TERM.
cat > "$fx/hang.sh" <<'EOF'
p="$RUNNER_STATE/hang.$$"
timeout 600 sleep 600 &
echo $! >> "$p.part"
if [ "${RUNNER_STUBBORN:-}" = 1 ]; then
    bash -c 'trap "" TERM; while :; do sleep 0.2; done' &
    echo $! >> "$p.part"
fi
echo $$ >> "$p.part"
mv "$p.part" "$p.pids"
echo hanging
wait
EOF
# spawn: passes, but leaves a child, a timeout group and a process in a session of its own that
# has the test's TMPDIR in its command line, as QEMU has.
cat > "$fx/spawn.sh" <<'EOF'
p="$RUNNER_STATE/spawn.$$.pids"
sleep 600 &
echo $! >> "$p"
timeout 600 sleep 600 &
echo $! >> "$p"
printf 'mkfifo "$0.fifo"; exec 3<> "$0.fifo"; read -r -t 600 -u 3 _\n' > "$TMPDIR/sleeper"
setsid bash "$TMPDIR/sleeper" &
echo $! >> "$p"
echo spawned
EOF
cat > "$fx/hostile.sh" <<'EOF'
printf '  ::error::x\n'
printf 'ok\r::error::x\n'
printf '::stop-commands::x\n'
printf '::add-mask::x\n'
printf '::warning::no newline'
EOF
# barrier: wait (up to 10 s) until $BARRIER tests run at once, or one of them saw that already;
# record the most seen at once.
cat > "$fx/barrier.sh" <<'EOF'
s="$RUNNER_STATE"
touch "$s/active.$$"
max=0
for ((i = 0; i < 200; i++)); do
    set -- "$s"/active.*
    [ $# -gt "$max" ] && max=$#
    [ $# -ge "$BARRIER" ] && touch "$s/reached"
    [ -e "$s/reached" ] && break
    sleep 0.05
done
for ((i = 0; i < 6; i++)); do
    set -- "$s"/active.*
    [ $# -gt "$max" ] && max=$#
    sleep 0.05
done
echo "$max" > "$s/max.$$"
rm -f "$s/active.$$"
EOF
for n in a b c; do
    printf 'echo "start %s" >> "$RUNNER_STATE/seq"\nsleep 0.2\necho "end %s" >> "$RUNNER_STATE/seq"\n' "$n" "$n" > "$fx/$n.sh"
done

echo "runner: 1. statuses, time limit, leftovers and CI output at -j 3"
RUNNER_STUBBORN=1 GITHUB_ACTIONS=true run mixed -j 3 --timeout 2 \
    "$fx/pass.sh" "$fx/skip.sh" "$fx/fail.sh" "$fx/crash.sh" "$fx/kill-parent.sh" "$fx/hang.sh" "$fx/spawn.sh" \
    "$fx/hostile.sh"
L="$OUT/mixed.log"
expect "exit status 1" '[ "$status" -eq 1 ]'
expect "summary" 'grep -Fxq "=== 2 passed, 5 failed, 1 skipped" "$L"'
for want in "0 $fx/pass.sh" "77 $fx/skip.sh" "3 $fx/fail.sh" "137 $fx/crash.sh" "137 $fx/kill-parent.sh" \
    "124 $fx/hang.sh" "1 $fx/spawn.sh" "0 $fx/hostile.sh"; do
    expect "recorded status ${want%% *} for $(basename "${want#* }")" \
        'printf "%s\n" "$durations" | cut -d" " -f2- | grep -Fxq -- "$want"'
done
expect "hang stopped at the time limit" 'grep -Fxq "[hang] run-tests: stopped after 2 seconds (--timeout)" "$L"'
expect "nothing hang and spawn started is left" 'all_gone "$RUNNER_STATE"/hang.*.pids "$RUNNER_STATE"/spawn.*.pids'
expect "hang had a timeout group and a child that ignores TERM" '[ "$(cat "$RUNNER_STATE"/hang.*.pids | wc -l)" -eq 3 ]'
expect "spawn fails for what it left running" \
    'grep -Eq "^\[spawn\] run-tests: stopped [34] process\(es\) the test left running$" "$L"'
expect "one annotation per failing test, by the runner" \
    '[ "$(grep -c "^::error title=" "$L")" -eq 5 ] &&
     grep -Eq "^::error title=.*spawn\.sh::spawned%0A%0Arun-tests: stopped [34] process\(es\) the test left running%0Aexit status 1$" "$L" &&
     grep -q "^::error title=.*fail\.sh::  FAIL thing$" "$L" &&
     grep -q "^::error title=.*crash\.sh::before the crash%0Aexit status 137$" "$L" &&
     grep -q "^::error title=.*hang\.sh::.*stopped after 2 seconds" "$L"'
expect "no test line can be read as a workflow command" \
    '! tr "\r" "\n" < "$L" | grep -v "^::error title=" | grep -q "^[[:space:]]*::"'
expect "hostile lines are shown, each behind the test name" \
    'grep -Fxq "[hostile]   ::error::x" "$L" && grep -Fxq "[hostile] ::stop-commands::x" "$L" &&
     grep -Fxq "[hostile] ::add-mask::x" "$L" && grep -Fxq "[hostile] ::warning::no newline" "$L" &&
     grep -Fxq "[hostile] ok$cr[hostile] ::error::x" "$L"'
expect "every test line is behind its test name" \
    'grep -Fxq "[pass] pass" "$L" && grep -Fxq "[fail]   FAIL thing" "$L" && grep -Fxq "[spawn] spawned" "$L"'
expect "test output keeps its own bytes" 'cat "$OUT"/mixed/runs/run.*/*-hostile/output.log | grep -aq "^ok$cr::error::x$"'
expect "the job summary is not touched" '[ ! -e "$tmp/summary.md" ]'
rm -rf "$RUNNER_STATE"
GITHUB_ACTIONS=false run local -j 3 --timeout 2 "$fx/pass.sh" "$fx/fail.sh" "$fx/hostile.sh"
expect "outside CI: exit 1 and no annotations" '[ "$status" -eq 1 ] && ! grep -q "^::" "$OUT/local.log"'

echo "runner: 2. at most N at once, one after the other at -j 1"
rm -rf "$RUNNER_STATE"
BARRIER=3 run workers -j 3 "$fx/barrier.sh" "$fx/barrier.sh" "$fx/barrier.sh" "$fx/barrier.sh" "$fx/barrier.sh" "$fx/barrier.sh"
expect "-j 3: six runs passed" '[ "$status" -eq 0 ] && grep -Fxq "=== 6 passed, 0 failed, 0 skipped" "$OUT/workers.log"'
expect "-j 3: three ran at once, never more" '[ "$(cat "$RUNNER_STATE"/max.* | sort -n | tail -n 1)" = 3 ]'
expected_seq=$'start a\nend a\nstart b\nend b\nstart c\nend c\nstart a\nend a'
for j in "-j 1" "-j1" ""; do
    rm -rf "$RUNNER_STATE"
    # shellcheck disable=SC2086
    run serial $j "$fx/a.sh" "$fx/b.sh" "$fx/c.sh" "$fx/a.sh"
    expect "${j:-no -j}: one at a time, in order, a twice" \
        '[ "$status" -eq 0 ] && [ "$(cat "$RUNNER_STATE/seq")" = "$expected_seq" ] &&
         [ "$(grep "^=== $fx" "$OUT/serial.log" | tr "\n" " ")" = "=== $fx/a.sh === $fx/b.sh === $fx/c.sh === $fx/a.sh " ]'
    rm -rf "$OUT/serial"
done

# polite: hangs like hang, but exits 0 on TERM.
printf 'trap "exit 0" TERM\n. "$(dirname "$0")/hang.sh"\n' > "$fx/polite.sh"

echo "runner: 3. TERM and INT stop the running tests"
for sig in TERM INT; do
    rm -rf "$RUNNER_STATE"
    mkdir -p "$RUNNER_STATE" "$OUT/cancel-$sig"
    # A background job (and anything started with INT ignored, like this test under the runner)
    # ignores INT; a runner started from a terminal doesn't.
    RORTHOX_TEST_ROOT="$OUT/cancel-$sig" env --default-signal=INT "$runner" -j 3 "$fx/hang.sh" "$fx/hang.sh" \
        "$fx/polite.sh" "$fx/pass.sh" > "$OUT/cancel-$sig.log" 2>&1 &
    pid=$!
    for ((i = 0; i < 200; i++)); do
        set -- "$RUNNER_STATE"/hang.*.pids
        [ $# -eq 3 ] && [ -e "$1" ] && break
        sleep 0.1
    done
    kill -"$sig" "$pid"
    for ((i = 0; i < 300; i++)); do
        kill -0 "$pid" 2> /dev/null || break
        sleep 0.1
    done
    if kill -0 "$pid" 2> /dev/null; then
        fail "$sig: the runner ended"
        kill -KILL "$pid" 2> /dev/null
    fi
    if wait "$pid"; then status=0; else status=$?; fi
    expect "$sig: exit status 1" '[ "$status" -eq 1 ]'
    expect "$sig: said so, the fourth test not started, the one that exits 0 on TERM failed" \
        'grep -Fxq "=== interrupted by SIG$sig: 1 test(s) not started" "$OUT/cancel-$sig.log" &&
         grep -Fxq "=== 0 passed, 3 failed, 0 skipped" "$OUT/cancel-$sig.log" &&
         grep -Fxq "[polite] run-tests: stopped, the run was interrupted" "$OUT/cancel-$sig.log" &&
         [ ! -e "$RUNNER_STATE"/ran.* ]'
    expect "$sig: nothing either test started is left" 'all_gone "$RUNNER_STATE"/hang.*.pids'
done

echo "runner: 4. bad command lines and --list"
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE"
for args in "-j 0 $fx/pass.sh" "-j x $fx/pass.sh" "$fx/pass.sh -j" "--timeout 0 $fx/pass.sh" "--bogus $fx/pass.sh" \
    "$fx/pass.sh $fx/missing.sh"; do
    # shellcheck disable=SC2086
    run bad $args
    expect "exit 2, nothing run: $args" '[ "$status" -eq 2 ] && ! grep -q "^===" "$OUT/bad.log"'
done
expect "no test ran from a bad command line" '[ ! -e "$RUNNER_STATE"/ran.* ]'
run list --list -j 4 "$fx/pass.sh" "$fx/skip.sh" "$fx/pass.sh"
expect "--list prints the selection" \
    '[ "$status" -eq 0 ] && [ "$(cat "$OUT/list.log")" = "$fx/pass.sh$nl$fx/skip.sh$nl$fx/pass.sh" ] &&
     [ ! -e "$RUNNER_STATE"/ran.* ]'
run list --list
expect "--list with no tests: smoke and scripts/tests" \
    '[ "$status" -eq 0 ] && [ "$(head -n 1 "$OUT/list.log")" = scripts/smoke.sh ] && grep -Fxq scripts/tests/runner.sh "$OUT/list.log"'

if [ "$failures" -ne 0 ]; then
    echo "runner: $failures check(s) failed" >&2
    exit 1
fi
echo "runner: all checks passed"
