#!/usr/bin/env bash
# Host-only fixtures for scripts/timing-refresh.py: medians of passed runs only, kept and default
# weights, a failing iteration, byte-exact output and bad inputs that must leave no candidate.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/timing-refresh
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
helper = root / 'scripts/timing-refresh.py'
about = runpy.run_path(str(helper))['ABOUT']
load_timings = runpy.run_path(str(root / 'scripts/run-tests.py'))['load_timings']
T = 'scripts/tests/'

with tempfile.TemporaryDirectory(dir=sys.argv[2]) as work:
    work = pathlib.Path(work)
    out = work / 'candidate.json'

    def refresh(current, *files, out=out):
        return subprocess.run(['python3', '-B', str(helper), '--current', str(current), '--out', str(out),
                               *map(str, files)], cwd=root, capture_output=True, text=True)

    def results(path, tests, accelerator='tcg', jobs=1, shard='', selected=None):
        data = {'shard': shard, 'jobs': jobs, 'accelerator': accelerator,
                'selected': len(tests) if selected is None else selected,
                'tests': [{'name': T + n, 'instance': f'{i + 1:03d}', 'result': r, 'exit_status': None,
                           'duration': d, 'output_dir': None} for i, (n, r, d) in enumerate(tests)]}
        path.write_text(json.dumps(data, indent=1) + '\n')
        return path

    current = work / 'current.json'
    current.write_text(json.dumps({'about': 'old', 'tests': {
        T + 'a.sh': 10.0, T + 'b.sh': 5.0, T + 'c.sh': 2.0, T + 'd.sh': 7.0,
        T + 'h.sh': 9.0, T + 'i.sh': 20.0, T + 's.sh': 1.0}}, indent=2))
    P, F, S, TO = 'passed', 'failed', 'skipped', 'timed out'
    # a: median of three passes, not the mean. b: a timeout between two passes never counts; the
    # middle of 4.0 and 4.3 rounds half up to 4.2. c: only failed/skipped runs, keeps 2.0. d: in no
    # run, keeps 7.0. e: new, passed once. f: new, never passed, stays out (the median). g: passed
    # in 0.0 s, at least 0.1. h: interrupted/not run/no result, keeps 9.0. i: fails only in the
    # second run, so the first and third still count. s: skipped every time, keeps 1.0.
    run1 = results(work / 'run1.json', [('a.sh', P, 11.0), ('b.sh', P, 4.0), ('c.sh', F, 1.0),
                   ('e.sh', P, 3.0), ('f.sh', F, 2.0), ('g.sh', P, 0.0), ('h.sh', 'interrupted', None),
                   ('i.sh', P, 10.0), ('s.sh', S, 0.0)])
    run2 = results(work / 'run2.json', [('a.sh', P, 12.0), ('b.sh', TO, 900.0), ('c.sh', S, 0.0),
                   ('f.sh', TO, 900.0), ('g.sh', P, 0.0), ('h.sh', 'not run', None),
                   ('i.sh', F, 99.0), ('s.sh', S, 0.0)])
    run3 = results(work / 'run3.json', [('a.sh', P, 30.0), ('b.sh', P, 4.3), ('c.sh', F, 1.5),
                   ('f.sh', F, 2.0), ('g.sh', P, 0.0), ('h.sh', 'no result', None),
                   ('i.sh', P, 14.0), ('s.sh', S, 0.0)])
    expected = ('{\n  "about": ' + json.dumps(about) + ',\n  "tests": {\n' +
                ',\n'.join(f'    "{T}{n}": {v}' for n, v in (
                    ('a.sh', '12.0'), ('b.sh', '4.2'), ('c.sh', '2.0'), ('d.sh', '7.0'), ('e.sh', '3.0'),
                    ('g.sh', '0.1'), ('h.sh', '9.0'), ('i.sh', '12.0'), ('s.sh', '1.0'))) +
                '\n  }\n}\n')
    p = refresh(current, run1, run2, run3)
    assert p.returncode == 0 and p.stderr == '', (p.returncode, p.stdout, p.stderr)
    first = out.read_bytes()
    assert first.decode() == expected, first.decode()
    assert load_timings(str(out))[T + 'a.sh'] == 12.0
    lines = p.stdout.splitlines()
    assert lines[0] == ('timing refresh: 3 run(s), 5 test(s) measured, 3 changed, 2 new, '
                        '4 kept without a passed run, 1 new without a passed run (median)'), lines
    assert lines[1:4] == [f'  {T}i.sh: 20.0 -> 12.0 s (-8.0)', f'  {T}a.sh: 10.0 -> 12.0 s (+2.0)',
                          f'  {T}b.sh: 5.0 -> 4.2 s (-0.8)'], lines
    assert f'  {T}e.sh: new, 3.0 s' in lines and f'  {T}c.sh: kept 2.0 s (no passed run)' in lines, lines
    # The same inputs give the same bytes, also in another order; an old candidate is replaced.
    out.write_text('stale')
    p = refresh(current, run3, run1, run2)
    assert p.returncode == 0 and out.read_bytes() == first, (p.stdout, p.stderr)
    p = refresh(current, run1, run2, run3)
    assert p.returncode == 0 and out.read_bytes() == first, (p.stdout, p.stderr)
    print('ok: medians of passed runs, kept and default weights, failing second run, same bytes')

    # The real timings file, measured exactly as it stands, comes back unchanged.
    real = load_timings(str(root / 'scripts/test-timings.json'))
    same = work / 'same.json'
    same.write_text(json.dumps({'shard': '', 'jobs': 1, 'accelerator': 'tcg', 'selected': len(real),
                                'tests': [{'name': n, 'result': P, 'duration': d} for n, d in real.items()]}))
    p = refresh(root / 'scripts/test-timings.json', same, same, same)
    assert p.returncode == 0 and load_timings(str(out)) == real, (p.stdout, p.stderr)
    assert ', 0 changed, 0 new, 0 kept' in p.stdout, p.stdout
    print('ok: the real timings file round-trips')

    # The real report writer: a passed run counts, a timed-out and a skipped one don't.
    run_dir = work / 'run'
    run_dir.mkdir()
    names = [T + n for n in ('x1.sh', 'x2.sh', 'x3.sh', 'x4.sh')]
    (run_dir / 'plan').write_bytes(b''.join(n.encode() + b'\0' for n in names))
    (run_dir / 'instances').write_bytes(b''.join(f'{i}\0d{i}\0'.encode() for i in (1, 2, 3, 4)))
    for i, line in enumerate(('0 52 done', '1 9000 timeout', '77 3 done', '0 61 done'), 1):
        (run_dir / f'd{i}').mkdir()
        (run_dir / f'd{i}' / 'result').write_text(line + '\n')
    r = subprocess.run(['python3', '-B', str(root / 'scripts/run-tests.py'), 'report', '--run-dir', str(run_dir),
                        '--jobs', '1', '--shard', '', '--accelerator', 'tcg', '--elapsed', '1'],
                       cwd=root, capture_output=True, text=True, env={'PATH': '/usr/bin:/bin'})
    assert r.returncode in (0, 1) and (run_dir / 'results.json').exists(), (r.returncode, r.stdout, r.stderr)
    current.write_text(json.dumps({'tests': {names[1]: 8.0, names[2]: 1.0}}))
    p = refresh(current, run_dir / 'results.json')
    assert p.returncode == 0 and load_timings(str(out)) == {
        names[0]: 5.2, names[1]: 8.0, names[2]: 1.0, names[3]: 6.1}, (p.stdout, p.stderr, out.read_text())
    print('ok: run-tests.py report output')

    # Bad inputs: exit 2 with a message, no traceback and no candidate left behind.
    current.write_text(json.dumps({'tests': {T + 'a.sh': 10.0}}))
    good = [('a.sh', P, 1.0)]
    def bad(why, *files, cur=current, target=out):
        target.write_text('stale candidate')
        p = refresh(cur, *files, out=target)
        assert p.returncode == 2 and not target.exists() and p.stdout == '', (why, p.returncode, p.stdout, p.stderr)
        assert p.stderr.startswith('timing-refresh: ') and 'Traceback' not in p.stderr, (why, p.stderr)
    count = iter(range(1000))
    def raw(text, name=None):
        path = work / (name or f'bad{next(count)}.json')
        path.write_bytes(text if isinstance(text, bytes) else text.encode())
        return path
    ok = results(work / 'ok.json', good)
    t = lambda **k: {'name': T + 'a.sh', 'result': P, 'duration': 1.0, **k}
    doc = lambda tests, **k: json.dumps({'shard': '', 'jobs': 1, 'accelerator': 'tcg', 'selected': len(tests),
                                         'tests': tests, **k})
    cases = {
        'missing file': work / 'absent.json',
        'not JSON': raw('{bad json'),
        'not UTF-8': raw(b'\xff'),
        'deep nesting': raw('[' * 100000),
        'huge integer': raw(doc([t()]).replace('"duration": 1.0', '"duration": ' + '9' * 5000)),
        'NaN duration': raw(doc([t()]).replace('"duration": 1.0', '"duration": NaN')),
        'infinite duration': raw(doc([t()]).replace('"duration": 1.0', '"duration": 1e400')),
        'duplicate key': raw(doc([t()])[:-1] + ', "jobs": 1}'),
        'not an object': raw('[]'),
        'KVM run': results(work / 'kvm.json', good, accelerator='kvm'),
        'parallel run': results(work / 'jobs.json', good, jobs=4),
        'shard': results(work / 'shard.json', good, shard='1/4'),
        'selected mismatch': results(work / 'sel.json', good, selected=2),
        'empty run': results(work / 'empty.json', []),
        'duplicate test': results(work / 'dup.json', good + good),
        'unknown result': raw(doc([t(result='flaky')])),
        'negative duration': raw(doc([t(duration=-1.0)])),
        'bool duration': raw(doc([t(duration=True)])),
        'string duration': raw(doc([t(duration='1.0')])),
        'passed without duration': raw(doc([t(duration=None)])),
        'nameless test': raw(doc([t(name='')])),
        'test not an object': raw(doc(['a.sh'])),
    }
    raw(doc([t()]), 'check.json')
    assert refresh(current, work / 'check.json').returncode == 0  # the case builder makes valid files
    for why, path in cases.items():
        bad(why, ok, path)  # a good file before a bad one still gives no candidate
    for why, text in (('bad --current', '{"tests": {"x": 0}}'), ('current not JSON', '{'),
                      ('current without tests', '{"about": "x"}')):
        bad(why, ok, cur=raw(text, 'cur.json'))
    bad('no results file')
    keep = current.read_bytes()
    p = refresh(current, ok, out=current)
    assert p.returncode == 2 and current.read_bytes() == keep, (p.stdout, p.stderr)
    p = refresh(current, ok, out=ok)
    assert p.returncode == 2 and ok.exists(), (p.stdout, p.stderr)
    print(f'ok: {len(cases) + 4} bad inputs leave no candidate; the inputs are never the output')
    print('PASS timing-refresh')
PY
