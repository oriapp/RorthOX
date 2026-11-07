#!/usr/bin/env bash
#
# Tests must be able to run side by side without touching each other or the build. The contract
# (scripts/run-tests.sh): every run of a test gets its own RORTHOX_TEST_ROOT and TMPDIR, every boot
# its own copy of the disk, and tests only read bin/ and build/.
#
# 1. Two runners at once, each with the same test: smoke (QEMU locks the disk it writes, so both
#    must boot copies; runner a also names smoke twice), screen-waits (it stops only its own
#    leftover processes) and task-list (a gdbstub socket each). All pass, each in its own directory.
# 2. TERM to kernel-exceptions right after its QEMU starts, and while its debugger is attached: no
#    process of that run is left (QEMU and the debugger have its TMPDIR in their command line), and
#    its TMPDIR is empty again (gdb socket directory, monitor pipe directory).
# 3. Every file in bin/, build/ (but build/tests and build/smoke) and programs/*/build/ is unchanged.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/test-isolation
rm -rf "$OUT"
mkdir -p "$OUT"
case "${TMPDIR:-}" in /*) tmp_base="$TMPDIR" ;; *) tmp_base=/tmp ;; esac
TMP=$(mktemp -d "$tmp_base/isolation.XXXXXX")
# What this test started in the background (runners, a test): stopped and waited for before $TMP goes.
children=()
stop_children() {
    local p
    for p in "${children[@]}"; do kill -TERM "$p" 2> /dev/null || :; done
    for p in "${children[@]}"; do wait "$p" 2> /dev/null || :; done
}
trap 'stop_children; rm -rf "$TMP"' EXIT

production() {
    find bin build programs \( -path build/tests -o -path build/smoke \) -prune -o -type f -print |
        grep -E '^(bin|build|programs/[^/]+/build)/' | LC_ALL=C sort | xargs -d '\n' sha256sum
}
production > "$OUT/production-before.sha256"

# pair <test> [<test again for runner a>]: two runners at once; $OUT/<name>-a and -b get their output.
# They are not this CI job's runner: their failures show here as FAIL lines, not as annotations.
pair() {
    local name a b sa=0 sb=0
    name=$(basename "$1" .sh)
    echo "test-isolation: two runners with $name at once"
    mkdir -p "$OUT/$name-a" "$OUT/$name-b"
    GITHUB_ACTIONS=false RORTHOX_TEST_ROOT="$OUT/$name-a" TMPDIR="$TMP" ./scripts/run-tests.sh "$@" \
        > "$OUT/$name-a.log" 2>&1 &
    a=$!
    GITHUB_ACTIONS=false RORTHOX_TEST_ROOT="$OUT/$name-b" TMPDIR="$TMP" ./scripts/run-tests.sh "$1" \
        > "$OUT/$name-b.log" 2>&1 &
    b=$!
    children=("$a" "$b")
    wait "$a" || sa=$?
    wait "$b" || sb=$?
    children=()
    echo "a=$sa b=$sb" > "$OUT/$name-status.txt"
    grep -h '^=== [0-9]* passed\|FAIL' "$OUT/$name-a.log" "$OUT/$name-b.log" | sed 's/^/  /' || true
}

pair scripts/smoke.sh scripts/smoke.sh
pair scripts/tests/screen-waits.sh
pair scripts/tests/task-list.sh

# The kernel's own line in each smoke boot, and QEMU's complaint if two of them shared a disk.
for d in "$OUT"/smoke-a/runs/run.*/001-smoke "$OUT"/smoke-a/runs/run.*/002-smoke "$OUT"/smoke-b/runs/run.*/001-smoke; do
    n=$(grep -a -c 'Thats cool' "$d/smoke/debugcon.log" 2> /dev/null || true)
    echo "boot=${n:-0}"
