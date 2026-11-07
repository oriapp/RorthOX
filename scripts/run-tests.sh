#!/usr/bin/env bash
#
# Runs the smoke test and every test in scripts/tests/ (each boots RorthOX in QEMU), up to N at a
# time, keeps going after a failure, and prints a summary.
#
# Usage:  ./scripts/run-tests.sh [-j N] [--timeout S] [--list] [test.sh ...]     (or: make test)
#   -j N         run up to N tests at once (default 1: one after the other, in the given order)
#   --timeout S  stop a test that runs longer than S seconds; it fails (default 900, or
#                $RORTHOX_TEST_TIMEOUT). QEMU_TIMEOUT still limits each QEMU run inside a test.
#   --list       print the tests that would run, one per line, and run nothing
#   test.sh ...  only these tests (a test named twice runs twice); default: scripts/smoke.sh and
#                every scripts/tests/*.sh
#
# Exit status: 0 when every test passed or was skipped (a test that exits with 77 is skipped),
# 1 when a test failed, crashed or timed out or the run was interrupted (or, at the top level, a file
# name in the run directory is one CI can't upload), 2 for a bad command line (no test runs then).
#
# Every run gets its own directory build/tests/runs/run.XXXXXX (inside $RORTHOX_TEST_ROOT/runs when
# the runner itself runs inside a test). Each test there gets its own numbered directory, passed to
# it as RORTHOX_TEST_ROOT (its output goes to $RORTHOX_TEST_ROOT/<name>/), and its own empty TMPDIR,
# removed when it ends. What a test prints goes to output.log in its numbered directory; when the
# test ends the runner prints it with "[<name>] " in front of every line (also after a carriage
# return), so a test's output can never pass for a line of the runner's own, such as a CI error
# annotation. Run times and exit statuses go to durations.txt in the run directory. A new top-level
# run removes the earlier runs that have finished. A top-level run also fails when a file name in its
# run directory has a character CI's artifact upload refuses (see the end).
#
# Every test runs in its own session. When it ends or runs out of time, or when the runner gets
# TERM, INT or HUP, whatever still runs in that session (QEMU, a debugger, timeout and their
# children) gets TERM and, 5 seconds later, KILL.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

usage() {
    echo "usage: $0 [-j N] [--timeout S] [--list] [test.sh ...]" >&2
    exit 2
}
if ((BASH_VERSINFO[0] < 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] < 1))); then
    echo "run-tests.sh needs bash 5.1 or newer (wait -n -p)" >&2
    exit 2
fi

jobs=1
timeout_s=${RORTHOX_TEST_TIMEOUT:-900}
list=no
tests=()
while [ $# -gt 0 ]; do
    case "$1" in
        -j) [ $# -ge 2 ] || usage; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        --timeout) [ $# -ge 2 ] || usage; timeout_s=$2; shift 2 ;;
        --timeout=*) timeout_s=${1#--timeout=}; shift ;;
        --list) list=yes; shift ;;
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
if [ "$list" = yes ]; then
    printf '%s\n' "${tests[@]}"
    exit 0
fi

if [ -n "${RORTHOX_TEST_ROOT:-}" ]; then runs="$RORTHOX_TEST_ROOT/runs"; else runs=build/tests/runs; fi
mkdir -p "$runs" || exit 2
if [ "$runs" = build/tests/runs ]; then
    for old in "$runs"/run.*; do
        if [ -f "$old/finished" ]; then rm -rf "$old"; fi
    done
