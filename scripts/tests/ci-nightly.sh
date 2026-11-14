#!/usr/bin/env bash
# Host-only nightly aggregate contract with independent report fixtures.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-nightly
mkdir -p "$OUT"
# The aggregate lists the tracked tests with git, so it needs a git checkout (a source archive has none).
if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    echo "ci-nightly: skipped, not a git checkout"
    exit 77
fi
python3 -B - "$PWD" "$OUT" <<'PY'
import json
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(dir=sys.argv[2]) as directory:
    work = pathlib.Path(directory)
    axis, flake, summary = work / 'axis', work / 'flake', work / 'summary.md'
    tracked = subprocess.check_output(['git', 'ls-files', '-z', '--', ':(glob)scripts/tests/*.sh'], cwd=root)
    expected = ['scripts/smoke.sh'] + sorted(p.decode('utf-8') for p in tracked.split(b'\0') if p)
    def save(path, accel, names, failed=None):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'accelerator': accel, 'selected': len(names), 'tests': [
            {'name': name, 'result': ('skipped' if accel == 'kvm' and name == 'scripts/tests/boot-segments.sh'
                                       else 'failed' if name == failed else 'passed'), 'duration': 1.0}
            for name in names]}))
    for accel in ('tcg', 'kvm'):
        for number in range(1, 5):
            names = expected[number - 1::4]
            save(axis / f'axis-{accel}-{number}' / 'results.json', accel, names)
    for number in range(1, 4):
        folder = flake / f'iteration-{number}'
        save(folder / 'results.json', 'tcg', expected)
        (folder / 'status.txt').write_text('0\n')
    (flake / 'test-timings.json').write_text('{}\n')
    def check(label, ok, row=None, jobs=('success',) * 4):
        summary.unlink(missing_ok=True)
        p = subprocess.run(['python3', '-B', 'scripts/ci-nightly.py', str(axis), str(flake), str(summary), str(work / 'staging'), *jobs],
                           cwd=root, capture_output=True, text=True)
        assert (p.returncode == 0) == ok, (label, p.returncode, p.stdout, p.stderr)
        assert ('::notice::' if ok else '::error::') in p.stdout, (label, p.stdout)
        assert summary.exists() and '## Nightly flake hunt' in summary.read_text(), label
        assert (work / 'staging/expected.txt').read_text().splitlines() == expected, label
        if ok:
            assert '## KVM / TCG parity' in summary.read_text(), label
        if row:
            assert row in summary.read_text(), (label, row)
    check('green', True, '| scripts/smoke.sh | 3/3 |')
    (flake / 'iteration-2/status.txt').write_text('1\n')
    check('exit', False, '| scripts/smoke.sh | 3/3 |')
    (flake / 'iteration-2/status.txt').write_text('0\n')
    save(flake / 'iteration-2/results.json', 'tcg', expected, 'scripts/smoke.sh')
    check('failed test', False, '| scripts/smoke.sh | 2/3 |')
    save(flake / 'iteration-2/results.json', 'tcg', expected[1:])
    check('partial test list', False, 'Iteration 2: invalid or missing report')
    save(flake / 'iteration-2/results.json', 'tcg', expected)
    save(axis / 'axis-kvm-1/results.json', 'kvm', expected[0::4], expected[0])
    check('KVM failure', False)
    save(axis / 'axis-kvm-1/results.json', 'kvm', expected[0::4])
    shard = axis / 'axis-kvm-4/results.json'
    shard.rename(shard.with_suffix('.saved'))
    check('missing shard', False)
    shard.with_suffix('.saved').rename(shard)
    (flake / 'test-timings.json').unlink()
    check('missing candidate', False, 'Timing candidate missing.')
    (flake / 'test-timings.json').write_text('{}\n')
    check('job failed', False, jobs=('success', 'success', 'failure', 'success'))
    check('job cancelled', False, jobs=('success', 'success', 'success', 'cancelled'))
    print('PASS ci-nightly: green, exits, reports, missing inputs and job results')
PY
