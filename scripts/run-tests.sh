#!/usr/bin/env bash
#
# Runs the smoke test and every test in scripts/tests/ (each boots RorthOX in QEMU),
# keeps going after a failure, and prints a summary. Exit status 77 means skipped;
# other nonzero statuses fail. Exit status 1 if any test failed.
#
# Usage:  ./scripts/run-tests.sh        (or: make test)
#         ./scripts/run-tests.sh scripts/tests/panic.sh ...   (only these)

set -uo pipefail
cd "$(dirname "$0")/.."

# CI keeps a copy of each test's combined output while tee streams it to the log.
if [ "${GITHUB_ACTIONS:-}" = true ]; then
    test_logs="$(mktemp -d)"
    trap 'rm -rf "$test_logs"' EXIT
fi

annotate() {
    local title="$1" log="$2" status="$3" message='' line count=0
    local details="$test_logs/details" escaped overflow="${4:-}"
    if [ "$overflow" = overflow ]; then
        cp "$log" "$details"
    else
        grep -a 'FAIL' "$log" | grep -av '^  ok ' > "$details" || true
    fi
    if [ ! -s "$details" ] && [ "$overflow" != overflow ]; then
        tail -n 20 "$log" > "$details"
        # The saved output may end mid-line even though the live stream was separated.
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

if [ $# -gt 0 ]; then
    tests=("$@")
else
    tests=(scripts/smoke.sh)
    for t in scripts/tests/*.sh; do
        [ -e "$t" ] && tests+=("$t")
    done
fi

passed=()
failed=()
skipped=()
for t in "${tests[@]}"; do
    echo "=== $t"
    if [ "${GITHUB_ACTIONS:-}" = true ]; then
        log="$test_logs/test-${#passed[@]}-${#failed[@]}-${#skipped[@]}"
        if bash "$t" 2>&1 | tee "$log"; then status=0; else status=${PIPESTATUS[0]}; fi
        # Keep the next header or annotation separate from unterminated test output.
        if [ -s "$log" ] && [ "$(tail -c1 "$log" | wc -l)" -eq 0 ]; then
            printf '\n'
        fi
    else
        if bash "$t"; then status=0; else status=$?; fi
    fi
    case "$status" in
        0) passed+=("$t") ;;
        77) skipped+=("$t") ;;
        *)
            failed+=("$t")
            if [ "${GITHUB_ACTIONS:-}" = true ]; then
                if [ "${#failed[@]}" -le 9 ]; then
                    annotate "$t" "$log" "$status"
                elif [ "${#failed[@]}" -eq 10 ]; then
                    # Reserve the last annotation for the full overflow list.
                    :
                fi
            fi
            ;;
    esac
done

if [ "${GITHUB_ACTIONS:-}" = true ] && [ "${#failed[@]}" -ge 10 ]; then
    printf '%s more failing tests:\n' "$((${#failed[@]} - 9))" > "$test_logs/overflow"
    printf '%s\n' "${failed[@]:9}" >> "$test_logs/overflow"
    annotate 'Remaining failed tests' "$test_logs/overflow" 1 overflow
fi

echo "=== ${#passed[@]} passed, ${#failed[@]} failed, ${#skipped[@]} skipped"
for t in "${failed[@]}"; do echo "FAILED: $t"; done
for t in "${skipped[@]}"; do echo "SKIPPED: $t"; done
[ ${#failed[@]} -eq 0 ]
