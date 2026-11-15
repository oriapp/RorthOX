#!/usr/bin/env bash
#
# Runs the smoke test and every test in scripts/tests/ (each boots RorthOX in QEMU), up to N at a
# time, keeps going after a failure, and prints a summary.
#
# Usage:  ./scripts/run-tests.sh [-j N] [--timeout S] [--shard I/N] [--timings F] [--list] [test.sh ...]
#                                                                             (or: make test)
#   -j N         run up to N tests at once (default 1: one after the other, in the given order)
#   --timeout S  stop a test that runs longer than S seconds; it fails (default 900, or
#                $RORTHOX_TEST_TIMEOUT). QEMU_TIMEOUT still limits each QEMU run inside a test.
#   --shard I/N  split the selected tests into N shards of about the same run time and run only
#                shard I (1 to N), longest test first. Every selected test is in exactly one shard.
#   --timings F  run times for --shard (default scripts/test-timings.json); a test that isn't
#                there counts as the median. Checked on every run, also without --shard.
#   --list       print the tests that would run (with --shard: that shard's), one per line, in the
#                order they would start, and run nothing
#   --results D  put the run's directory at D (new or empty) instead of build/tests/runs/run.XXXXXX
#   test.sh ...  only these tests (a test named twice runs twice); default: scripts/smoke.sh and
#                every scripts/tests/*.sh
#
# Exit status: 0 when every test passed or was skipped (a test that exits with 77 is skipped),
# 1 when a test failed, crashed or timed out or the run was interrupted (or, at the top level, a file
# name in the run directory is one CI can't upload), 2 for a bad command line or bad timings file, or
# RORTHOX_ACCEL=kvm where KVM doesn't work (no test runs then). A shard with no tests runs nothing
# and exits 0.
#
# RORTHOX_ACCEL=tcg|kvm|auto (default tcg) picks the accelerator once, before the first test (see
# scripts/qemu-accel.sh); the reports and the last lines name it, the reason, the QEMU version and
# the CPU model.
#
# Every run gets its own directory build/tests/runs/run.XXXXXX (inside $RORTHOX_TEST_ROOT/runs when
# the runner itself runs inside a test). Each test there gets its own numbered directory, passed to
# it as RORTHOX_TEST_ROOT (its output goes to $RORTHOX_TEST_ROOT/<name>/), and its own empty TMPDIR,
# removed when it ends. What a test prints goes to output.log in its numbered directory; when the
# test ends the runner prints it with "[<name>] " in front of every line (also after a carriage
# return), so a test's output can never pass for a line of the runner's own, such as a CI error
# annotation. Run times and exit statuses go to durations.txt in the run directory, and at the end the
# runner writes its reports there (scripts/run-tests.py report): results.json, junit.xml, summary.md
# (also added to $GITHUB_STEP_SUMMARY when that is set and the runner doesn't run inside a test) and
# failed/, a bounded copy of the output and logs of the tests that failed. A new top-level run
# removes the earlier runs that have finished. A top-level run also fails when a file name in its
# run directory has a character CI's artifact upload refuses (see the end).
#
# Every test runs in its own session. When it ends or runs out of time, or when the runner gets
# TERM, INT, HUP or PIPE (its output was closed), whatever still runs in that session (QEMU, a
# debugger, timeout and their children) gets TERM and, 5 seconds later, KILL. On any other way out
# (another signal it can catch, an error) the runner stops its running tests the same way before it
# exits. KILL to the runner itself leaves its running tests behind, in their own sessions: stop
# them by session or by their TMPDIR (rx.XXXXXX).

