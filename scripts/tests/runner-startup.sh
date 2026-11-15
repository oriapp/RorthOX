#!/usr/bin/env bash
#
# The test runner (scripts/run-tests.sh) while a test is still starting. The runner starts each
# test as "setsid bash ..." in the background; until that process has run setsid it is still in
# the runner's own session, not yet the leader of one of its own. Here a setsid stand-in, first in
# the runner's PATH, holds the first test (hold.sh) at that point until this test lets it go (or,
# with STARTUP_TERM=die, until TERM ends it), then runs the real setsid. hold.sh says from inside
# the test that it runs, then blocks; it ends by itself (exit 0) only after 6 s. pass.sh comes
# after it. The runner runs in a session of its own, with one more process in it: the runner must
# not signal that session, only the held process.
# 1. TERM, hold.sh let go before it (after the runner's one-second wakeup): hold.sh interrupted,
#    pass.sh not started, the runner exits 1.
# 2. INT while hold.sh is held: the runner sends TERM to the held process; let go after that,
#    hold.sh interrupted, pass.sh not started, exit 1.
# 3. TERM while held, with a stand-in that dies of TERM: the same.
# 4. --timeout 1 while held: TERM to the held process at the limit; let go after that, hold.sh
#    timed out (status 124), pass.sh still runs, exit 1.
# 5. --timeout 3, hold.sh let go before the limit (after the runner's one-second wakeup): timed
#    out (124), exit 1.
# Every case: the runner ends, nothing of hold.sh or the stand-in is left, and the other process in
# the runner's session is still there.

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
runner="$PWD/scripts/run-tests.sh"
OUT=${RORTHOX_TEST_ROOT:-build/tests}/runner-startup
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
tmp=$(mktemp -d)
fx="$tmp/fixtures"
mkdir -p "$fx/bin"
failures=0
real_setsid=$(command -v setsid) || { echo "runner-startup: no setsid" >&2; exit 1; }

