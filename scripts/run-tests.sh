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
    if bash "$t"; then status=0; else status=$?; fi
    case "$status" in
        0) passed+=("$t") ;;
        77) skipped+=("$t") ;;
        *) failed+=("$t") ;;
    esac
done

echo "=== ${#passed[@]} passed, ${#failed[@]} failed, ${#skipped[@]} skipped"
for t in "${failed[@]}"; do echo "FAILED: $t"; done
for t in "${skipped[@]}"; do echo "SKIPPED: $t"; done
[ ${#failed[@]} -eq 0 ]
