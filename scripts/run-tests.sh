#!/usr/bin/env bash
#
# Runs the smoke test and every test in scripts/tests/ (each boots RorthOX in QEMU),
# keeps going after a failure, and prints a summary. Exit status 1 if any test failed.
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
for t in "${tests[@]}"; do
    echo "=== $t"
    if bash "$t"; then passed+=("$t"); else failed+=("$t"); fi
done

echo "=== ${#passed[@]} passed, ${#failed[@]} failed"
for t in "${failed[@]}"; do echo "FAILED: $t"; done
[ ${#failed[@]} -eq 0 ]