done > "$OUT/smoke-boots.txt"
cat "$OUT"/smoke-*/runs/run.*/*/smoke/monitor.log 2> /dev/null | grep -a -i 'lock' > "$OUT/smoke-locks.txt" || true
ls -A "$TMP" > "$OUT/runner-tmp.txt"

# term <case> <what to wait for>: start kernel-exceptions (case "early": QEMU stopped at power-on,
# then the debugger), wait for <what>, TERM the test, and look at what is left of it.
term() {
    local name="$1" root="$OUT/term-$1" tmp="$TMP/term-$1" pid status=0 i left reached=no
    echo "test-isolation: TERM kernel-exceptions ($name)"
    mkdir -p "$root" "$tmp"
    RORTHOX_TEST_ROOT="$root" TMPDIR="$tmp" KERNEL_EXCEPTION_CASES=early \
        bash scripts/tests/kernel-exceptions.sh > "$root.log" 2>&1 &
    pid=$!
    children=("$pid")
    for ((i = 0; i < 600; i++)); do
        kill -0 "$pid" 2> /dev/null || break
        case "$2" in
            qemu) [ -e "$root/kernel-exceptions/early/monitor.log" ] && reached=yes ;;
            debugger) pgrep -f -- "exceptions.py early $tmp/" > /dev/null && reached=yes ;;
        esac
        [ "$reached" = yes ] && break
        sleep 0.1
    done
    kill -TERM "$pid" 2> /dev/null || :
    wait "$pid" || status=$?
    children=()
    for ((i = 0; i < 100; i++)); do
        left=$(pgrep -f -- "$tmp/" | wc -l || true)
        [ "$left" -eq 0 ] && break
        sleep 0.1
    done
    {
        echo "reached=$reached"
        echo "exit=$status"
        echo "left=$left"
        echo "tmp entries=$(ls -A "$tmp" | wc -l)"
    } > "$OUT/term-$name.txt"
    pgrep -a -f -- "$tmp/" > "$OUT/term-$name-left.txt" || true
    sed 's/^/  /' "$OUT/term-$name.txt" "$OUT/term-$name-left.txt"
    pkill -f -- "$tmp/" || :
}

term qemu qemu
term debugger debugger

production > "$OUT/production-after.sha256"
if cmp -s "$OUT/production-before.sha256" "$OUT/production-after.sha256"; then
    echo "unchanged" > "$OUT/production.txt"
else
    diff "$OUT/production-before.sha256" "$OUT/production-after.sha256" | sed 's/^/  /' || true
    echo "changed" > "$OUT/production.txt"
fi

check "smoke: both runners passed"                   "$OUT/smoke-status.txt"        "a=0 b=0" line
check "smoke: runner a ran smoke twice"              "$OUT/smoke-a.log"             "=== 2 passed, 0 failed, 0 skipped" line
check "smoke: runner b passed"                       "$OUT/smoke-b.log"             "=== 1 passed, 0 failed, 0 skipped" line
check "smoke: three boots in three directories"      "$OUT/smoke-boots.txt"         "boot=0" absent
check "smoke: no disk lock clash"                    "$OUT/smoke-locks.txt"         "lock" absent
check "screen-waits: both runners passed"            "$OUT/screen-waits-status.txt" "a=0 b=0" line
check "task-list: both runners passed"               "$OUT/task-list-status.txt"    "a=0 b=0" line
check "runners removed their temporary directories"  "$OUT/runner-tmp.txt"          "rx." absent
for name in qemu debugger; do
    r="$OUT/term-$name.txt"
    check "TERM ($name): reached that point"            "$r" "reached=yes" line
    check "TERM ($name): the test did not pass"         "$r" "exit=0" absent
    check "TERM ($name): no QEMU or debugger left"      "$r" "left=0" line
    check "TERM ($name): its TMPDIR is empty"           "$r" "tmp entries=0" line
done
check "bin/, build/ and programs/*/build/ unchanged" "$OUT/production.txt"          "unchanged" line

if [ "$failures" -ne 0 ]; then
    echo "test-isolation: $failures check(s) failed" >&2
    exit 1
fi
echo "test-isolation: all checks passed"
