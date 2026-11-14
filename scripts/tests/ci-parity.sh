#!/usr/bin/env bash
# Host-only fixtures for complete reports, divergent results and missing tests.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-parity
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import json
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(dir=sys.argv[2]) as work:
    work = pathlib.Path(work)
    tcg = work / 'tcg'
    runs = [work / f'kvm-{i}' for i in (1, 2, 3)]
    def save(path, accel, tests):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'accelerator': accel, 'selected': len(tests), 'tests': tests}))
    def test(name, result='passed'):
        return {'name': name, 'result': result, 'duration': 1.0}
    for i in (1, 2, 3, 4):
        save(tcg / f'results-{i}' / 'results.json', 'tcg', [test(f'test-{i}')])
    for run in runs:
        save(run / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    def check(ok, needle):
        p = subprocess.run(['python3', '-B', str(root / 'scripts/ci-parity.py'), str(tcg),
                            *(str(run) for run in runs)], cwd=root, capture_output=True, text=True)
        assert (p.returncode == 0) == ok and needle in p.stdout, (p.returncode, p.stdout, p.stderr)
    check(True, 'parity OK')
    target = runs[1] / 'results.json'
    save(target, 'kvm', [test(f'test-{i}', 'failed' if i == 2 else 'passed') for i in (1, 2, 3, 4)])
    check(False, 'KVM 2 test-2%3A TCG passed, KVM failed')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 3, 4)])
    check(False, 'KVM 2 test-2%3A TCG passed, KVM missing')
    target.unlink()
    check(False, 'parity error')
    target.write_text('{bad json')
    check(False, 'parity error')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] +
         [test('bad: %\n::error::fake')])
    check(False, 'bad%3A %25%0A%3A%3Aerror')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    shard = tcg / 'results-3' / 'results.json'
    shard.unlink()
    check(False, 'parity error')
    print('PASS ci-parity: equal, failed, missing test, missing/malformed reports')
PY