set -uo pipefail
# Bash cannot trap a signal that was ignored when it started. Restore SIGPIPE before any
# output: a closed CI log pipe must interrupt the run and stop every active test.
if [ -r /proc/$$/status ]; then
    ignored=$(awk '$1 == "SigIgn:" { print $2 }' "/proc/$$/status")
    if [[ $ignored =~ ^[0-9A-Fa-f]+$ ]] && (( (16#$ignored & (1 << 12)) != 0 )); then
        exec env --default-signal=PIPE bash "$0" "$@"
    fi
fi
cd "$(dirname "$0")/.." || exit 2

usage() {
    echo "usage: $0 [-j N] [--timeout S] [--shard I/N] [--timings F] [--list] [--results D] [test.sh ...]" >&2
    exit 2
}
if ((BASH_VERSINFO[0] < 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] < 1))); then
    echo "run-tests.sh needs bash 5.1 or newer (wait -n -p)" >&2
    exit 2
fi

jobs=1
timeout_s=${RORTHOX_TEST_TIMEOUT:-900}
list=no
shard=
shard_set=no
timings=scripts/test-timings.json
results=
tests=()
while [ $# -gt 0 ]; do
    case "$1" in
        -j) [ $# -ge 2 ] || usage; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        --timeout) [ $# -ge 2 ] || usage; timeout_s=$2; shift 2 ;;
        --timeout=*) timeout_s=${1#--timeout=}; shift ;;
        --shard) [ $# -ge 2 ] || usage; shard=$2; shard_set=yes; shift 2 ;;
        --shard=*) shard=${1#--shard=}; shard_set=yes; shift ;;
        --timings) [ $# -ge 2 ] || usage; timings=$2; shift 2 ;;
        --timings=*) timings=${1#--timings=}; shift ;;
        --list) list=yes; shift ;;
        --results) [ $# -ge 2 ] || usage; results=$2; shift 2 ;;
        --results=*) results=${1#--results=}; shift ;;
        --) shift; tests+=("$@"); break ;;
        -*) echo "unknown option: $1" >&2; usage ;;
        *) tests+=("$1"); shift ;;
    esac
