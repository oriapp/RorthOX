#!/usr/bin/env bash
#
# The runner's reports (scripts/run-tests.sh, scripts/run-tests.py), with small fixture tests instead
# of QEMU:
# 1. A run with a pass, a skip, a failure that leaves logs, a screen dump, a disk image, a symlink and
#    a 2.6 MB log, a test whose name and output hold Markdown, XML, control and invalid UTF-8
#    characters, and one that runs past --timeout: results.json, junit.xml (parsed back) and
#    summary.md agree with the run, the summary goes to $GITHUB_STEP_SUMMARY only from a top-level
#    runner, names are escaped, and failed/ holds the failed tests' output and logs only, in
#    bounded sizes, without disk images or symlinks, with a note of what was cut or left out.
# 2. The summary stays within its size limit and lists the failures first.
# 3. --shard: a failing test fails only its own shard, and shows up in that shard's reports and
#    annotations; the two shards' results.json hold the selection exactly.
# 4. TERM: the test that was stopped and the one never started are in the reports.
# 5. A run whose reports can't be written fails.

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
runner="$PWD/scripts/run-tests.sh"
OUT=${RORTHOX_TEST_ROOT:-build/tests}/reports
rm -rf "$OUT"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The runs' directories hold test names that are no good as file names elsewhere (an artifact
# upload refuses | < > and :): they stay in the temporary directory, the runners' logs in $OUT.
RES="$tmp/results"
mkdir -p "$RES"
fx="$tmp/fixtures"
mkdir -p "$fx"
failures=0
nl=$'\n'

ok() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; failures=$((failures + 1)); }
expect() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

# top <case> <runner arguments>...: a top-level runner (not inside a test) writing to $RES/<case>/,
# its output in $OUT/<case>.log, the job summary in $tmp/<case>.md.
top() {
    local name="$1"
    shift
    if env -u RORTHOX_TEST_ROOT GITHUB_ACTIONS=true GITHUB_STEP_SUMMARY="$tmp/$name.md" \
        timeout -k 5 120 "$runner" --results "$RES/$name" "$@" > "$OUT/$name.log" 2>&1; then
        status=0
    else
        status=$?
    fi
}

# py <script> <args>...: a Python check, exit 0 when it holds.
py() { python3 -B -c "$@"; }

printf 'echo pass\n' > "$fx/pass.sh"
printf 'echo skip\nexit 77\n' > "$fx/skip.sh"
cat > "$fx/fail.sh" <<'EOF'
o="$RORTHOX_TEST_ROOT/fail"
mkdir -p "$o/out"
echo debugcon line > "$o/out/debugcon.log"
echo "> screen text" > "$o/screen.txt"
printf 'P6\n1 1\n255\nabc' > "$o/screen.ppm"
head -c 2000000 /dev/zero > "$o/big.ppm"
seq 1 400000 > "$o/out/trace.log"
truncate -s 64M "$o/disk.img"
ln -s /etc/passwd "$o/passwd.log"
echo "  FAIL thing"
exit 3
EOF
hostile=$'h|<b>[x](y)!`a`&*_~#@\x01\nz'
cat > "$fx/$hostile.sh" <<'EOF'
printf '  FAIL ]]> </failure> & <x> \033[31m red \001\014 \377 end\n'
printf '::error::from the test\n'
exit 1
EOF
printf 'sleep 60\n' > "$fx/hang.sh"
sel=("$fx/pass.sh" "$fx/skip.sh" "$fx/fail.sh" "$fx/$hostile.sh" "$fx/hang.sh")

