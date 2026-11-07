#!/usr/bin/env bash
# Host-only workflow security and aggregate regression checks.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/workflow
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import json
import os
import pathlib
import subprocess
import sys
import tempfile

root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
text = (root / '.github/workflows/build.yml').read_text()
lint = root / 'scripts/workflow-lint.py'
with tempfile.TemporaryDirectory() as temp:
    temp = pathlib.Path(temp)
    workflow = temp / 'build.yml'
    no_yaml = temp / 'no-yaml'
    no_yaml.mkdir()
    (no_yaml / 'yaml.py').write_text('raise ImportError("simulated missing PyYAML")\n')
    def run(text, valid):
        workflow.write_text(text)
        for path in (None, no_yaml):
            env = os.environ.copy()
            if path is not None:
                env['PYTHONPATH'] = str(path)
            result = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                                    capture_output=True, env=env)
            assert (result.returncode == 0) == valid, result.stderr
    run(text, True)
    # A dash-style run's sibling name/env expressions are not shell expressions.
    run(text.replace('      - name: Build and package',
                     '      - run: echo "$ARG"\n        env:\n          ARG: ${{ github.event.inputs.arg }}\n'
                     '        name: Check ${{ matrix.shard }}\n      - name: Build and package', 1), True)
    for tag, broken in (('unpinned', text.replace('actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9', 'actions/cache@v6')),
                        ('credentials', text.replace('persist-credentials: false', 'persist-credentials: true')),
                        ('other-run', text.replace('name: production\n          path:', 'name: production\n          run-id: 12\n          path:')),
                        ('trigger', text.replace('  pull_request:', '  pull_request_target:')),
                        ('skipped-aggregate', text.replace('    if: always()\n    runs-on:', '    if: success()\n    runs-on:')),
                        ('permissions', text.replace('permissions: {}', 'permissions:\n  contents: write')),
                        ('extra-grant', text.replace('      contents: read', '      contents: read\n      id-token: write', 1)),
                        ('shell-expression', text.replace('          make all iso', '          echo "${{ github.event.inputs.arg }}"\n          make all iso')),
                        ('dash-run', text.replace('      - name: Build and package', '      - run: echo "${{ github.event.inputs.arg }}"\n      - name: Build and package', 1)),
                        ('next-line-run', text.replace('run: |\n          make all iso', 'run:\n          echo "${{ github.event.inputs.arg }}"', 1)),
                        ('keep-run', text.replace('run: |\n          make all iso', 'run: |+\n          echo "${{ github.event.inputs.arg }}"', 1)),
                        ('indent-run', text.replace('run: |\n          make all iso', 'run: |2\n          echo "${{ github.event.inputs.arg }}"', 1)),
                        ('fold-run', text.replace('run: |\n          make all iso', 'run: >+\n          echo "${{ github.event.inputs.arg }}"', 1)),
                        ('continued-run', text.replace('run: |\n          make all iso', 'run: make all iso\n            echo "${{ github.event.inputs.arg }}"', 1)),
                        ('quoted-run', text.replace('        run: |', '        "run": echo "${{ github.event.inputs.arg }}"', 1)),
                        ('spaced-run', text.replace('        run: |', '        run : echo "${{ github.event.inputs.arg }}"', 1)),
                        ('flow-run', text.replace('      - name: Build and package', '      - {name: Build and package, run: "echo ${{ github.event.inputs.arg }}"}\n      - name: Build and package', 1)),
                        ('alias-run', text.replace('      - name: Build and package', '      - name: Label\n        value: &cmd "echo ${{ github.event.inputs.arg }}"\n      - run: *cmd\n      - name: Build and package', 1)),
                        ('quoted-space-run', text.replace('run: |\n          make all iso', '"run" : echo "${{ github.event.inputs.arg }}"', 1)),
                        ('single-quoted-space-run', text.replace('run: |\n          make all iso', "'run'  : echo '${{ github.event.inputs.arg }}'", 1)),
                        ('escaped-key-run', text.replace('run: |\n          make all iso', '"r\\x75n": echo "${{ github.event.inputs.arg }}"', 1)),
                        ('explicit-key-run', text.replace('run: |\n          make all iso', '? run\n        : echo "${{ github.event.inputs.arg }}"', 1)),
                        ('tagged-key-run', text.replace('run: |\n          make all iso', '!!str run: echo "${{ github.event.inputs.arg }}"', 1))):
        print('checking', tag, flush=True)
        run(broken, False)
        print('PASS rejected', tag)
    results = temp / 'results'
    for i in range(1, 5):
        d = results / f'results-{i}'
        d.mkdir(parents=True)
        (d / 'results.json').write_text(json.dumps({'tests': [{'result': 'passed'}]}))
    aggregate = root / 'scripts/ci-result.py'
    def check(build, shards, success):
        proc = subprocess.run(['python3', '-B', str(aggregate), str(results), build, shards],
                              capture_output=True, text=True)
        assert (proc.returncode == 0) == success, proc.stdout + proc.stderr
    check('success', 'success', True)
    check('failure', 'skipped', False)
    report = results / 'results-3/results.json'
    report.write_text(json.dumps({'tests': [{'result': 'failed'}]}))
    check('success', 'success', False)
    assert 'failed' in subprocess.run(['python3', '-B', str(aggregate), str(results),
                                       'success', 'success'], capture_output=True, text=True).stdout
    report.write_text('{not json')
    check('success', 'success', False)
    report.write_text(json.dumps({'tests': [{}]}))
    check('success', 'success', False)
    report.write_text(json.dumps({'tests': [{'result': 'passed'}]}))
    (results / 'results-2/results.json').unlink()
    check('success', 'success', False)
    print('PASS aggregate requires every job and report')
(out / 'result.txt').write_text('workflow: all cases passed\n')
PY