done
[[ $jobs =~ ^[1-9][0-9]{0,3}$ ]] || { echo "-j needs a number from 1 to 9999, not '$jobs'" >&2; exit 2; }
[[ $timeout_s =~ ^[1-9][0-9]{0,6}$ ]] || { echo "--timeout needs a number of seconds, not '$timeout_s'" >&2; exit 2; }
if [ ${#tests[@]} -eq 0 ]; then
    tests=(scripts/smoke.sh)
    for t in scripts/tests/*.sh; do
        [ -e "$t" ] && tests+=("$t")
    done
fi
for t in "${tests[@]}"; do
    [ -f "$t" ] || { echo "no such test: $t" >&2; exit 2; }
done
# The helper checks the timings and picks the shard's tests.
plan_args=(--timings "$timings")
[ "$shard_set" = no ] || plan_args+=(--shard "$shard")
plan="$(mktemp)" || exit 2
if ! python3 -B scripts/run-tests.py plan "${plan_args[@]}" -- "${tests[@]}" > "$plan"; then
    rm -f "$plan"
    exit 2
fi
mapfile -d '' tests < "$plan"
rm -f "$plan"
if [ "$list" = yes ]; then
    [ ${#tests[@]} -eq 0 ] || printf '%s\n' "${tests[@]}"
    exit 0
fi

# monotonic: sets now to the time since boot in hundredths of a second (/proc/uptime). Run times
# and time limits use it, not SECONDS or EPOCHREALTIME: those follow the wall clock, which NTP or a
# VM that resumes can step back or ahead in the middle of a test.
monotonic() {
    local up
    read -r up _ < /proc/uptime 2> /dev/null && [[ $up =~ ^[0-9]+\.[0-9][0-9]$ ]] && now=$((10#${up/./}))
}
monotonic || { echo "run-tests.sh needs /proc/uptime for its clock" >&2; exit 2; }

# The accelerator for every test, decided here once before any test starts (scripts/qemu-accel.sh):
# the tests get RORTHOX_ACCEL=tcg|kvm and RORTHOX_ACCEL_REASON. With KVM, a test fails when one of its
# QEMUs didn't report "kvm support: enabled" in its monitor.log (not below a directory named runs: a
# runner inside the test checks its own runs, and a test keeps QEMU stand-ins there).
. scripts/qemu-accel.sh
accel_select || exit 2
qemu_version=$("${QEMU:-qemu-system-i386}" --version 2> /dev/null | head -n 1)
qemu_version=${qemu_version:-unknown}
cpu_model=${QEMU_CPU:-qemu32}

if [ -n "$results" ]; then
    mkdir -p "$results" || exit 2
    [ -z "$(ls -A "$results")" ] || { echo "--results: $results is not empty" >&2; exit 2; }
    run_dir=$results
else
    if [ -n "${RORTHOX_TEST_ROOT:-}" ]; then runs="$RORTHOX_TEST_ROOT/runs"; else runs=build/tests/runs; fi
    mkdir -p "$runs" || exit 2
    if [ "$runs" = build/tests/runs ]; then
        for old in "$runs"/run.*; do
            if [ -f "$old/finished" ]; then rm -rf "$old"; fi
        done
    fi
    run_dir="$(mktemp -d "$runs/run.XXXXXX")" || exit 2
fi
[ ${#tests[@]} -eq 0 ] || printf '%s\0' "${tests[@]}" > "$run_dir/plan"
: >> "$run_dir/plan"
: >> "$run_dir/instances"
run_start=$now
# Unix socket paths are short (108 bytes), so the temporary directories stay on a short absolute
# path. The runner finds a test's processes by that path (pgrep -f takes a regular expression), so
# a TMPDIR with other characters than these isn't used.
if [[ ${TMPDIR:-} =~ ^/[A-Za-z0-9/._-]+$ ]]; then tmp_base=$TMPDIR; else tmp_base=/tmp; fi
run_tmp="$(mktemp -d "$tmp_base/rx.XXXXXX")" || exit 2
ticker=
declare -A active=()   # session (= the pid the runner started) -> index in tests
# A signal only marks the run as interrupted: the main loop then stops every running test, and the
# run directory is marked finished only after that. Nobody reads the output after PIPE.
cancel=
trap 'cancel=TERM' TERM
trap 'cancel=INT' INT
trap 'cancel=HUP' HUP
trap 'cancel=PIPE; exec > /dev/null' PIPE
trap on_exit EXIT
grace=5

annotate() {
    local title="$1" log="$2" status="$3" message='' line count=0
    local details="$run_tmp/details" escaped overflow="${4:-}"
    if [ "$overflow" = overflow ]; then
        cp "$log" "$details"
    else
        grep -a 'FAIL' "$log" | grep -av '^  ok ' > "$details" || true
    fi
    if [ ! -s "$details" ] && [ "$overflow" != overflow ]; then
        tail -n 20 "$log" > "$details"
        # The saved output may end mid-line.
        if [ -s "$details" ] && [ "$(tail -c1 "$details" | wc -l)" -eq 0 ]; then
            printf '\n' >> "$details"
        fi
        printf 'exit status %s\n' "$status" >> "$details"
    fi
    # Limit lines before escaping, and bytes before percent expansion (at most 3x).
    count="$(wc -l < "$details")"
    while IFS= read -r line || [ -n "$line" ]; do
        [ "$count" -gt 0 ] || break
        message+="${message:+$'\n'}$line"
        count=$((count - 1))
    done < <(if [ "$overflow" = overflow ]; then cat "$details"; else head -n 30 "$details"; fi)
    if [ "$overflow" != overflow ] && [ "$(wc -l < "$details")" -gt 30 ]; then
        message+=$'\n'"... $(($(wc -l < "$details") - 30)) more lines"
    fi
    if [ "$overflow" != overflow ] && [ "${#message}" -gt 1200 ]; then
        message="${message:0:1200}"$'\n... output truncated'
    fi
    title="${title//'%'/'%25'}"
    title="${title//$'\r'/'%0D'}"
    title="${title//$'\n'/'%0A'}"
    title="${title//':'/'%3A'}"
    title="${title//','/'%2C'}"
    escaped="${message//'%'/'%25'}"
    escaped="${escaped//$'\r'/'%0D'}"
    escaped="${escaped//$'\n'/'%0A'}"
    printf '::error title=%s::%s\n' "$title" "$escaped"
}

# A test's name as the runner prints it: one line, whatever the file is called.
one_line() {
    local s="${1//$'\r'/?}"
    printf '%s' "${s//$'\n'/?}"
}

# replay <name> <file>: print a test's output, "[<name>] " in front of every line and after every
# carriage return that doesn't end a line (a CI log also starts a line there). NUL bytes are dropped;
# output.log keeps them.
replay() {
    local LC_ALL=C prefix line cr
    prefix="[$(one_line "$1")] "
    cr=$'\r'"$prefix"
    while IFS= read -r line || [ -n "$line" ]; do
        [ "$cancel" != PIPE ] || break
        line=${line%$'\r'}
        printf '%s%s\n' "$prefix" "${line//$'\r'/"$cr"}"
    done < "$2"
}

# count_left <session> <test tmp>: sets left_count to the number of processes still running in the
# test's session, or outside it with the test's TMPDIR in their command line (QEMU's sockets and
# pipes are there). Fails when a pgrep was stopped by a signal. No subshells here or in stop_test:
# a subshell doesn't keep the runner's traps, so a signal to the runner's process group (Ctrl-C,
# or timeout around the runner) would end it and lose the answer.
count_left() {
    local -A seen=()
    local p s1 s2 pids=()
    pgrep -s "$1" > "$run_tmp/left" 2> /dev/null
    s1=$?
    pgrep -f -- "$2/" >> "$run_tmp/left" 2> /dev/null
    s2=$?
    mapfile -t pids < "$run_tmp/left"
    for p in "${pids[@]}"; do seen[$p]=1; done
    # The newly launched child may still be waiting to call setsid, with no TMPDIR in its argv.
    if ours "$1"; then seen[$1]=1; fi
    left_count=${#seen[@]}
    [ "$s1" -le 1 ] && [ "$s2" -le 1 ]
}

# signal_test <child/session> <test tmp> <signal>: before setsid the child belongs to our
# session, so signal only its verified PID. Afterwards also signal its private session.
signal_test() {
    local sid="$1" dir="$2" sig="$3"
    pkill -"$sig" -s "$sid" 2> /dev/null
    ours "$sid" && kill -"$sig" "$sid" 2> /dev/null
    pkill -"$sig" -f -- "$dir/" 2> /dev/null
    return 0
}

# stop_test <session> <test tmp>: TERM to everything the test left, KILL to what is still there
# $grace seconds later. Sets stopped to the number of processes that were left.
stop_test() {
    local sid="$1" dir="$2" i
    for ((i = 0; i < 3; i++)); do
        count_left "$sid" "$dir" && break
    done
    stopped=$left_count
    [ "$stopped" -gt 0 ] || return 0
    signal_test "$sid" "$dir" TERM
    for ((i = 0; i < grace * 10; i++)); do
        count_left "$sid" "$dir" && [ "$left_count" -eq 0 ] && return 0
        sleep 0.1
    done
    signal_test "$sid" "$dir" KILL
    for ((i = 0; i < 50; i++)); do
        count_left "$sid" "$dir" && [ "$left_count" -eq 0 ] && break
        sleep 0.1
    done
}

# on_exit: the EXIT trap. Stops the tests that still run (after a signal other than TERM, INT, HUP
# or PIPE, or an error), then removes the temporary directories and marks the run finished.
on_exit() {
    local sid
    [ -n "$ticker" ] && kill "$ticker" 2> /dev/null
    for sid in "${!active[@]}"; do
        stop_test "$sid" "$run_tmp/$((${active[$sid]} + 1))"
    done
    rm -rf "$run_tmp"
    : > "$run_dir/finished"
}

# ours <pid>: a living direct child, whether or not it has reached setsid. A zombie can
# be reaped even if Bash has already removed its job; a reused PID is not our child.
ours() {
    local stat rest
    { read -r stat < "/proc/$1/stat"; } 2> /dev/null || return 1
    read -r -a rest <<< "${stat##*) }"
    [ "${rest[1]}" = "$$" ] && [ "${rest[0]}" != Z ]
}

passed=()
failed=()
skipped=()
failed_order=()
declare -a statuses=() logs=() starts=() deadlines=() stopping=() sessions=() roots=()
next=0

start_test() {
    local i="$1" t="${tests[$1]}" root
    root="$run_dir/$(printf '%03d' $((i + 1)))-$(basename "$t" .sh)"
    mkdir -p "$root" "$run_tmp/$((i + 1))"
    roots[i]=$root
    printf '%s\0%s\0' "$((i + 1))" "${root##*/}" >> "$run_dir/instances"
    logs[i]="$root/output.log"
    monotonic
    starts[i]=$now
    deadlines[i]=$((now + timeout_s * 100))
    # A background job of a script is never a process group leader, so setsid runs bash in place:
    # $! is the session id. That shell runs the test and exits with its status (it only dies of a
    # signal itself on KILL); its own messages, such as "Killed" for the test, go nowhere.
    RORTHOX_TEST_ROOT="$root" TMPDIR="$run_tmp/$((i + 1))" \
        setsid bash -c 'trap : TERM HUP; bash "$1" 2>&1; exit $?' run-test "$t" \
        > "${logs[i]}" 2> /dev/null < /dev/null &
    sessions[i]=$!
    active[$!]=$i
}

