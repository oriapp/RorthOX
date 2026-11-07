#!/usr/bin/env python3
"""Summarize same-run shard reports; fail unless the build and every shard succeeded."""
import json
import os
import pathlib
import sys


def main(root, build, shards):
    lines = ['## CI results', '', '| Job | Conclusion | Passed | Failed | Skipped |',
             '| --- | --- | ---: | ---: | ---: |']
    ok = build == 'success' and shards == 'success'
    lines.append(f'| build | {build} | — | — | — |')
    for i in range(1, 5):
        path = pathlib.Path(root) / f'results-{i}' / 'results.json'
        if not path.is_file():
            lines.append(f'| test {i} | missing | — | — | — |')
            ok = False
            continue
        try:
            data = json.loads(path.read_text())
            tests = data['tests']
            if not isinstance(tests, list) or not all(isinstance(t, dict) and 'result' in t for t in tests):
                raise ValueError('invalid tests list')
        except (OSError, ValueError, KeyError, TypeError):
            lines.append(f'| test {i} | invalid report | — | — | — |')
            ok = False
            continue
        counts = {key: sum(t['result'] == key for t in tests) for key in ('passed', 'skipped')}
        # Other result spellings (crash, timeout, etc.) must not be called a pass.
        bad = sum(t['result'] not in ('passed', 'skipped') for t in tests)
        lines.append(f'| test {i} | {"failed" if bad else "reported"} | '
                     f'{counts["passed"]} | {bad} | {counts["skipped"]} |')
        ok &= bad == 0
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