ok() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; failures=$((failures + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

# The setsid stand-in: holds instance 001 (hold.sh) before the real setsid until a line comes on
# $STARTUP_GATE/release. It writes its pid to held, and each TERM it gets while held to signals.
cat > "$fx/bin/setsid" <<'EOF'
#!/usr/bin/env bash
if [[ ${RORTHOX_TEST_ROOT:-} == */001-hold ]]; then
    exec 3<> "$STARTUP_GATE/release"
    [ "${STARTUP_TERM:-}" = die ] || trap 'echo TERM >> "$STARTUP_GATE/signals"' TERM
    echo $$ > "$STARTUP_GATE/held.part"
    mv "$STARTUP_GATE/held.part" "$STARTUP_GATE/held"
    until read -r -u 3 _; do :; done
    trap - TERM
    exec 3>&-
fi
exec "$STARTUP_REAL_SETSID" "$@"
EOF
chmod +x "$fx/bin/setsid"
cat > "$fx/hold.sh" <<'EOF'
mkfifo "$TMPDIR/block"
exec 3<> "$TMPDIR/block"
sid=$(ps -o sid= -p $$)
echo "${sid// /}" > "$STARTUP_GATE/ready.part"
mv "$STARTUP_GATE/ready.part" "$STARTUP_GATE/ready"
read -r -t 6 -u 3 _
echo "hold: ended by itself"
EOF
printf 'touch "$STARTUP_GATE/pass.ran"\necho pass\n' > "$fx/pass.sh"

# cleanup: KILL whatever a broken runner left of hold.sh, the stand-in and the sentinels.
cleanup() {
    local f
    pkill -KILL -f -- "$fx/hold.sh" 2> /dev/null
    pkill -KILL -f -- "$fx/bin/setsid" 2> /dev/null
    for f in "$tmp"/*/sentinel; do
        [ -s "$f" ] && kill -KILL "$(cat "$f")" 2> /dev/null
    done
}
trap 'cleanup; rm -rf "$tmp"' EXIT

# wait_file <file>: up to 15 s for it to show up (a bound for a failure, not a wait for the runner).
wait_file() {
    local i
    for ((i = 0; i < 150; i++)); do
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

# start <case> <runner arguments>...: the runner in a session of its own with a sentinel process
# (sleep) in it, the stand-in first in its PATH; its output goes to $OUT/<case>.log, its run to
# $gate/results. Sets gate and pid.
start() {
    local name="$1"
    shift
    gate="$tmp/$name"
    mkdir -p "$gate"
    mkfifo "$gate/release"
    # Kept open here, so letting go never blocks, also when nothing is held any more.
    exec 4<> "$gate/release"
    # A background job is no process group leader: setsid runs bash in place, $! is the runner.
    STARTUP_GATE="$gate" STARTUP_REAL_SETSID="$real_setsid" PATH="$fx/bin:$PATH" RORTHOX_TEST_ROOT="$gate/root" \
        "$real_setsid" bash -c 'sleep 600 & echo $! > "$STARTUP_GATE/sentinel"; exec env --default-signal=INT "$@"' \
        sentinel "$runner" --results "$gate/results" "$@" > "$OUT/$name.log" 2>&1 4>&- &
    pid=$!
}

let_go() { echo go >&4; }

# finish <case>: up to 30 s for the runner to end (else KILL); sets status. Then the checks every
# case shares.
finish() {
    local name="$1" i held
    for ((i = 0; i < 300; i++)); do
        kill -0 "$pid" 2> /dev/null || break
        sleep 0.1
    done
    if kill -0 "$pid" 2> /dev/null; then
        fail "$name: the runner ended (killed after 30 s)"
        let_go
        kill -KILL "$pid" 2> /dev/null
    fi
    if wait "$pid"; then status=0; else status=$?; fi
    held=$(cat "$gate/held" 2> /dev/null)
    expect "$name: nothing of hold.sh or the stand-in is left" \
        '[ -n "$held" ] && ! kill -0 "$held" 2> /dev/null && ! pgrep -s "$held" > /dev/null &&
         ! pgrep -f -- "$fx/hold.sh" > /dev/null && ! pgrep -f -- "$fx/bin/setsid" > /dev/null'
    expect "$name: the other process in the runner's session is still there" \
        '[ -s "$gate/sentinel" ] && kill -0 "$(cat "$gate/sentinel")" 2> /dev/null'
    exec 4>&-
    cleanup
}

# result <test> <result> [<exit status>]: the test's entry in the current case's results.json.
result() {
    python3 -B -c '
import json, os, sys
r = json.load(open(sys.argv[1] + "/results.json"))
t = [t for t in r["tests"] if os.path.basename(t["name"]) == sys.argv[2]]
assert len(t) == 1 and t[0]["result"] == sys.argv[3], t
assert len(sys.argv) == 4 or t[0]["exit_status"] == int(sys.argv[4]), t
' "$gate/results" "$@" 2> /dev/null
}

# interrupted <case> <signal>: the checks of a run stopped by a signal.
interrupted() {
    local name="$1" sig="$2" L="$OUT/$1.log"
    expect "$name: exit 1, said so, pass.sh not started" \
        '[ "$status" -eq 1 ] && grep -Fxq "=== interrupted by SIG$sig: 1 test(s) not started" "$L" &&
         grep -Fxq "=== 0 passed, 1 failed, 0 skipped" "$L" && [ ! -e "$gate/pass.ran" ]'
    expect "$name: hold.sh interrupted, pass.sh not run, in results.json" \
        'result hold.sh interrupted && result pass.sh "not run" &&
         grep -Fxq "[hold] run-tests: stopped, the run was interrupted" "$L"'
}

echo "runner-startup: 1. TERM, hold.sh let go before it"
start term-before "$fx/hold.sh" "$fx/pass.sh"
wait_file "$gate/held" || fail "term-before: hold.sh was held"
# Held over the runner's one-second wakeup.
sleep 1.5
let_go
wait_file "$gate/ready" || fail "term-before: hold.sh runs"
kill -TERM "$pid"
finish term-before
interrupted term-before TERM

echo "runner-startup: 2. INT while hold.sh is held, let go after the runner's TERM"
start int-after "$fx/hold.sh" "$fx/pass.sh"
wait_file "$gate/held" || fail "int-after: hold.sh was held"
kill -INT "$pid"
expect "int-after: the runner sends TERM to the held process" 'wait_file "$gate/signals"'
let_go
finish int-after
interrupted int-after INT

echo "runner-startup: 3. TERM while held, the stand-in dies of it"
STARTUP_TERM=die start term-held "$fx/hold.sh" "$fx/pass.sh"
wait_file "$gate/held" || fail "term-held: hold.sh was held"
kill -TERM "$pid"
finish term-held
interrupted term-held TERM
expect "term-held: hold.sh never ran" '[ ! -e "$gate/ready" ]'

echo "runner-startup: 4. --timeout 1 while hold.sh is held"
start timeout-held --timeout 1 "$fx/hold.sh" "$fx/pass.sh"
wait_file "$gate/held" || fail "timeout-held: hold.sh was held"
expect "timeout-held: the runner sends TERM to the held process at the limit" 'wait_file "$gate/signals"'
let_go
finish timeout-held
expect "timeout-held: exit 1, hold.sh timed out (124), pass.sh passed" \
    '[ "$status" -eq 1 ] && result hold.sh "timed out" 124 && result pass.sh passed 0 &&
     grep -Fxq "[hold] run-tests: stopped after 1 seconds (--timeout)" "$OUT/timeout-held.log"'

echo "runner-startup: 5. --timeout 3, hold.sh let go before the limit"
start timeout-before --timeout 3 "$fx/hold.sh"
wait_file "$gate/held" || fail "timeout-before: hold.sh was held"
# Held over the runner's one-second wakeup.
sleep 1.5
let_go
finish timeout-before
expect "timeout-before: exit 1, hold.sh timed out (124)" \
    '[ "$status" -eq 1 ] && result hold.sh "timed out" 124 &&
     grep -Fxq "[hold] run-tests: stopped after 3 seconds (--timeout)" "$OUT/timeout-before.log"'

if [ "$failures" -ne 0 ]; then
    echo "runner-startup: $failures check(s) failed" >&2
    exit 1
fi
echo "runner-startup: all checks passed"