# finish_test <index> <exit status>: stop what the test left, print its output and record it.
finish_test() {
    local i="$1" status="$2" t="${tests[$1]}" left tenths
    stop_test "${sessions[i]}" "$run_tmp/$((i + 1))"
    left=$stopped
    unset "active[${sessions[i]}]"
    rm -rf "${run_tmp:?}/$((i + 1))"
    case "${stopping[i]:-}" in
        timeout)
            status=124
            printf '\nrun-tests: stopped after %s seconds (--timeout)\n' "$timeout_s" >> "${logs[i]}"
            ;;
        cancel)
            # Interrupted is never a pass or a skip.
            if [ "$status" -eq 0 ] || [ "$status" -eq 77 ]; then status=130; fi
            printf '\nrun-tests: stopped, the run was interrupted\n' >> "${logs[i]}"
            ;;
        *)
            # A test stops what it starts; what it leaves running makes it fail.
            if [ "$left" -gt 0 ]; then
                if [ "$status" -eq 0 ] || [ "$status" -eq 77 ]; then status=1; fi
                printf '\nrun-tests: stopped %s process(es) the test left running\n' "$left" >> "${logs[i]}"
            fi
            if [ "$RORTHOX_ACCEL" = kvm ] && [ -d "${roots[i]}" ] && { [ "$status" -eq 0 ] || [ "$status" -eq 77 ]; }; then
                find "${roots[i]}" -name runs -type d -prune -o -name monitor.log -type f \
                    ! -exec grep -aq 'kvm support: enabled' {} \; -print > "$run_tmp/no-kvm" 2> /dev/null
                if [ -s "$run_tmp/no-kvm" ]; then
                    status=1
                    { printf '\nrun-tests: RORTHOX_ACCEL=kvm, but these QEMUs did not report kvm support: enabled:\n'
                      sed 's/^/  /' "$run_tmp/no-kvm"; } >> "${logs[i]}"
                fi
            fi
            ;;
    esac
    statuses[i]=$status
    monotonic
    tenths=$(((now - starts[i]) / 10))
    echo "$((tenths / 10)).$((tenths % 10)) $status $t" >> "$run_dir/durations.txt"
    if [ -d "${roots[i]}" ] && [ ! -L "${roots[i]}" ]; then
        rm -f -- "${roots[i]}/result"
        echo "$status $tenths ${stopping[i]:-done}" > "${roots[i]}/result"
    fi
    echo "=== $(one_line "$t")"
    replay "$(basename "$t" .sh)" "${logs[i]}"
    case "$status" in
        0) ;;
        77) ;;
        *)
            nfailed=$((nfailed + 1))
            failed_order+=("$t")
            # Up to 9 annotations; the last one is kept for the full overflow list.
            if [ "${GITHUB_ACTIONS:-}" = true ] && [ "$nfailed" -le 9 ]; then
                annotate "$t" "${logs[i]}" "$status"
            fi
            ;;
    esac
}

