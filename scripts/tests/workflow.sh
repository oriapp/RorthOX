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
            env.pop('GITHUB_ACTIONS', None)  # Only the explicit Actions probe requires PyYAML.
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
    # Every security rule must reject a realistic change in both parser modes.
    expr = '${{ github.event.inputs.arg }}'
    build_run = 'run: |\n          ./ci/run.sh make all iso'
    negatives = (
        ('unpinned', 'actions/upload-artifact@b7c566a772e6b6bfb58ed0dc250532a479d7789f', 'actions/upload-artifact@v6'),
        ('credentials', 'persist-credentials: false', 'persist-credentials: true'),
        ('other-run', 'name: production\n          path:', 'name: production\n          run-id: 12\n          path:'),
        ('skipped-aggregate', '    if: always()\n    runs-on:', '    if: success()\n    runs-on:'),
        ('permissions', 'permissions: {}', 'permissions:\n  contents: write'),
        ('extra-grant', '      contents: read', '      contents: read\n      id-token: write'),
        ('publication-gate', "github.repository == 'oriapp/RorthOX' && ", ''),
        ('publish-not-advisory', '  image:\n    continue-on-error: true\n', '  image:\n'),
        ('publish-no-step-timeout', '      - name: Publish only after validation\n        timeout-minutes: 20\n', '      - name: Publish only after validation\n'),
        ('prep-permission', '  image-prep:\n    runs-on: ubuntu-24.04\n    timeout-minutes: 120 # Cold candidate builds have a 100-minute step limit.\n    permissions:\n      contents: read\n', '  image-prep:\n    runs-on: ubuntu-24.04\n    timeout-minutes: 120 # Cold candidate builds have a 100-minute step limit.\n    permissions:\n      contents: read\n      packages: write\n'),
        ('secret-in-test', '          SHARD: ${{ matrix.shard }}\n', '          SHARD: ${{ matrix.shard }}\n          T: ${{ secrets.GITHUB_TOKEN }}\n'),
        ('duplicate-secret', '          GHCR_TOKEN: ${{ secrets.GITHUB_TOKEN }}', '          GHCR_TOKEN: ${{ secrets.GITHUB_TOKEN }}\n          EXTRA: ${{ secrets.GITHUB_TOKEN }}'),
        ('token-in-build', '    needs: image-prep', '    needs: image-prep\n    env:\n      T: ${{ github.token }}'),
        ('token-in-workflow', 'permissions: {}', 'permissions: {}\nenv:\n  T: ${{ github.token }}'),
        ('secret-index', '          SHARD: ${{ matrix.shard }}', '          SHARD: ${{ matrix.shard }}\n          T: ${{ secrets[\"GITHUB_TOKEN\"] }}'),
        ('secret-whole', '          SHARD: ${{ matrix.shard }}', '          SHARD: ${{ matrix.shard }}\n          T: ${{ toJSON(secrets) }}'),
        ('github-index', '          SHARD: ${{ matrix.shard }}', '          SHARD: ${{ matrix.shard }}\n          T: ${{ github[\"token\"] }}'),
        ('github-whole', '          SHARD: ${{ matrix.shard }}', '          SHARD: ${{ matrix.shard }}\n          T: ${{ toJSON(github) }}'),
        ('aggregate-masked', '      - name: Check every job\n', '      - name: Check every job\n        continue-on-error: true\n'),
        ('required-test-masked', '  test:\n    needs:', '  test:\n    continue-on-error: true\n    needs:'),
        ('required-prep-masked', '  image-prep:\n    runs-on:', '  image-prep:\n    continue-on-error: true\n    runs-on:'),
        ('aggregate-job-masked', '  result:\n    needs:', '  result:\n    continue-on-error: true\n    needs:'),
        ('kvm-required', '    needs: [build, test]\n    if: always()', '    needs: [build, test, kvm]\n    if: always()'),
        ('kvm-in-pr', "    if: github.event_name == 'push' && github.repository == 'oriapp/RorthOX'\n    continue-on-error: true", '    if: always()\n    continue-on-error: true'),
        ('kvm-as-tcg', '          RORTHOX_ACCEL: kvm', '          RORTHOX_ACCEL: tcg'),
        ('publish-commented-needs', '    needs: [image-prep, build, test]\n    if: github.event_name', '    needs: image-prep # needs: [image-prep, build, test]\n    if: github.event_name'),
        ('no-logout-always', '        if: always()\n        run: docker logout ghcr.io', '        run: docker logout ghcr.io'),
        ('run-after-logout', '          echo "ref=$IMAGE_REF" >> "$GITHUB_OUTPUT"\n', '          echo "ref=$IMAGE_REF" >> "$GITHUB_OUTPUT"\n          docker run --rm "$IMAGE_REF" true\n'),
        ('publish-early', '    needs: [image-prep, build, test]\n    if: github.event_name', '    needs: image-prep\n    if: github.event_name'),
        ('spaced-coe', '        id: image-id\n', '        id: image-id\n        continue-on-error : true\n'),
        ('late-prep-coe', '  image-prep:\n    runs-on: ubuntu-24.04', '  image-prep:\n    runs-on: ubuntu-24.04\n    continue-on-error: true'),
        ('commented-fast', '      fail-fast: false', '      fail-fast: true # fail-fast: false'),
        ('dash-run', '      - name: Build and package', '      - run: echo "' + expr + '"\n      - name: Build and package'),
        ('next-line-run', '      - name: Build and package', '      - run:\n          echo "' + expr + '"\n      - name: Build and package'),
        ('keep-run', '      - name: Build and package', '      - run: |+\n          echo "' + expr + '"\n      - name: Build and package'),
        ('indent-run', '      - name: Build and package', '      - run: |2\n          echo "' + expr + '"\n      - name: Build and package'),
        ('fold-run', '      - name: Build and package', '      - run: >+\n          echo "' + expr + '"\n      - name: Build and package'),
        ('continued-run', build_run, 'run: ./ci/run.sh make all iso\n            echo "' + expr + '"'),
        ('quoted-run', '        with:\n          persist-credentials: false', '        "with":\n          persist-credentials: false'),
        ('spaced-run', '        run: |', '        run : echo "' + expr + '"'),
        ('flow-run', '      - name: Build and package', '      - {name: Build and package, run: "echo ' + expr + '"}\n      - name: Build and package'),
        ('alias-run', '      - name: Build and package', '      - name: Label\n        value: &cmd "echo ' + expr + '"\n      - run: *cmd\n      - name: Build and package'),
        ('quoted-space-run', build_run, '"run" : echo "' + expr + '"'),
        ('escaped-key-run', build_run, '"r\x75n": echo "' + expr + '"'),
        ('explicit-key-run', build_run, '? run\n        : echo "' + expr + '"'),
        ('tagged-key-run', build_run, '!!str run: echo "' + expr + '"'),
        ('escaped-hex-run', '      - name: Build and package', '      - run: "echo $\\x7b{ github.event.inputs.arg }"\n      - name: Build and package'),
        ('escaped-line-run', '      - name: Build and package', '      - run: "echo $\\\n          {{ github.event.inputs.arg }}"\n      - name: Build and package'),
        ('nextline-escaped-run', '      - name: Build and package', '      - run:\n          "echo $\\x7b{ github.event.inputs.arg }"\n      - name: Build and package'),
        ('comment-escaped-run', '      - name: Build and package', '      - run: # c\n          "echo $\\x7b{ github.event.inputs.arg }"\n      - name: Build and package'),
        ('verbatim-tag-run', '      - name: Build and package', '      - run: !<tag:yaml.org,2002:str> "echo $\\x7b{ github.event.inputs.arg }"\n      - name: Build and package'),
    )
    for tag, old, new in negatives:
        assert old in text and new != old, tag
        print('checking', tag, flush=True)
        run(text.replace(old, new, 1), False)
    # Exercise decoded YAML context references and the workflow/job boundaries.
    kvm_head = text.index('\n  kvm:\n')
    def kvm_change(old, new):
        head, body = text[:kvm_head], text[kvm_head:]
        assert old in body
        return head + body.replace(old, new, 1)
    credential_step = '          RORTHOX_IMAGE_ID: ${{ steps.image-id.outputs.id }}\n'
    cases = (
        ('escaped-secret-hex', text.replace(credential_step, credential_step +
            r'          T: "${{ \x73ecrets.GITHUB_TOKEN }}"' + '\n', 1), 'credentials outside publication'),
        ('escaped-secret-unicode', text.replace(credential_step, credential_step +
            r'          T: "${{ \u0073ecrets.GITHUB_TOKEN }}"' + '\n', 1), 'credentials outside publication'),
        ('escaped-token', kvm_change(credential_step, credential_step +
            r'          T: "${{ github.tok\x65n }}"' + '\n'), 'credentials outside publication'),
        ('escaped-index', kvm_change(credential_step, credential_step +
            r'''          T: "${{ github\x5b'token'] }}"''' + '\n'), 'credentials outside publication'),
        ('github-filter', kvm_change(credential_step, credential_step +
            "          T: ${{ join(github.*, ',') }}\n"), 'credentials outside publication'),
        ('value-jobs-env', text.replace('permissions: {}\n',
            'permissions: {}\nenv:\n  NOTE: jobs:x\n  T: ${{ github.token }}\n', 1),
            'credentials outside publication'),
        ('comment-jobs-env', text.replace('permissions: {}\n',
            'permissions: {}\n# jobs: unrelated\nenv:\n  T: ${{ github.token }}\n', 1),
            'credentials outside publication'),
        ('kvm-image-id', kvm_change(credential_step, ''), 'KVM job must use the same verified image'),
        ('kvm-manifest', kvm_change('build/ci/production.tar.gz "$MANIFEST_SHA256"',
            'build/ci/production.tar.gz "$(sha256sum build/ci/production.tar.gz | cut -c1-64)"'),
            'KVM job must use the same verified image'),
        ('kvm-commented-accel', kvm_change('          RORTHOX_ACCEL: kvm\n',
            '          # RORTHOX_ACCEL: kvm\n          RORTHOX_ACCEL: tcg\n'),
            'KVM job must use the same verified image'),
        ('kvm-no-udev', kvm_change('            echo \'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"\' | sudo tee /etc/udev/rules.d/99-kvm4all.rules\n', ''),
            'KVM sudo is confined to device setup'),
        ('kvm-privileged', kvm_change('      - name: Add KVM test summary\n        timeout-minutes: 2\n',
            '      - name: Extra\n        timeout-minutes: 1\n        run: docker run --privileged --rm alpine true\n      - name: Add KVM test summary\n        timeout-minutes: 1\n'),
            'KVM job must not run host Docker'),
        ('kvm-container-diagnostic', kvm_change('        run: ./ci/run.sh bash ci/kvm-diagnostics.sh container',
            '        run: echo skipped'),
            'KVM job must use the same verified image'),
        ('kvm-extract', kvm_change('      - name: Extract KVM production build',
            '      - name: Unpack KVM production build'), 'KVM job must use the same verified image'),
        ('kvm-test-timeout', kvm_change("          RORTHOX_TEST_TIMEOUT: '300'\n", ''),
            'KVM job must use the same verified image'),
        ('kvm-upload', kvm_change('      - name: Upload KVM results\n        timeout-minutes: 2\n        if: always()',
            '      - name: Upload KVM results\n        timeout-minutes: 2\n        if: success()'),
            'KVM job must use the same verified image'),
        ('kvm-step-timeout', kvm_change('      - name: Test with KVM\n        timeout-minutes: 31',
            '      - name: Test with KVM'), 'kvm: step'),
        ('kvm-override-accel', kvm_change('          ./ci/run.sh ./scripts/run-tests.sh -j 2',
            '          export RORTHOX_ACCEL=tcg\n          ./ci/run.sh ./scripts/run-tests.sh -j 2'), 'KVM run must not override its accelerator'),
        ('github-nested', kvm_change(credential_step, credential_step +
            '          T: ${{ toJSON((github)) }}\n'), 'credentials outside publication'),
        ('github-or', kvm_change(credential_step, credential_step +
            '          T: ${{ toJSON(null || github) }}\n'), 'credentials outside publication'),
        ('github-and', kvm_change(credential_step, credential_step +
            '          T: ${{ toJSON(true && github) }}\n'), 'credentials outside publication'),
        ('escaped-expression-start', kvm_change(credential_step, credential_step +
            r'          T: "\x24\x7b\x7b \x73ecrets.GITHUB_TOKEN }}"' + '\n'), 'escaped scalar requires YAML parser'),
        ('binary-tag', kvm_change(credential_step, credential_step +
            '          T: !!binary JHt7IHNlY3JldHMuR0lUSFVCX1RPS0VOIH19\n'), 'YAML tags are not supported'),
        ('inline-binary-tag', kvm_change(credential_step, credential_step +
            '          T: [!!binary JHt7IHNlY3JldHMuR0lUSFVCX1RPS0VOIH19]\n'), 'YAML tags are not supported'),
        ('docker-create', kvm_change('          bash ci/kvm-diagnostics.sh host "$before" "$after"',
            '            docker create alpine true'), 'KVM job must use the same verified image'),
        ('sudo-extra', kvm_change('            sudo udevadm settle',
            '            sudo chmod 0666 /var/run/docker.sock\n            sudo udevadm settle'), 'KVM sudo is confined to device setup'),
        ('host-script', kvm_change('          ./ci/run.sh ./scripts/run-tests.sh -j 2 --results build/tests/runs/kvm',
            '          ./ci/run.sh ./scripts/run-tests.sh -j 2 --results build/tests/runs/kvm\n          ./scripts/accelerator.sh'), 'KVM scripts must run inside the image'),
        ('kvm-job-container', kvm_change('    runs-on: ubuntu-24.04\n',
            '    runs-on: ubuntu-24.04\n    container: alpine:latest\n'), 'KVM job must not run host Docker'),
        ('commented-image-id', kvm_change(credential_step, '          # RORTHOX_IMAGE_ID: ${{ steps.image-id.outputs.id }}\n'), 'KVM job must use the same verified image'),
        ('commented-manifest', kvm_change('          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}',
            '          # MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}\n          MANIFEST_SHA256: deadbeef'), 'KVM job must use the same verified image'),
        ('commented-extract', kvm_change('          ./ci/run.sh python3 -B scripts/ci-artifact.py extract build/ci/production.tar.gz "$MANIFEST_SHA256"',
            '          # ./ci/run.sh python3 -B scripts/ci-artifact.py extract build/ci/production.tar.gz "$MANIFEST_SHA256"\n          ./ci/run.sh python3 -B scripts/ci-artifact.py extract build/ci/production.tar.gz "$(sha256sum build/ci/production.tar.gz)"'), 'KVM job must use the same verified image'),
        ('commented-test-limit', kvm_change("          RORTHOX_TEST_TIMEOUT: '300'", "          # RORTHOX_TEST_TIMEOUT: '300'"), 'KVM job must use the same verified image'),
    )
    cases += (
        ('continued-escaped-secret', kvm_change(credential_step, credential_step +
            '          T: "safe\n' + r'            \x24\x7b\x7b \x73ecrets.GITHUB_TOKEN }}"' + '\n'),
            'escaped scalar requires YAML parser'),
        ('duplicate-kvm-step-name', kvm_change('      - name: Test with KVM\n        timeout-minutes: 31',
            '      - name: Test with KVM\n        timeout-minutes: 1\n        run: echo duplicate\n'
            '      - name: Test with KVM\n        timeout-minutes: 30'), 'duplicate KVM step name'),
    )
    for tag, mutated, reason in cases:
        print('checking', tag, flush=True)
        workflow.write_text(mutated)
        for path in (None, no_yaml):
            mode = 'yaml' if path is None else 'fallback'
            env = os.environ.copy()
            env.pop('GITHUB_ACTIONS', None)
            if path is not None:
                env['PYTHONPATH'] = str(path)
            result = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                                    capture_output=True, text=True, env=env)
            expected = ('escaped scalar requires YAML parser' if path is not None and
                        tag in ('escaped-secret-hex', 'escaped-secret-unicode', 'escaped-token', 'escaped-index', 'escaped-expression-start', 'continued-escaped-secret')
                        else 'credentials outside publication' if path is None and tag in ('escaped-expression-start', 'continued-escaped-secret')
                        else reason)
            assert result.returncode != 0 and expected in result.stderr, (tag, mode, result.stderr)
    env = os.environ.copy()
    env['GITHUB_ACTIONS'] = 'true'
    env['PYTHONPATH'] = str(no_yaml)
    workflow.write_text(text)
    missing = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                             capture_output=True, env=env)
    assert missing.returncode != 0 and b'PyYAML is required' in missing.stderr
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
