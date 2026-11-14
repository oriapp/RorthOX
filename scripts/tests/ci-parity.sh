#!/usr/bin/env bash
# Host-only fixtures for complete reports, divergent results and missing tests.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-parity
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory(dir=sys.argv[2]) as work:
    work = pathlib.Path(work)
    tcg, kvm = work / 'tcg', work / 'kvm'
    expected = work / 'expected.txt'
    expected.write_text(''.join(f'test-{i}\n' for i in range(1, 5)))
    def save(path, accel, tests):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'accelerator': accel, 'selected': len(tests), 'tests': tests}))
    def test(name, result='passed'):
        return {'name': name, 'result': result, 'duration': 1.0}
    def report(side, number):
        return side / f'results-{number}' / 'results.json'
    for i in range(1, 5):
        save(report(tcg, i), 'tcg', [test(f'test-{i}')])
        save(report(kvm, i), 'kvm', [test(f'test-{i}')])
    tests_dir = work / 'tests'
    tests_dir.mkdir()
    (tests_dir / 'boot-segments.sh').write_bytes((root / 'scripts/tests/boot-segments.sh').read_bytes())
    listed = subprocess.run(['python3', '-B', str(root / 'scripts/tcg_only.py'), str(tests_dir)],
                            cwd=root, capture_output=True, text=True, check=True)
    assert listed.stdout.splitlines() == ['scripts/tests/boot-segments.sh'], listed.stdout
    def check(ok, needle, summary=False):
        out = work / 'summary.md'
        out.unlink(missing_ok=True)
        p = subprocess.run(['python3', '-B', str(root / 'scripts/ci-parity.py'), str(tcg), str(kvm), str(expected)] +
                           ([str(out)] if summary else []) + ['--tests-dir', str(tests_dir)],
                           cwd=root, capture_output=True, text=True)
        assert (p.returncode == 0) == ok and needle in p.stdout and 'Traceback' not in p.stderr, (p.returncode, p.stdout, p.stderr)
        return p.stdout, out.read_text() if out.exists() else ''
    check(True, 'parity OK')
    save(report(kvm, 2), 'tcg', [test('test-2')])
    check(False, 'parity error')
    save(report(kvm, 2), 'kvm', [test('test-2')])
    only = 'scripts/tests/boot-segments.sh'
    expected.write_text(expected.read_text() + only + '\n')
    save(report(tcg, 1), 'tcg', [test('test-1'), test(only)])
    save(report(kvm, 1), 'kvm', [test('test-1'), test(only, 'skipped')])
    output, _ = check(True, 'TCG-only 1')
    assert 'TCG 4.0s; KVM 4.0s; top 5:' in output, output
    caller = tests_dir / 'ci-parity-caller.sh'
    caller.write_text('qemu_require_tcg\n')
    check(False, 'TCG-only test list stale')
    caller.unlink()
    save(report(kvm, 1), 'kvm', [test('test-1'), test(only, 'failed')])
    check(False, f'{only}: TCG passed, KVM failed')
    save(report(kvm, 1), 'kvm', [test('test-1', 'skipped'), test(only, 'skipped')])
    check(False, 'test-1: TCG passed, KVM skipped')
    save(report(kvm, 1), 'kvm', [test('test-1'), test(only, 'skipped')])
    escape = runpy.run_path(str(root / 'scripts/ci-parity.py'))['escape']
    assert escape('a:b,c%\r\n') == 'a:b,c%25%0D%0A'
    assert escape('a:b,c%\r\n', property=True) == 'a%3Ab%2Cc%25%0D%0A'
    assert escape('a' * 3499 + '%\n') == 'a' * 3499
    save(report(kvm, 2), 'kvm', [test('test-2', 'failed')])
    check(False, 'test-2: TCG passed, KVM failed')
    report(kvm, 2).unlink()
    check(False, 'incomplete test coverage: TCG missing 0, KVM missing 1')
    report(kvm, 2).write_text('{bad json')
    check(False, 'parity error')
    save(report(kvm, 2), 'kvm', [test('test-2')])
    save(report(kvm, 4), 'kvm', [test('test-4'), test('test-2')])
    check(False, 'duplicate kvm test across shards')
    save(report(kvm, 4), 'kvm', [test('test-4')])
    save(report(kvm, 2), 'kvm', [test('test-2'), test('bad: %\n::error::fake')])
    check(False, 'incomplete test coverage')
    save(report(kvm, 2), 'kvm', [test('test-2')])
    report(kvm, 2).write_bytes(b'\xff')
    check(False, 'parity error')
    report(kvm, 2).write_text('{' * 2000)
    check(False, 'parity error')
    report(kvm, 2).write_text('{"accelerator":"kvm","selected":1,"tests":[{"name":"x","result":"passed","duration":1' + '9' * 3000 + '}]}')
    check(False, 'parity error')
    save(report(kvm, 2), 'kvm', [test('test-2'), test('[evil](https://example.invalid)')])
    check(False, 'incomplete test coverage', summary=True)
    save(report(kvm, 2), 'kvm', [test('test-2')])
    save(report(tcg, 1), 'tcg', [test('\udcff')])
    save(report(kvm, 1), 'kvm', [test('\udcff'), test(only, 'skipped')])
    check(False, 'parity error', summary=True)
    save(report(tcg, 1), 'tcg', [test('test-1'), test(only)])
    save(report(kvm, 1), 'kvm', [test('test-1'), test(only, 'skipped')])
    report(tcg, 3).unlink()
    check(False, 'incomplete test coverage: TCG missing 1, KVM missing 0')
    print('PASS ci-parity: 4+4 shards, TCG-only, divergent, missing, duplicate and malformed reports')
PY
