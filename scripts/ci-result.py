#!/usr/bin/env python3
"""Summarize same-run shard reports; fail unless the build and every shard succeeded."""
import json
import os
import pathlib
import re
import sys


def main(root, build, shards):
    lines = ['## CI results', '', '| Job | Conclusion | Accelerator | Passed | Failed | Skipped |',
             '| --- | --- | --- | ---: | ---: | ---: |']
    ok = build == 'success' and shards == 'success'
    lines.append(f'| build | {build} | — | — | — | — |')
    for i in range(1, 6):
        label = f'test {i}' if i <= 4 else 'TCG-only'
        title = f'Test shard {i}' if i <= 4 else 'TCG-only test'
        folder = pathlib.Path(root) / ('results-1/tcg-only' if i == 5 else f'results-{i}')
        path = folder / 'results.json'
        if not path.is_file():
            lines.append(f'| {label} | missing | — | — | — | — |')
            ok = False
            continue
        try:
            data = json.loads(path.read_text())
            tests = data['tests']
            accel = data['accelerator']
            reason = data['accelerator_reason']
            if accel not in ('kvm', 'tcg') or (i == 5 and accel != 'tcg') or not isinstance(reason, str) or not reason:
                raise ValueError('invalid accelerator')
            if not isinstance(tests, list) or not tests or not all(isinstance(t, dict) and 'result' in t for t in tests):
                raise ValueError('invalid tests list')
            if i == 5:
                callers = {str(test) for test in pathlib.Path('scripts/tests').glob('*.sh')
                           if test.name != 'accelerator.sh' and
                           re.search(r'^\s*qemu_require_tcg\b', test.read_text(), re.M)}
                names = [test.get('name') for test in tests]
                if not callers or set(names) != callers or len(names) != len(callers):
                    raise ValueError('TCG-only tests missing from report')
        except (OSError, ValueError, KeyError, TypeError):
            lines.append(f'| {label} | invalid report | — | — | — | — |')
            ok = False
            continue
        counts = {key: sum(t['result'] == key for t in tests) for key in ('passed', 'skipped')}
        # Other result spellings (crash, timeout, etc.) must not be called a pass.
        bad = sum(t['result'] not in ('passed', 'skipped') for t in tests)
        safe = reason[:1500].replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
        print(f'::notice title={title} accelerator::{accel}: {safe}')
        lines.append(f'| {label} | {"failed" if bad else "reported"} | {accel} | '
                     f'{counts["passed"]} | {bad} | {counts["skipped"]} |')
        ok &= bad == 0 and (i != 5 or counts['skipped'] == 0)
    lines.extend(['', f'Overall: {"success" if ok else "failure"}.'])
    summary = '\n'.join(lines) + '\n'
    print(summary, end='')
    if 'GITHUB_STEP_SUMMARY' in os.environ:
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f:
            f.write(summary)
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: ci-result.py RESULTS_DIR BUILD_CONCLUSION TEST_CONCLUSION')
    sys.exit(main(*sys.argv[1:]))
