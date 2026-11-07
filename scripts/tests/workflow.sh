#!/usr/bin/env bash
# Host-only workflow security and aggregate regression checks.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/workflow
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import json
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
    def run(text, valid):
        workflow.write_text(text)
        result = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root, capture_output=True)
        assert (result.returncode == 0) == valid, result.stderr
    run(text, True)
    for tag, broken in (('unpinned', text.replace('actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9', 'actions/cache@v6')),
                        ('credentials', text.replace('persist-credentials: false', 'persist-credentials: true')),
                        ('other-run', text.replace('name: production\n          path:', 'name: production\n          run-id: 12\n          path:')),
                        ('trigger', text.replace('  pull_request:', '  pull_request_target:')),
                        ('skipped-aggregate', text.replace('    if: always()\n    runs-on:', '    if: success()\n    runs-on:')),
                        ('permissions', text.replace('permissions: {}', 'permissions:\n  contents: write')),
                        ('extra-grant', text.replace('      contents: read', '      contents: read\n      id-token: write', 1)),
                        ('shell-expression', text.replace('          make all iso', '          echo "${{ github.event.inputs.arg }}"\n          make all iso'))):
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
