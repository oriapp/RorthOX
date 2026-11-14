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
    tcg_only = 'scripts/tests/boot-segments.sh'
    save(tcg / 'results-1' / 'results.json', 'tcg', [test('test-1'), test(tcg_only)])
    for run in runs:
        save(run / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] + [test(tcg_only, 'skipped')])
    check(True, 'TCG-only 1')
    p = subprocess.run(['python3', '-B', str(root / 'scripts/ci-parity.py'), str(tcg),
                        *(str(run) for run in runs)], cwd=root, capture_output=True, text=True)
    assert 'TCG 5.0s; KVM 4.0s, 4.0s, 4.0s; top 5:' in p.stdout, p.stdout
    save(runs[0] / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] + [test(tcg_only, 'failed')])
    check(False, 'KVM 1 scripts/tests/boot-segments.sh: TCG passed, KVM failed')
    save(runs[0] / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] + [test(tcg_only, 'skipped')])
    save(runs[0] / 'results.json', 'kvm', [test('test-1', 'skipped')] +
         [test(f'test-{i}') for i in (2, 3, 4)] + [test(tcg_only, 'skipped')])
    check(False, 'KVM 1 test-1: TCG passed, KVM skipped')
    for run in runs:
        save(run / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    save(tcg / 'results-1' / 'results.json', 'tcg', [test('test-1')])
    import runpy
    escape = runpy.run_path(str(root / 'scripts/ci-parity.py'))['escape']
    assert escape('a:b,c%\r\n') == 'a:b,c%25%0D%0A'
    assert escape('a:b,c%\r\n', property=True) == 'a%3Ab%2Cc%25%0D%0A'
    assert escape('a' * 3499 + '%\n') == 'a' * 3499
    assert escape('a' * 3498 + '%\n') == 'a' * 3498
    target = runs[1] / 'results.json'
    save(target, 'kvm', [test(f'test-{i}', 'failed' if i == 2 else 'passed') for i in (1, 2, 3, 4)])
    check(False, 'KVM 2 test-2: TCG passed, KVM failed')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 3, 4)])
    check(False, 'KVM 2 test-2: TCG passed, KVM missing')
    target.unlink()
    check(False, 'parity error')
    target.write_text('{bad json')
    check(False, 'parity error')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] +
         [test('bad: %\n::error::fake')])
    check(False, 'bad: %25%0A::error')
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    def probe(needle, *, summary=False):
        path = work / 'parity.md'
        path.unlink(missing_ok=True)
        args = ['python3', '-B', str(root / 'scripts/ci-parity.py'), str(tcg),
                *(str(run) for run in runs)]
        if summary:
            args.append(str(path))
        p = subprocess.run(args, cwd=root, capture_output=True, text=True)
        assert p.returncode == 1 and '::notice::KVM/TCG parity OK' not in p.stdout, (p.returncode, p.stdout, p.stderr)
        assert needle in p.stdout and 'Traceback' not in p.stderr, (p.stdout, p.stderr)
        return path.read_text() if summary and path.exists() else ''
    target.write_bytes(b'\xff')
    probe('parity error')  # Invalid UTF-8 is a report error, not a traceback.
    target.write_text('{' * 2000)
    probe('parity error')  # Deep JSON nesting is bounded by the decoder.
    target.write_text('{"accelerator":"kvm","selected":1,"tests":[{"name":"x","result":"passed","duration":1' + '9' * 3000 + '}]}')
    probe('parity error')  # Very large JSON numbers must not escape error handling.
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)] + [test('[evil](https://example.invalid)')])
    summary = probe('parity differs', summary=True)
    assert '&#91;evil&#93;' in summary and '[evil](' not in summary, summary
    # A bad Unicode name must not emit a successful verdict before output fails.
    save(target, 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    save(tcg / 'results-1' / 'results.json', 'tcg', [test('\udcff')])
    for run in runs:
        save(run / 'results.json', 'kvm', [test('\udcff')] + [test(f'test-{i}') for i in (2, 3, 4)])
    probe('parity error', summary=True)
    save(tcg / 'results-1' / 'results.json', 'tcg', [test('test-1')])
    for run in runs:
        save(run / 'results.json', 'kvm', [test(f'test-{i}') for i in (1, 2, 3, 4)])
    shard = tcg / 'results-3' / 'results.json'
    shard.unlink()
    check(False, 'parity error')
    print('PASS ci-parity: equal, failed, missing test, missing/malformed reports')
PY
