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
# 5. Other ways out: INT to the runner's whole process group while it waits for what a test left
#    still stops that and fails the test; a TMPDIR with regular expression characters doesn't hide
#    a test's processes; a runner whose output is closed (| head -n 1), or that gets USR1, stops its
#    running tests before it exits.
# 6. A top-level run (no RORTHOX_TEST_ROOT) fails when a file name in its run directory has a
#    character CI's artifact upload refuses (" : < > | * ? CR LF) and names it; a clean run passes.
# 7. --shard I/N: the shards together hold every selected test exactly once (a test named twice
#    twice), the longest first by the timings file, ties by path and then by shard number, tests
#    it doesn't know as the median, the same whatever the order given; a shard runs just its
#    tests, a shard with none runs nothing and passes; a bad shard or timings file exits 2 and
#    runs nothing. The real timings file splits the whole suite into 4 shards.
#
# The runs of the runner under test go to a temporary directory (CI uploads build/tests); what
# each one printed stays in $OUT/<case>.log.

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

# run <case> <runner arguments>...: run the runner; its output goes to $OUT/<case>.log, its runs to $roots/<case>/.
roots="$tmp/roots"
run() {
    local name="$1"
    shift
    mkdir -p "$roots/$name" "$RUNNER_STATE"
    if RORTHOX_TEST_ROOT="$roots/$name" timeout -k 5 120 "$runner" "$@" > "$OUT/$name.log" 2>&1; then status=0; else status=$?; fi
    durations=$(cat "$roots/$name"/runs/run.*/durations.txt 2> /dev/null || true)
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
expect "test output keeps its own bytes" 'cat "$roots"/mixed/runs/run.*/*-hostile/output.log | grep -aq "^ok$cr::error::x$"'
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
    rm -rf "$roots/serial"
done

# polite: hangs like hang, but exits 0 on TERM.
printf 'trap "exit 0" TERM\n. "$(dirname "$0")/hang.sh"\n' > "$fx/polite.sh"

echo "runner: 3. TERM and INT stop the running tests"
for sig in TERM INT; do
    rm -rf "$RUNNER_STATE"
    mkdir -p "$RUNNER_STATE" "$roots/cancel-$sig"
    # A background job (and anything started with INT ignored, like this test under the runner)
    # ignores INT; a runner started from a terminal doesn't.
    RORTHOX_TEST_ROOT="$roots/cancel-$sig" env --default-signal=INT "$runner" -j 3 "$fx/hang.sh" "$fx/hang.sh" \
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
echo "runner: 5. other ways out"
# leave: passes, leaving a child that ignores TERM. chatty: waits for hang to start, then prints
# more than a pipe holds.
printf 'bash -c '\''trap "" TERM; while :; do sleep 0.2; done'\'' &\necho $! > "$RUNNER_STATE/leave.pids"\n' > "$fx/leave.sh"
cat > "$fx/chatty.sh" <<'EOF'
for ((i = 0; i < 100; i++)); do
    set -- "$RUNNER_STATE"/hang.*.pids
    [ -e "$1" ] && break
    sleep 0.1
done
seq 1 200000
EOF
# reap <pid files>...: KILL what a broken runner left (pids, their groups and sessions).
reap() {
    local p
    cat "$@" 2> /dev/null | while read -r p; do
        pkill -KILL -s "$p" 2> /dev/null; pkill -KILL -g "$p" 2> /dev/null; kill -KILL "$p" 2> /dev/null
    done
}
# wait_file <glob>: up to 10 s for a file to show up.
wait_file() {
    for ((i = 0; i < 100; i++)); do
        set -- $1
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE" "$roots/group-int"
# setsid: the runner leads a process group of its own, as in a terminal.
RORTHOX_TEST_ROOT="$roots/group-int" env --default-signal=INT setsid "$runner" "$fx/leave.sh" > "$OUT/group-int.log" 2>&1 &
pid=$!
wait_file "$RUNNER_STATE/leave.pids"
# The runner now waits up to 5 s for the child that ignores TERM.
sleep 1
kill -INT -- "-$pid"
if wait "$pid"; then status=0; else status=$?; fi
expect "INT to the runner's process group while it stops what a test left: test failed, nothing left" \
    '[ "$status" -eq 1 ] && grep -Fxq "=== 0 passed, 1 failed, 0 skipped" "$OUT/group-int.log" &&
     grep -Eq "^\[leave\] run-tests: stopped [12] process\(es\) the test left running$" "$OUT/group-int.log" &&
     all_gone "$RUNNER_STATE/leave.pids"'
reap "$RUNNER_STATE/leave.pids"
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE" "$tmp/a+b(c"
TMPDIR="$tmp/a+b(c" run weird "$fx/spawn.sh"
expect "TMPDIR with + and (: what spawn left is found and stopped" \
    '[ "$status" -eq 1 ] && grep -Eq "^\[spawn\] run-tests: stopped [34] process\(es\) the test left running$" "$OUT/weird.log" &&
     all_gone "$RUNNER_STATE"/spawn.*.pids'
reap "$RUNNER_STATE"/spawn.*.pids
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE" "$roots/pipe"
{ RORTHOX_TEST_ROOT="$roots/pipe" timeout -k 5 60 "$runner" -j 2 "$fx/hang.sh" "$fx/chatty.sh" 2> "$OUT/pipe.err"
  echo $? > "$RUNNER_STATE/pipe.status"; } | head -n 1 > "$OUT/pipe.log"
expect "output closed (| head -n 1): exit 1, the running test stopped" \
    '[ "$(cat "$RUNNER_STATE/pipe.status")" = 1 ] && [ "$(cat "$OUT/pipe.log")" = "=== $fx/chatty.sh" ] &&
     all_gone "$RUNNER_STATE"/hang.*.pids'
reap "$RUNNER_STATE"/hang.*.pids
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE" "$roots/usr1"
RORTHOX_TEST_ROOT="$roots/usr1" "$runner" "$fx/hang.sh" > "$OUT/usr1.log" 2>&1 &
pid=$!
wait_file "$RUNNER_STATE/hang.*.pids"
kill -USR1 "$pid"
if wait "$pid"; then status=0; else status=$?; fi
expect "USR1: the runner stops the running test before it exits" \
    '[ "$status" -ne 0 ] && all_gone "$RUNNER_STATE"/hang.*.pids && [ -e "$(echo "$roots"/usr1/runs/run.*)/finished" ]'
reap "$RUNNER_STATE"/hang.*.pids

echo "runner: 6. file names CI can't upload"
# A copy of the runner in a tree of its own: run at the top level there, its runs go to
# $tmp/top/build/tests/runs, not to this checkout's.
mkdir -p "$tmp/top/scripts"
cp "$runner" "$tmp/top/scripts/run-tests.sh"
cp scripts/run-tests.py scripts/test-timings.json "$tmp/top/scripts/"
printf 'echo pass\n' > "$fx/bad:name.sh"
cat > "$fx/odd-output.sh" <<'EOF'
touch "$RORTHOX_TEST_ROOT/a?b" "$RORTHOX_TEST_ROOT/c"$'\r'"d"
echo pass
EOF
# top <case> <runner arguments>...: like run, with no RORTHOX_TEST_ROOT.
top() {
    local name="$1"
    shift
    if env -u RORTHOX_TEST_ROOT -u GITHUB_STEP_SUMMARY timeout -k 5 120 "$tmp/top/scripts/run-tests.sh" "$@" \
        > "$OUT/$name.log" 2>&1; then status=0; else status=$?; fi
}
GITHUB_ACTIONS=true top top-clean "$fx/pass.sh"
expect "clean names: exit 0, nothing said" \
    '[ "$status" -eq 0 ] && grep -Fxq "=== 1 passed, 0 failed, 0 skipped" "$OUT/top-clean.log" &&
     ! grep -q "BAD NAME\|^::" "$OUT/top-clean.log"'
GITHUB_ACTIONS=true top top-bad "$fx/pass.sh" "$fx/bad:name.sh" "$fx/odd-output.sh"
L="$OUT/top-bad.log"
expect "bad names: exit 1, both named, one annotation" \
    '[ "$status" -eq 1 ] && grep -Fxq "=== 3 passed, 0 failed, 0 skipped" "$L" &&
     grep -Eq "^=== 3 file name\(s\) in build/tests/runs/run\.[^ ]+ CI can.t upload" "$L" &&
     grep -Eq "^BAD NAME: build/tests/runs/run\.[^/]+/002-bad:name$" "$L" &&
     grep -Eq "^BAD NAME: build/tests/runs/run\.[^/]+/003-odd-output/a\?b$" "$L" &&
     grep -Eq "^BAD NAME: build/tests/runs/run\.[^/]+/003-odd-output/c\?d$" "$L" &&
     [ "$(grep -c "^::error " "$L")" -eq 1 ] && grep -q "^::error title=File names CI cannot upload::.*002-bad:name%0A" "$L"'
GITHUB_ACTIONS=false top top-local "$fx/bad:name.sh"
expect "bad names outside CI: exit 1, no annotation" \
    '[ "$status" -eq 1 ] && grep -q "^BAD NAME: .*001-bad:name$" "$OUT/top-local.log" && ! grep -q "^::" "$OUT/top-local.log"'

echo "runner: 7. shards"
rm -rf "$RUNNER_STATE"
mkdir -p "$RUNNER_STATE"
for n in A B C D M N; do
    printf 'echo "%s" >> "$RUNNER_STATE/seq"\n' "$n" > "$fx/$n.sh"
done
printf '{"tests": {"%s": 10, "%s": 6, "%s": 4, "%s": 4.0, "%s/gone.sh": 2}}\n' \
    "$fx/A.sh" "$fx/B.sh" "$fx/C.sh" "$fx/D.sh" "$fx" > "$fx/timings.json"
# shards <case> <shard count> <tests>...: every shard's --list into $OUT/<case>.<i>.
shards() {
    local name="$1" n="$2" i
    shift 2
    for ((i = 1; i <= n; i++)); do
        run list --list --timings "$fx/timings.json" --shard "$i/$n" "$@"
        [ "$status" -eq 0 ] || fail "$name: --shard $i/$n exit $status"
        cp "$OUT/list.log" "$OUT/$name.$i"
    done
}
# A=10, B=6, C=4, D=4, then M and N (unknown: the median, 4): A to 1, B to 2, C to 2 (C before D
# by path), D to 1 (10 = 10: the lower number), M to 2 (14 > 10; M before N by path), N to 1 (14 =
# 14). With a weight below 4 for M, N would go to 2 as well.
shards order 2 "$fx/A.sh" "$fx/B.sh" "$fx/C.sh" "$fx/D.sh" "$fx/N.sh" "$fx/M.sh"
expect "2 shards: longest first, ties by path and shard number, a new test as the median" \
    '[ "$(cat "$OUT/order.1")" = "$fx/A.sh$nl$fx/D.sh$nl$fx/N.sh" ] &&
     [ "$(cat "$OUT/order.2")" = "$fx/B.sh$nl$fx/C.sh$nl$fx/M.sh" ]'
shards again 2 "$fx/M.sh" "$fx/N.sh" "$fx/D.sh" "$fx/C.sh" "$fx/B.sh" "$fx/A.sh"
expect "the same shards whatever the order given" \
    'cmp -s "$OUT/order.1" "$OUT/again.1" && cmp -s "$OUT/order.2" "$OUT/again.2"'
sel=("$fx/A.sh" "$fx/N.sh" "$fx/pass.sh" "$fx/B.sh" "$fx/A.sh" "$fx/C.sh" "$fx/skip.sh" "$fx/D.sh" "$fx/A.sh")
shards dup 4 "${sel[@]}"
expect "4 shards hold the selection exactly, a test named three times three times" \
    '[ "$(cat "$OUT"/dup.[1-4] | sort)" = "$(printf "%s\n" "${sel[@]}" | sort)" ]'
expect "... none of them empty, A in three of them" \
    '[ -s "$OUT/dup.1" ] && [ -s "$OUT/dup.4" ] && [ "$(grep -lFx "$fx/A.sh" "$OUT"/dup.[1-4] | wc -l)" -eq 3 ]'
run shard2 --timings "$fx/timings.json" --shard 2/2 "$fx/A.sh" "$fx/B.sh" "$fx/C.sh" "$fx/D.sh" "$fx/N.sh" "$fx/M.sh"
expect "--shard 2/2 runs B, C and M, in that order" \
    '[ "$status" -eq 0 ] && [ "$(cat "$RUNNER_STATE/seq")" = "B${nl}C${nl}M" ] &&
     grep -Fxq "=== 3 passed, 0 failed, 0 skipped" "$OUT/shard2.log"'
rm -f "$RUNNER_STATE/seq"
run empty --timings "$fx/timings.json" --shard 3/3 "$fx/A.sh" "$fx/B.sh"
expect "a shard with no tests runs nothing and passes" \
    '[ "$status" -eq 0 ] && grep -Fxq "=== 0 passed, 0 failed, 0 skipped" "$OUT/empty.log" && [ ! -e "$RUNNER_STATE/seq" ]'
printf '{"tests": {"a": 1}}' > "$fx/t-ok.json"
printf '{"tests": {"a": 1' > "$fx/t-cut.json"
printf '{"tests": {"a": 0}}' > "$fx/t-zero.json"
printf '{"tests": {"a": -3}}' > "$fx/t-neg.json"
printf '{"tests": {"a": NaN}}' > "$fx/t-nan.json"
printf '{"tests": {"a": 1e999}}' > "$fx/t-inf.json"
printf '{"tests": {"a": "4"}}' > "$fx/t-text.json"
printf '{"tests": {"a": true}}' > "$fx/t-bool.json"
printf '{"tests": {"a": 1, "a": 2}}' > "$fx/t-twice.json"
printf '{"a": 1}' > "$fx/t-flat.json"
for args in "--shard 0/2" "--shard 3/2" "--shard 2" "--shard 1/0" "--shard x/2" "--shard /2" "--shard 1/2/3" \
    "--shard 1/2 --timings $fx/missing.json" "--timings $fx/t-cut.json" "--shard 1/2 --timings $fx/t-zero.json" \
    "--shard 1/2 --timings $fx/t-neg.json" "--shard 1/2 --timings $fx/t-nan.json" "--shard 1/2 --timings $fx/t-inf.json" \
    "--shard 1/2 --timings $fx/t-text.json" "--shard 1/2 --timings $fx/t-bool.json" \
    "--shard 1/2 --timings $fx/t-twice.json" "--shard 1/2 --timings $fx/t-flat.json" "--shard"; do
    # shellcheck disable=SC2086
    run bad $args "$fx/pass.sh"
    expect "exit 2, nothing run: $args" '[ "$status" -eq 2 ] && ! grep -q "^===" "$OUT/bad.log" && [ ! -e "$RUNNER_STATE"/ran.* ]'
done
run good --shard 1/1 --timings "$fx/t-ok.json" "$fx/pass.sh"
expect "... and a good one runs" '[ "$status" -eq 0 ] && grep -Fxq "=== 1 passed, 0 failed, 0 skipped" "$OUT/good.log"'
run list --list
cp "$OUT/list.log" "$OUT/all"
for i in 1 2 3 4; do
    run list --list --shard "$i/4"
    [ "$status" -eq 0 ] || fail "suite --shard $i/4 exit $status"
    cp "$OUT/list.log" "$OUT/suite.$i"
done
expect "the suite's 4 shards hold every test once" \
    '[ -s "$OUT/all" ] && [ "$(sort "$OUT"/suite.[1-4])" = "$(sort "$OUT/all")" ] &&
     [ -s "$OUT/suite.1" ] && [ -s "$OUT/suite.2" ] && [ -s "$OUT/suite.3" ] && [ -s "$OUT/suite.4" ]'

if [ "$failures" -ne 0 ]; then
    echo "runner: $failures check(s) failed" >&2
    exit 1
fi
echo "runner: all checks passed"