echo "reports: 1. results, JUnit, summary and failed output"
top all -j 2 --timeout 3 "${sel[@]}"
R="$RES/all"
expect "exit status 1" '[ "$status" -eq 1 ]'
expect "the reports are written" '[ -s "$R/results.json" ] && [ -s "$R/junit.xml" ] && [ -s "$R/summary.md" ]'
# shellcheck disable=SC2016
expect "results.json: every test, its result, exit status and output directory" "py '
import json, os, sys
r = json.load(open(sys.argv[1] + \"/results.json\"))
want = [(\"pass.sh\", \"passed\", 0), (\"skip.sh\", \"skipped\", 77), (\"fail.sh\", \"failed\", 3),
        (sys.argv[2] + \".sh\", \"failed\", 1), (\"hang.sh\", \"timed out\", 124)]
got = [(os.path.basename(t[\"name\"]), t[\"result\"], t[\"exit_status\"]) for t in r[\"tests\"]]
assert got == want, got
assert [t[\"instance\"] for t in r[\"tests\"]] == [\"001\", \"002\", \"003\", \"004\", \"005\"]
assert all(os.path.isfile(os.path.join(sys.argv[1], t[\"output_dir\"], \"output.log\")) for t in r[\"tests\"])
assert all(t[\"duration\"] >= 0 for t in r[\"tests\"]) and r[\"tests\"][4][\"duration\"] >= 2
assert (r[\"selected\"], r[\"passed\"], r[\"failed\"], r[\"skipped\"], r[\"not_run\"]) == (5, 1, 3, 1, 0)
assert (r[\"shard\"], r[\"jobs\"], r[\"accelerator\"]) == (\"\", 2, \"tcg\")
' \"\$R\" \"\$hostile\""
# shellcheck disable=SC2016
expect "junit.xml parses back: counts, names, failure text" "py '
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1] + \"/junit.xml\").getroot()
s = root.find(\"testsuite\")
for el in (root, s):
    assert (el.get(\"tests\"), el.get(\"failures\"), el.get(\"errors\"), el.get(\"skipped\")) == (\"5\", \"3\", \"0\", \"1\")
cases = s.findall(\"testcase\")
assert [c.get(\"name\").rsplit(\"/\", 1)[1] for c in cases] == [\"pass.sh\", \"skip.sh\", \"fail.sh\", sys.argv[2].replace(\"\x01\", \"?\") + \".sh\", \"hang.sh\"]
assert cases[0].find(\"failure\") is None and cases[1].find(\"skipped\") is not None
assert cases[2].find(\"failure\").get(\"message\") == \"failed (exit status 3)\" and \"  FAIL thing\" in cases[2].find(\"failure\").text
f = cases[3].find(\"failure\").text
assert \"  FAIL ]]> </failure> & <x> ?[31m red ?? \ufffd end\" in f and \"::error::from the test\" in f, repr(f)
assert cases[4].find(\"failure\").get(\"type\") == \"timed out\"
assert {p.get(\"name\"): p.get(\"value\") for p in s.find(\"properties\")} == {\"shard\": \"\", \"jobs\": \"2\", \"accelerator\": \"tcg\"}
' \"\$R\" \"\$hostile\""
S="$R/summary.md"
expect "summary: counts, jobs, shard, accelerator, one row per test" \
    'grep -Fq "1 passed, 3 failed, 1 skipped, 0 not run of 5 in " "$S" &&
     grep -Fq "(up to 2 at once, shard all, accelerator tcg)" "$S" && [ "$(grep -c "^| " "$S")" -eq 7 ]'
expect "summary: failures first, then the rest in order" \
    '[ "$(grep "^| " "$S" | tail -n 5 | cut -d" " -f4 | tr "\n" " ")" = "failed failed timed passed skipped " ] &&
     grep -q "^| .*/fail.sh | failed (exit status 3) | [0-9.]* s |$" "$S" &&
     grep -q "^| .*/hang.sh | timed out (exit status 124) | [0-9.]* s |$" "$S"'
row=$(grep "^| .*h&#124;" "$S")
expect "summary: the hostile name is one row, escaped" \
    '[ "$(grep -c "h&#124;" "$S")" -eq 1 ] &&
     [[ $row == *"/h&#124;&#60;b&#62;&#91;x&#93;(y)&#33;&#96;a&#96;&#38;&#42;&#95;&#126;&#35;&#64;?&#10;z.sh | failed (exit status 1) |"* ]] &&
     [ "$(printf "%s" "$row" | tr -cd "|" | wc -c)" -eq 4 ] && ! printf "%s" "$row" | grep -q "[][<>!\`*_~]"'
expect "summary: added to \$GITHUB_STEP_SUMMARY by the top-level runner" 'cmp -s "$S" "$tmp/all.md"'
expect "annotations: one per failure, the hostile line only behind its name" \
    '[ "$(grep -ac "^::error title=" "$OUT/all.log")" -eq 4 ] && grep -aq "^::error title=.*fail\.sh::  FAIL thing$" "$OUT/all.log" &&
     ! grep -aq "^::error::from the test" "$OUT/all.log"'
# The run directory holds the hostile name twice (its output and its copy in failed/), a name CI
# can't upload: the top-level runner says so in one more annotation.
expect "... and one for the names CI can't upload" \
    '[ "$(grep -ac "^BAD NAME: $R/\(failed/\)\?004-h|<b>" "$OUT/all.log")" -eq 2 ] &&
     grep -aq "^::error title=File names CI cannot upload::" "$OUT/all.log"'
F="$R/failed"
expect "failed/: the three failed tests, not the others" \
    '[ "$(ls -q "$F" | wc -l)" -eq 3 ] && [ -d "$F/003-fail" ] && [ -d "$F/005-hang" ] && [ ! -e "$F/001-pass" ] && [ ! -e "$F/002-skip" ]'
expect "failed/: output, debugcon, screen text and image of the failure" \
    'cmp -s "$F/003-fail/output.log" "$R/003-fail/output.log" && cmp -s "$F/003-fail/fail/out/debugcon.log" "$R/003-fail/fail/out/debugcon.log" &&
     cmp -s "$F/003-fail/fail/screen.txt" "$R/003-fail/fail/screen.txt" && cmp -s "$F/003-fail/fail/screen.ppm" "$R/003-fail/fail/screen.ppm" &&
     grep -q "stopped after 3 seconds" "$F/005-hang/output.log"'
expect "failed/: no disk image, symlink or too big image; index.txt says so" \
    '[ ! -e "$F/003-fail/fail/disk.img" ] && [ ! -e "$F/003-fail/fail/passwd.log" ] && [ ! -e "$F/003-fail/fail/big.ppm" ] &&
     grep -Fxq "left out (67108864 bytes, not a log, text or screen file): fail/disk.img" "$F/003-fail/index.txt" &&
     grep -Fxq "left out (not a regular file): fail/passwd.log" "$F/003-fail/index.txt" &&
     grep -Fxq "left out (2000000 bytes, an image too big to keep): fail/big.ppm" "$F/003-fail/index.txt"'
T="$F/003-fail/fail/out/trace.log"
expect "failed/: a big log keeps its start and end, cut to 1 MiB, and says so" \
    '[ "$(stat -c %s "$T")" -le 1048576 ] && [ "$(stat -c %s "$T")" -gt 1000000 ] && [ "$(head -n 1 "$T")" = 1 ] &&
     [ "$(tail -n 1 "$T")" = 400000 ] && grep -Eq "^\[\.\.\. [0-9]+ bytes cut here \.\.\.\]$" "$T" &&
     grep -Eq "^cut to [0-9]+ of 2688895 bytes \(start and end kept\): fail/out/trace.log$" "$F/003-fail/index.txt"'
expect "failed/: smaller than the run" \
    '[ "$(du -s --block-size=1M "$F" | cut -f1)" -le 2 ] && [ "$(du -s --apparent-size --block-size=1M "$R" | cut -f1)" -ge 64 ]'
rm -rf "$RES/nested" "$tmp/nested.md"
GITHUB_STEP_SUMMARY="$tmp/nested.md" RORTHOX_TEST_ROOT="$RES" "$runner" --results "$RES/nested" "$fx/pass.sh" > "$OUT/nested.log" 2>&1
expect "a runner inside a test writes summary.md but not \$GITHUB_STEP_SUMMARY" \
    '[ -s "$RES/nested/summary.md" ] && [ ! -e "$tmp/nested.md" ]'
mkdir -p "$RES/used" && touch "$RES/used/x"
top used "$fx/pass.sh"
expect "--results needs a new or empty directory" '[ "$status" -eq 2 ] && [ ! -e "$RES/used/plan" ]'

echo "reports: 2. the summary's size limit"
mkdir -p "$RES/small"
cp "$R/plan" "$R/instances" "$RES/small/"
for d in "$R"/[0-9][0-9][0-9]-*; do
    mkdir -p "$RES/small/${d##*/}"
    cp "$d/result" "$d/output.log" "$RES/small/${d##*/}/"
done
python3 -B scripts/run-tests.py report --run-dir "$RES/small" --jobs 2 --shard "" --accelerator tcg --elapsed 1 \
    --summary-limit 500
S="$RES/small/summary.md"
expect "at most 500 bytes, failures first, the rest counted" \
    '[ "$(stat -c %s "$S")" -le 500 ] && [ "$(grep "^| " "$S" | sed -n 3p | grep -c "/fail\.sh | failed")" -eq 1 ] &&
     ! grep -q "/pass\.sh" "$S" &&
     grep -Eq "^[1-4] more test\(s\) not listed here: see results.json.$" "$S"'

echo "reports: 3. a failing test in a shard"
printf '{"tests": {"%s": 10, "%s": 6, "%s": 3}}\n' "$fx/pass.sh" "$fx/skip.sh" "$fx/fail.sh" > "$fx/timings.json"
three=("$fx/fail.sh" "$fx/skip.sh" "$fx/pass.sh")
for i in 1 2; do
    top "shard$i" -j 2 --timings "$fx/timings.json" --shard "$i/2" "${three[@]}"
    eval "status$i=\$status"
done
expect "shard 1 (pass) exits 0, shard 2 (skip, fail) exits 1" '[ "$status1" -eq 0 ] && [ "$status2" -eq 1 ]'
# shellcheck disable=SC2016
expect "the shards' results.json hold the selection once, the failure in shard 2" "py '
import json, sys
a, b = (json.load(open(sys.argv[1] + d + \"/results.json\")) for d in (\"/shard1\", \"/shard2\"))
assert sorted(t[\"name\"] for t in a[\"tests\"] + b[\"tests\"]) == sorted(sys.argv[2:])
assert [t[\"name\"].rsplit(\"/\", 1)[1] for t in b[\"tests\"]] == [\"skip.sh\", \"fail.sh\"]
assert (a[\"shard\"], a[\"failed\"], b[\"shard\"], b[\"failed\"]) == (\"1/2\", 0, \"2/2\", 1)
' \"\$RES\" \"\${three[@]}\""
expect "... and in shard 2's JUnit, summary and annotations only" \
    'grep -q "<testcase name=\"[^\"]*/fail.sh\" classname=\"rorthox\" time=\"[0-9.]*\"><failure message=\"failed (exit status 3)\"" "$RES/shard2/junit.xml" &&
     grep -q "testsuite name=\"RorthOX tests (shard 2/2)\" tests=\"2\" failures=\"1\"" "$RES/shard2/junit.xml" &&
     grep -Fq "0 passed, 1 failed, 1 skipped, 0 not run of 2 in " "$tmp/shard2.md" && grep -Fq "shard 2/2, accelerator tcg" "$tmp/shard2.md" &&
     grep -q "^::error title=.*fail\.sh::" "$OUT/shard2.log" && ! grep -q "^::error" "$OUT/shard1.log" &&
     grep -Fq "1 passed, 0 failed, 0 skipped, 0 not run of 1 in " "$tmp/shard1.md" && [ ! -e "$RES/shard1/failed" ]'

echo "reports: 4. an interrupted run"
mkdir -p "$RES/cancel"
env -u RORTHOX_TEST_ROOT GITHUB_STEP_SUMMARY="$tmp/cancel.md" "$runner" --results "$RES/cancel" "$fx/hang.sh" "$fx/pass.sh" \
    > "$OUT/cancel.log" 2>&1 &
pid=$!
for ((i = 0; i < 100; i++)); do
    [ -s "$RES/cancel/instances" ] && break
    sleep 0.1
done
kill -TERM "$pid"
if wait "$pid"; then status=0; else status=$?; fi
# shellcheck disable=SC2016
expect "exit 1; the stopped test interrupted, the other not run, in every report" "[ \"\$status\" -eq 1 ] && py '
import json, sys, xml.etree.ElementTree as ET
r = json.load(open(sys.argv[1] + \"/results.json\"))
assert [(t[\"result\"], t[\"output_dir\"]) for t in r[\"tests\"]] == [(\"interrupted\", \"001-hang\"), (\"not run\", None)], r
s = ET.parse(sys.argv[1] + \"/junit.xml\").getroot().find(\"testsuite\")
assert (s.get(\"failures\"), s.get(\"errors\")) == (\"1\", \"1\")
assert s.findall(\"testcase\")[1].find(\"error\").get(\"message\") == \"not run: the run was interrupted\"
' \"\$RES/cancel\" && grep -Fq \"0 passed, 1 failed, 0 skipped, 1 not run of 2\" \"\$tmp/cancel.md\""

echo "reports: 5. reports that can't be written fail the run"
printf 'rm -f "$RORTHOX_TEST_ROOT/../plan"\nmkdir "$RORTHOX_TEST_ROOT/../plan"\n' > "$fx/break-plan.sh"
top broken "$fx/break-plan.sh"
expect "a passing test, but no reports: exit 1, and it says so" \
    '[ "$status" -eq 1 ] && grep -Fxq "=== 1 passed, 0 failed, 0 skipped" "$OUT/broken.log" &&
     grep -Fxq "=== the reports could not be written" "$OUT/broken.log"'

if [ "$failures" -ne 0 ]; then
    echo "reports: $failures check(s) failed" >&2
    exit 1
fi
echo "reports: all checks passed"