nfailed=0
while :; do
    while [ -z "$cancel" ] && [ "${#active[@]}" -lt "$jobs" ] && [ "$next" -lt "${#tests[@]}" ]; do
        start_test "$next"
        next=$((next + 1))
    done
    [ "${#active[@]}" -gt 0 ] || break
    # Out of time or interrupted: TERM to the test, KILL $grace seconds later.
    monotonic
    for sid in "${!active[@]}"; do
        i=${active[$sid]}
        if [ -z "${stopping[i]:-}" ] && { [ -n "$cancel" ] || [ "$now" -ge "${deadlines[i]}" ]; }; then
            if [ -n "$cancel" ]; then stopping[i]=cancel; else stopping[i]=timeout; fi
            signal_test "$sid" "$run_tmp/$((i + 1))" TERM
            deadlines[i]=$((now + grace * 100))
        elif [ -n "${stopping[i]:-}" ] && [ "$now" -ge "${deadlines[i]}" ]; then
            signal_test "$sid" "$run_tmp/$((i + 1))" KILL
        fi
    done
    # Wake up at least once a second for the time limits; a signal ends the wait at once.
    [ -n "$ticker" ] || { sleep 1 & ticker=$!; }
    done_pid=
    wait -n -p done_pid "$ticker" "${!active[@]}" 2> /dev/null
    status=$?
    if [ -z "${done_pid:-}" ]; then
        :
    elif [ "$done_pid" = "$ticker" ]; then
        ticker=
    elif [ -n "${active[$done_pid]:-}" ]; then
        finish_test "${active[$done_pid]}" "$status"
    fi
    # Bash drops a completed job from its job list once it has reported it (e.g. as killed);
    # wait <pid> still knows its status. Never block waiting for a living pre-setsid child.
    for sid in "${!active[@]}"; do
        ours "$sid" && continue
        wait "$sid" 2> /dev/null
        finish_test "${active[$sid]}" "$?"
    done
