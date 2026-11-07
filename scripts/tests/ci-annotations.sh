#!/usr/bin/env bash
# Check that the runner reports failures in GitHub's check-run annotations.
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# The runners below are not the CI job's: keep them away from its summary, and keep their runs out
# of the job's test output (CI uploads it, and one test here is called fail:a,b).
export GITHUB_STEP_SUMMARY="$tmp/summary.md" RORTHOX_TEST_ROOT="$tmp"
runner="$PWD/scripts/run-tests.sh"
printf '#!/usr/bin/env bash\necho pass\n' > "$tmp/pass.sh"
printf '#!/usr/bin/env bash\necho skip\nexit 77\n' > "$tmp/skip.sh"
cat > "$tmp/fail:a,b.sh" <<'EOF'
#!/usr/bin/env bash
printf '  FAIL value%%:,%s\r\n' 'one'
printf 'host FAIL second\n'
printf 'end\n'
exit 3
EOF
printf '#!/usr/bin/env bash\necho crash\nexit 42\n' > "$tmp/crash.sh"
printf '#!/usr/bin/env bash\nexit 2\n' > "$tmp/empty.sh"
printf '#!/usr/bin/env bash\nprintf crash-no-newline; exit 5\n' > "$tmp/no-newline.sh"
printf '#!/usr/bin/env bash\necho "  ok   no FAIL line"; echo gave-up; exit 2\n' > "$tmp/okfail.sh"
args=("$tmp/pass.sh" "$tmp/skip.sh" "$tmp/fail:a,b.sh" "$tmp/crash.sh" "$tmp/empty.sh" "$tmp/no-newline.sh" "$tmp/okfail.sh")
for i in {1..11}; do
    printf '#!/usr/bin/env bash\necho "  FAIL extra"\nexit 1\n' > "$tmp/extra$i.sh"
    args+=("$tmp/extra$i.sh")
done
printf '#!/usr/bin/env bash\nfor i in {1..35}; do echo "  FAIL line$i"; done\nexit 1\n' > "$tmp/extra1.sh"
printf '#!/usr/bin/env bash\nprintf "  FAIL "; printf "x%%.0s" {1..5000}; echo\nexit 1\n' > "$tmp/extra2.sh"
# Put the last name beyond the normal 1,200-byte per-test message limit.
for i in {1..45}; do args+=("$tmp/extra11.sh"); done
printf '#!/usr/bin/env bash\necho "  FAIL last"; exit 1\n' > "$tmp/last-overflow-marker.sh"
args+=("$tmp/last-overflow-marker.sh")
if GITHUB_ACTIONS=true "$runner" "${args[@]}" > "$tmp/ci.log" 2>&1; then
    echo 'FAIL: CI runner accepted failures'; exit 1
else
    status=$?
fi
[ "$status" -eq 1 ] || { echo "FAIL: CI exit $status"; exit 1; }
[ "$(grep -c '^::error ' "$tmp/ci.log")" -eq 10 ] || { echo 'FAIL: annotation count'; exit 1; }
grep -Fx -- "::error title=$tmp/fail%3Aa%2Cb.sh::  FAIL value%25:,one%0D%0Ahost FAIL second" "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: escaped message or title'; exit 1;
}
grep -Fx -- "::error title=$tmp/crash.sh::crash%0Aexit status 42" "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: fallback'; exit 1;
}
grep -Fx -- "::error title=$tmp/empty.sh::exit status 2" "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: empty fallback'; exit 1;
}
grep -Fx -- "::error title=$tmp/no-newline.sh::crash-no-newline%0Aexit status 5" "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: unterminated output annotation'; exit 1;
}
grep -Fx -- "::error title=$tmp/okfail.sh::  ok   no FAIL line%0Agave-up%0Aexit status 2" "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: passing line masked failure'; exit 1;
}
# Test output must show in the CI log (each line after its test's name), not just in the annotations.
grep -Fx -- '=== '$tmp'/pass.sh' "$tmp/ci.log" > /dev/null &&
grep -Fx -- '[pass] pass' "$tmp/ci.log" > /dev/null &&
grep -Fx -- '[fail:a,b] end' "$tmp/ci.log" > /dev/null &&
grep -Fx -- '[extra3]   FAIL extra' "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: live output missing'; exit 1;
}
grep -E '^::error .*more lines' "$tmp/ci.log" | grep -q '5 more lines' || {
    echo 'FAIL: line cap'; exit 1;
}
grep -E '^::error .*output truncated' "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: byte cap'; exit 1;
}
[ "$(grep -c 'more failing tests' "$tmp/ci.log")" -eq 1 ] &&
grep '^::error title=Remaining failed tests::' "$tmp/ci.log" | grep -Fq 'last-overflow-marker.sh' || {
    echo 'FAIL: overflow list incomplete'; exit 1;
}
! grep '^::error ' "$tmp/ci.log" | grep -E 'title=.*(pass|skip)\.sh' > /dev/null || {
    echo 'FAIL: pass/skip annotated'; exit 1;
}
grep -Fx -- '=== 1 passed, 62 failed, 1 skipped' "$tmp/ci.log" > /dev/null || {
    echo 'FAIL: summary'; exit 1;
}
if GITHUB_ACTIONS=false "$runner" "${args[@]}" > "$tmp/local.log" 2>&1; then
    echo 'FAIL: local runner accepted failures'; exit 1
else
    status=$?
fi
[ "$status" -eq 1 ] && ! grep '^::error ' "$tmp/local.log" > /dev/null || {
    echo 'FAIL: local runner emitted annotations or changed exit'; exit 1;
}
GITHUB_ACTIONS=true "$runner" "$tmp/pass.sh" "$tmp/skip.sh" > "$tmp/ok.log" 2>&1
! grep '^::error ' "$tmp/ok.log" > /dev/null || { echo 'FAIL: passing runner annotated'; exit 1; }
# Overflow follows the last test even when that test did not terminate its output.
printf '#!/usr/bin/env bash\nprintf tail-no-nl\n' > "$tmp/tail-pass.sh"
printf '#!/usr/bin/env bash\nprintf tail-no-nl; exit 4\n' > "$tmp/tail-fail.sh"
printf '#!/usr/bin/env bash\nprintf "tail-no-nl\\000"\n' > "$tmp/tail-nul.sh"
for last in tail-pass tail-fail tail-nul; do
    overflow_args=()
    for i in {1..10}; do overflow_args+=("$tmp/extra11.sh"); done
    overflow_args+=("$tmp/$last.sh")
    if GITHUB_ACTIONS=true "$runner" "${overflow_args[@]}" > "$tmp/$last.log" 2>&1; then
        echo "FAIL: overflow runner accepted failures ($last)"; exit 1
    else
        status=$?
    fi
    [ "$status" -eq 1 ] || { echo "FAIL: overflow exit ($last): $status"; exit 1; }
    awk -v p="[$last] tail-no-nl" '/^::error title=Remaining failed tests::/ { if (previous != p) exit 1; count++ } { previous=$0 } END { if (count != 1) exit 1 }' "$tmp/$last.log" || {
        echo "FAIL: overflow glued to last output ($last)"; exit 1;
    }
done
[ ! -e "$tmp/summary.md" ] || { echo 'FAIL: a nested runner wrote the job summary'; exit 1; }
echo 'ci-annotations: all checks passed'