fi
run_dir="$(mktemp -d "$runs/run.XXXXXX")" || exit 2
# Unix socket paths are short (108 bytes), so the temporary directories stay on a short absolute path.
case "${TMPDIR:-}" in /*) tmp_base="$TMPDIR" ;; *) tmp_base=/tmp ;; esac
run_tmp="$(mktemp -d "$tmp_base/rx.XXXXXX")" || exit 2
ticker=
# A signal only marks the run as interrupted: the main loop then stops every running test, and the
# run directory is marked finished only after that.
cancel=
trap 'cancel=TERM' TERM
trap 'cancel=INT' INT
trap 'cancel=HUP' HUP
trap '[ -n "$ticker" ] && kill "$ticker" 2> /dev/null; rm -rf "$run_tmp"; : > "$run_dir/finished"' EXIT
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
        line=${line%$'\r'}
        printf '%s%s\n' "$prefix" "${line//$'\r'/"$cr"}"
    done < "$2"
}

# leftovers <session> <test tmp>: what still runs in the test's session, or outside it with the
# test's TMPDIR in its command line (QEMU's sockets and pipes are there).
leftovers() {
    { pgrep -s "$1"; pgrep -f -- "$2/"; } 2> /dev/null | sort -u
}

# stop_test <session> <test tmp>: TERM to everything the test left, KILL to what is still there
# $grace seconds later. Prints how many processes were left.
stop_test() {
    local sid="$1" dir="$2" left i
    left=$(leftovers "$sid" "$dir" | wc -l)
    [ "$left" -gt 0 ] || { echo 0; return 0; }
    pkill -TERM -s "$sid" 2> /dev/null
    pkill -TERM -f -- "$dir/" 2> /dev/null
    for ((i = 0; i < grace * 10; i++)); do
        [ -n "$(leftovers "$sid" "$dir")" ] || { echo "$left"; return 0; }
        sleep 0.1
    done
    pkill -KILL -s "$sid" 2> /dev/null
    pkill -KILL -f -- "$dir/" 2> /dev/null
    for ((i = 0; i < 50; i++)); do
        [ -n "$(leftovers "$sid" "$dir")" ] || break
        sleep 0.1
    done
    echo "$left"
}

# ours <pid>: the pid is still a test this runner started (its child and a session leader), not a
# finished one whose number came back.
ours() {
    local stat rest
    { read -r stat < "/proc/$1/stat"; } 2> /dev/null || return 1
    read -r -a rest <<< "${stat##*) }"
    [ "${rest[1]}" = "$$" ] && [ "${rest[3]}" = "$1" ]
}

passed=()
failed=()
skipped=()
failed_order=()
declare -a statuses=() logs=() starts=() deadlines=() stopping=() sessions=()
declare -A active=()   # session (= the pid the runner started) -> index in tests
next=0

start_test() {
    local i="$1" t="${tests[$1]}" root
    root="$run_dir/$(printf '%03d' $((i + 1)))-$(basename "$t" .sh)"
    mkdir -p "$root" "$run_tmp/$((i + 1))"
    logs[i]="$root/output.log"
    starts[i]=${EPOCHREALTIME/[.,]/}   # microseconds
    deadlines[i]=$((SECONDS + timeout_s))
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
    left=$(stop_test "${sessions[i]}" "$run_tmp/$((i + 1))")
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
            ;;
    esac
    statuses[i]=$status
    tenths=$(((${EPOCHREALTIME/[.,]/} - starts[i]) / 100000))
    echo "$((tenths / 10)).$((tenths % 10)) $status $t" >> "$run_dir/durations.txt"
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
    # Out of time or interrupted: TERM to the test's whole session, KILL $grace seconds later.
    for sid in "${!active[@]}"; do
        i=${active[$sid]}
        if [ -z "${stopping[i]:-}" ] && { [ -n "$cancel" ] || [ "$SECONDS" -ge "${deadlines[i]}" ]; }; then
            if [ -n "$cancel" ]; then stopping[i]=cancel; else stopping[i]=timeout; fi
            pkill -TERM -s "$sid" 2> /dev/null
            deadlines[i]=$((SECONDS + grace))
        elif [ -n "${stopping[i]:-}" ] && [ "$SECONDS" -ge "${deadlines[i]}" ]; then
            pkill -KILL -s "$sid" 2> /dev/null
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
    # bash drops a job from its job list once it has reported it (e.g. as killed); wait <pid> still
    # knows its status.
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
echo "=== output in $run_dir"
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
[ ${#failed[@]} -eq 0 ] && [ -z "$cancel" ] && [ ${#bad_names[@]} -eq 0 ]