done

for ((i = 0; i < ${#tests[@]}; i++)); do
    case "${statuses[i]:-}" in
        '') ;;
        0) passed+=("${tests[i]}") ;;
        77) skipped+=("${tests[i]}") ;;
        *) failed+=("${tests[i]}") ;;
    esac
done

if [ "${GITHUB_ACTIONS:-}" = true ] && [ "$nfailed" -ge 10 ]; then
    printf '%s more failing tests:\n' "$((nfailed - 9))" > "$run_tmp/overflow"
    printf '%s\n' "${failed_order[@]:9}" >> "$run_tmp/overflow"
    annotate 'Remaining failed tests' "$run_tmp/overflow" 1 overflow
fi

[ -z "$cancel" ] || echo "=== interrupted by SIG$cancel: $((${#tests[@]} - next)) test(s) not started"
echo "=== ${#passed[@]} passed, ${#failed[@]} failed, ${#skipped[@]} skipped"
echo "=== accelerator $RORTHOX_ACCEL ($RORTHOX_ACCEL_REASON), $qemu_version, CPU $cpu_model"
echo "=== output in $run_dir"
reports=ok
monotonic
if python3 -B scripts/run-tests.py report --run-dir "$run_dir" --jobs "$jobs" --shard "$shard" \
    --accelerator "$RORTHOX_ACCEL" --accelerator-reason "$RORTHOX_ACCEL_REASON" --qemu "$qemu_version" \
    --cpu "$cpu_model" --elapsed "$(((now - run_start) / 100))"; then
    [ ! -d "$run_dir/failed" ] || echo "=== output of the failed tests in $run_dir/failed"
    # Only the top-level runner writes the CI job summary (summary.md stays below 1 MiB).
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ] && [ -z "${RORTHOX_TEST_ROOT:-}" ]; then
        cat "$run_dir/summary.md" >> "$GITHUB_STEP_SUMMARY" || reports=failed
    fi
else
    reports=failed
fi
[ "$reports" = ok ] || echo "=== the reports could not be written"
for t in "${failed[@]}"; do echo "FAILED: $(one_line "$t")"; done
for t in "${skipped[@]}"; do echo "SKIPPED: $(one_line "$t")"; done

# CI uploads build/tests, and its artifact upload refuses a file whose path has one of
# " : < > | * ? or a carriage return or newline in it. A top-level run fails when a name in its run
# directory has one (a runner inside a test may run tests with such names, in its own directory).
bad_names=()
if [ -z "${RORTHOX_TEST_ROOT:-}" ]; then
    crlf=$'\r\n'
    while IFS= read -r -d '' f; do
        bad_names+=("$f")
    done < <(find "$run_dir" -mindepth 1 \( -name '*[":<>|*?]*' -o -name "*[$crlf]*" \) -print0 | sort -z)
fi
if [ ${#bad_names[@]} -gt 0 ]; then
    echo "=== ${#bad_names[@]} file name(s) in $run_dir CI can't upload (\" : < > | * ? CR LF):"
    for f in "${bad_names[@]:0:20}"; do echo "BAD NAME: $(one_line "$f")"; done
    if [ "${GITHUB_ACTIONS:-}" = true ]; then
        for f in "${bad_names[@]:0:20}"; do one_line "$f"; echo; done > "$run_tmp/bad-names"
        annotate 'File names CI cannot upload' "$run_tmp/bad-names" 1 overflow
    fi
fi
[ ${#failed[@]} -eq 0 ] && [ -z "$cancel" ] && [ ${#bad_names[@]} -eq 0 ] && [ "$reports" = ok ]
