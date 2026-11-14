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
    # The required path must consume the image selected by the read-only producer.
    needles = (
        ('legacy-packages', '      - name: Install packages\n        run: sudo apt-get update\n', False),
        ('prep-advisory', '  image-prep:\n    continue-on-error: true', False),
    )
    for tag, addition, _ in needles:
        mutated = text.replace('  image-prep:', addition, 1)
        print('checking', tag, flush=True)
        run(mutated, False)
    edits = (
        ('missing-image-dependency', '    needs: image-prep', '    needs: build'),
        ('missing-shard-image-dependency', '    needs: [image-prep, build]', '    needs: build'),
        ('host-build', './ci/run.sh make all iso', 'make all iso'),
        ('host-test', './ci/run.sh ./scripts/run-tests.sh --shard', './scripts/run-tests.sh --shard'),
        ('missing-receive', 'bash ci/image-job.sh receive', 'echo ignored'),
        ('missing-fetch', 'bash ci/image-job.sh fetch', 'echo ignored'),
        ('no-shard-image-id', 'RORTHOX_IMAGE_ID: ${{ steps.image-id.outputs.id }}', 'RORTHOX_IMAGE_ID: wrong'),
        ('required-failure-masked', '  build:\n', '  build:\n    continue-on-error: true\n'),
        ('bad-permission', '      packages: write', '      packages: read'),
        ('no-prep-timeout', '        timeout-minutes: 100', '        timeout-minutes: 120'),
        ('missing-archive-timeout', '      - name: Transfer candidate image\n        timeout-minutes: 12', '      - name: Transfer candidate image'),
        ('no-pipefail', '        shell: bash # Fail the step when prepare fails, not just when tee fails.\n', ''),
        ('long-archive-retention', '          retention-days: 1', '          retention-days: 90'),
        ('unsafe-trigger', '  pull_request:', '  pull_request_target:'),
        ('run-expression', '          ./ci/run.sh make all iso', '          echo "${{ github.event.inputs.arg }}"'),
    )
    for tag, old, new in edits:
        assert old in text, tag
        mutated = text.replace(old, new, 1)
        print('checking', tag, flush=True)
        run(mutated, False)
    env = os.environ.copy()
    env['GITHUB_ACTIONS'] = 'true'
    env['PYTHONPATH'] = str(no_yaml)
    workflow.write_text(text)
    missing = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                             capture_output=True, env=env)
    assert missing.returncode != 0 and b'PyYAML is required' in missing.stderr
    for value in (r'run: "echo $\x7b{ github.event.inputs.arg }}"',
                  r'run: !!str "echo $\u007b{ github.event.inputs.arg }}"'):
        run(text.replace('run: |', value, 1), False)
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
