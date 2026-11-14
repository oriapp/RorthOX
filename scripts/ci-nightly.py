#!/usr/bin/env python3
"""Summarize the nightly accelerator matrix and three serial TCG repetitions."""
import importlib.util
import pathlib
import shutil
import subprocess
import sys

spec = importlib.util.spec_from_file_location('ci_parity', pathlib.Path(__file__).with_name('ci-parity.py'))
parity_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity_module)
Invalid, escape, load, markdown = (getattr(parity_module, key) for key in ('Invalid', 'escape', 'load', 'markdown'))


def main(args):
    if len(args) != 8:
        raise Invalid('usage: ci-nightly.py AXIS_DIR FLAKE_DIR SUMMARY WORK_DIR IMAGE BUILD AXIS FLAKE')
    axis, flake, summary, work = map(pathlib.Path, args[:4])
    # Ignore untracked fixtures created by other tests running in parallel.
    tracked = subprocess.check_output(['git', 'ls-files', '-z', '--', ':(glob)scripts/tests/*.sh'])
    expected = ['scripts/smoke.sh'] + sorted(p.decode('utf-8') for p in tracked.split(b'\0') if p)
    expected_file = work / 'expected.txt'
    expected_file.parent.mkdir(parents=True, exist_ok=True)
    expected_file.write_text('\n'.join(expected) + '\n')
    prepared = work / 'parity'
    if prepared.exists():
        shutil.rmtree(prepared)
    for accel in ('tcg', 'kvm'):
        for number in range(1, 5):
            target = prepared / accel / f'results-{number}'
            target.mkdir(parents=True, exist_ok=True)
            source = axis / f'axis-{accel}-{number}' / 'results.json'
            if source.is_file():
                (target / 'results.json').write_bytes(source.read_bytes())
    parity_summary = work / 'parity.md'
    parity_summary.unlink(missing_ok=True)   # never show an earlier run's parity table
    parity = subprocess.run(['python3', '-B', 'scripts/ci-parity.py',
                             str(prepared / 'tcg'), str(prepared / 'kvm'),
                             str(expected_file), str(parity_summary)], capture_output=True, text=True)
    print(parity.stdout, end='')
    if parity.stderr:
        print(escape(parity.stderr), file=sys.stderr)
    bad = parity.returncode != 0
    lines = ['## Nightly flake hunt', '', 'Three serial TCG runs, one host CPU; no retries.', '']
    counts = dict.fromkeys(expected, 0)
    for number in range(1, 4):
        run = flake / f'iteration-{number}'
        try:
            status = (run / 'status.txt').read_text().strip()
            if status != '0':
                bad = True
            results = load(run / 'results.json', 'tcg')
            if set(results) != set(expected):
                raise Invalid('missing or extra tests')
            for name, (result, _) in results.items():
                counts[name] += result == 'passed'
                if result != 'passed':
                    bad = True
            lines.append(f'Iteration {number}: exit {status}; {sum(item[0] == "passed" for item in results.values())}/{len(expected)} passed.')
        except (Invalid, OSError, UnicodeError, ValueError) as error:
            bad = True
            lines.append(f'Iteration {number}: invalid or missing report ({markdown(str(error))[:160]}).')
    candidate = flake / 'test-timings.json'
    if not candidate.is_file():
        bad = True
        lines.append('Timing candidate missing.')
    lines.extend(['', '| Test | Passes |', '| --- | ---: |'])
    for name in sorted(counts):
        lines.append(f'| {markdown(name)[:160]} | {counts[name]}/3 |')
    lines.append('')
    lines.append('Jobs: ' + ', '.join(f'{name}={markdown(value)[:40]}' for name, value in zip(('image', 'build', 'axis', 'flake'), args[4:])))
    if any(value != 'success' for value in args[4:]):
        bad = True
    summary_text = '\n'.join(lines) + '\n'
    with summary.open('a', encoding='utf-8') as out:
        if parity_summary.is_file():
            out.write(parity_summary.read_text(encoding='utf-8'))
        out.write(summary_text[:12000])
    failed = sum(count != 3 for count in counts.values())
    print(('::error::' if bad else '::notice::') + escape(f'Nightly: {len(expected)} tests, {failed} below 3/3; parity {"failed" if parity.returncode else "OK"}; jobs {args[4:]}'))
    return int(bad)


if __name__ == '__main__':
    try:
        code = main(sys.argv[1:])
    except (Invalid, OSError, UnicodeError, ValueError) as error:
        print('::error::' + escape(f'Nightly aggregate: {error}'))
        code = 1
    sys.exit(code)
