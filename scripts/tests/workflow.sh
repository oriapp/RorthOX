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
import shutil
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
    # Required shards must not lose host device setup, select forced TCG or copy
    # arbitrary artifacts. Both YAML and parser-free lint reject these edits.
    shard_cases = (
        ('no-shard-udev', '            if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi', '            echo no-device'),
        ('forced-shard-tcg', '          RORTHOX_ACCEL: auto', '          RORTHOX_ACCEL: tcg'),
        ('shard-rsync', '          ./ci/run.sh ./scripts/run-tests.sh --shard',
         '          rsync -a /tmp/x build/tests/\n          ./ci/run.sh ./scripts/run-tests.sh --shard'),
        ('shard-sudo', '            if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi',
         '            sudo id\n            if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi'),
    )
    for tag, old, new in shard_cases:
        assert old in text, tag
        print('checking', tag, flush=True)
        run(text.replace(old, new, 1), False)
    # Each payload targets one rule, not a second unrelated prohibition.
    shard_run = '          ./ci/run.sh ./scripts/run-tests.sh --shard'
    cases = (
        ('host-unhandled-udev',
         'if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi',
         'if [ -z "$failed" ]; then sudo udevadm settle --timeout=30; fi', 'KVM host setup'),
        ('host-no-settle-timeout', 'sudo udevadm settle --timeout=30',
         'sudo udevadm settle', 'KVM host setup'),
        ('sudo-other-job', '      - name: Build and package', '      - run: sudo id\n      - name: Build and package', 'KVM sudo'),
        ('sudo-decoded', '      - name: Verify and test', '      - run: "su\\x64o id"\n      - name: Verify and test', 'KVM sudo'),
        ('shard-docker', shard_run, '          docker create something\n' + shard_run, 'without host Docker'),
        ('shard-container', '  test:\n', '  test:\n    container: ubuntu:24.04\n', 'without host Docker'),
        ('shard-services', '  test:\n', '  test:\n    services: {}\n', 'without host Docker'),
        ('shard-timeout', '          RORTHOX_ACCEL: auto\n', "          RORTHOX_ACCEL: auto\n          RORTHOX_TEST_TIMEOUT: '300'\n", 'without host Docker'),
        ('shard-override', shard_run, '          export RORTHOX_ACCEL=tcg\n' + shard_run, 'must not override'),
        ('shard-inline-override', shard_run, '          RORTHOX_ACCEL=tcg ' + shard_run.strip() + '\n' + shard_run, 'must not override'),
        ('tcg-missing', '      - name: Test TCG-only boot cases', '      - name: Test TCG cases', 'TCG-only'),
        ('tcg-accel', '      - name: Test TCG-only boot cases\n        if: ${{ !cancelled() && matrix.shard == 1 && steps.image-id.outcome == \'success\' }}\n        env:\n          RORTHOX_REQUIRE_ISO: \'1\'\n          RORTHOX_ACCEL: tcg',
         '      - name: Test TCG-only boot cases\n        if: ${{ !cancelled() && matrix.shard == 1 && steps.image-id.outcome == \'success\' }}\n        env:\n          RORTHOX_REQUIRE_ISO: \'1\'\n          RORTHOX_ACCEL: auto', 'TCG-only'),
        ('host-duplicated', '      - name: Verify and test',
         '      - name: Enable the host KVM device if present\n        run: echo extra\n      - name: Verify and test', 'KVM device'),
        ('host-diagnostics', '          bash ci/kvm-diagnostics.sh host "$before" "$after"',
         '          echo no-diagnostics', 'KVM host setup'),
        ('host-marker-forced', '            failed=\n', '            failed=forced\n', 'KVM host setup'),
        ('sudo-moved-step', '      - name: Verify and test',
         '      - name: Extra\n        run: sudo id\n      - name: Verify and test', 'KVM sudo'),
        ('tcg-missing-image-guard', " && steps.image-id.outcome == 'success'",
         '', 'TCG-only'),
        ('tcg-results-location', 'build/tests/runs/shard-1/tcg-only',
         'build/tests/runs/tcg-only', 'TCG-only'),
        ('binary-tag', '          SHARD: ${{ matrix.shard }}', '          SHARD: ${{ matrix.shard }}\n          T: !!binary YWJj', 'YAML tags'),
        ('value-jobs-env', 'permissions: {}', 'permissions: {}\n# jobs:\nenv:\n  T: ${{ github.token }}', 'credentials'),
        ('escaped-secret-hex', '          SHARD: ${{ matrix.shard }}',
         '          SHARD: ${{ matrix.shard }}\n          T: "${{ se\\x63rets.GITHUB_TOKEN }}"', 'credentials'),
        ('escaped-secret-unicode', '          SHARD: ${{ matrix.shard }}',
         '          SHARD: ${{ matrix.shard }}\n          T: "${{ se\\u0063rets.GITHUB_TOKEN }}"', 'credentials'),
        ('escaped-expression', '      - name: Verify and test',
         '      - run: "echo $\\x7b{ github.event.inputs.arg }"\n      - name: Verify and test', 'expression'),
        ('fallback-escaped-scalar', '          SHARD: ${{ matrix.shard }}',
         '          SHARD: ${{ matrix.shard }}\n          T: "\\x24{{ se\\x63rets.GITHUB_TOKEN }}"', 'credentials'),
        ('sudo-step-shell', '      - name: Verify and test',
         '      - name: Extra\n        shell: "su\\x64o bash -e {0}"\n        run: echo ok\n      - name: Verify and test', 'KVM sudo'),
        ('sudo-job-shell', '  build:\n',
         '  build:\n    defaults:\n      run:\n        shell: "su\\x64o bash -e {0}"\n', 'KVM sudo'),
        ('sudo-workflow-shell', 'permissions: {}',
         'permissions: {}\ndefaults:\n  run:\n    shell: sudo bash -e {0}', 'KVM sudo'),
        ('marker-step-env', '          RORTHOX_ACCEL: auto\n',
         '          RORTHOX_ACCEL: auto\n          RORTHOX_KVM_SETUP_FAILED: fake\n', 'setup marker'),
        ('marker-job-env', '  test:\n',
         '  test:\n    env:\n      RORTHOX_KVM_SETUP_FAILED: fake\n', 'setup marker'),
        ('marker-run', shard_run, '          export RORTHOX_KVM_SETUP_FAILED=fake\n' + shard_run, 'setup marker'),
        ('marker-host-unconditional', '            if [ -n "$failed" ]; then\n',
         '            echo "RORTHOX_KVM_SETUP_FAILED=fake" >> "$GITHUB_ENV"\n            if [ -n "$failed" ]; then\n', 'KVM host setup'),
        ('shard-unset', shard_run, '          unset RORTHOX_ACCEL\n' + shard_run, 'must not override'),
        ('shard-env-unset', shard_run, '          env -u RORTHOX_ACCEL ' + shard_run.strip() + '\n' + shard_run, 'must not override'),
        ('shard-escaped-shell-override', '      - name: Verify and test',
         '      - name: Extra\n        shell: "env RORTHOX\\x5fACCEL=tcg bash -e {0}"\n        run: echo ok\n      - name: Verify and test', 'must not override'),
        ('marker-escaped-shell', '      - name: Verify and test',
         '      - name: Extra\n        shell: "env RORTHOX\\x5fKVM_SETUP_FAILED=x bash -e {0}"\n        run: echo ok\n      - name: Verify and test', 'setup marker'),
        ('override-decoded-run', '      - name: Verify and test',
         '      - run: "echo RORTHOX\\x5fACCEL=tcg"\n      - name: Verify and test', 'must not override'),
        ('marker-decoded-run', '      - name: Verify and test',
         '      - run: "echo RORTHOX\\x5fKVM_SETUP_FAILED=x"\n      - name: Verify and test', 'setup marker'),
        ('override-decoded-env', '          RORTHOX_ACCEL: auto\n',
         '          RORTHOX_ACCEL: auto\n          T: "RORTHOX\\x5fACCEL=tcg"\n', 'must not override'),
        ('marker-decoded-env', '          RORTHOX_ACCEL: auto\n',
         '          RORTHOX_ACCEL: auto\n          T: "RORTHOX\\x5fKVM_SETUP_FAILED=x"\n', 'setup marker'),
        ('upload-root', '            build/tests/runs/shard-${{ matrix.shard }}/results.json',
         '            build/tests/runs/shard-${{ matrix.shard }}/results.json\n            build/tests/runs/sibling/results.json', 'upload paths'),
    )
    for tag, old, new, why in cases:
        assert text.count(old) == 1 or tag in ('shard-timeout',), tag
        variant = text.replace(old, new, 1)
        print('checking', tag, flush=True)
        workflow.write_text(variant)
        for path in (None, no_yaml):
            env = os.environ.copy()
            env.pop('GITHUB_ACTIONS', None)
            if path is not None: env['PYTHONPATH'] = str(path)
            p = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root, env=env,
                               text=True, capture_output=True)
            assert p.returncode and (why in p.stderr or (why == 'KVM host setup' and 'KVM sudo' in p.stderr)
                                     or path is not None and 'requires YAML parser' in p.stderr), (tag, p.stderr)
    env = os.environ.copy()
    env['GITHUB_ACTIONS'] = 'true'
    env['PYTHONPATH'] = str(no_yaml)
    workflow.write_text(text)
    missing = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                             capture_output=True, env=env)
    assert missing.returncode != 0 and b'PyYAML is required' in missing.stderr
    # Multi-path upload-artifact roots files at the common parent of every
    # search path, including paths for files that are absent on this shard.
    upload = text.split('      - name: Upload shard results\n', 1)[1].split('  image-prep:', 1)[0]
    paths = upload.split('          path: |\n', 1)[1].split('          if-no-files-found:', 1)[0].splitlines()
    patterns = [line.strip() for line in paths if line.strip()]
    results = temp / 'results'
    for i in range(1, 5):
        shard = temp / f'upload-{i}'
        shard_report = shard / f'build/tests/runs/shard-{i}/results.json'
        shard_report.parent.mkdir(parents=True)
        shard_report.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'RORTHOX_ACCEL=auto: no KVM', 'tests': [{'result': 'passed'}]}))
        if i == 1:
            tcg_report = shard / 'build/tests/runs/shard-1/tcg-only/results.json'
            tcg_report.parent.mkdir(parents=True)
            tcg_report.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'RORTHOX_ACCEL=tcg',
                                              'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
        search_paths = [shard / p.replace('${{ matrix.shard }}', str(i)) for p in patterns]
        root_path = pathlib.Path(os.path.commonpath([str(src if p.endswith('/') else src.parent)
                                                    for p, src in zip(patterns, search_paths)]))
        for source in (shard_report, tcg_report) if i == 1 else (shard_report,):
            target = results / f'results-{i}' / source.relative_to(root_path)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    aggregate = root / 'scripts/ci-result.py'
    def check(build, shards, success):
        proc = subprocess.run(['python3', '-B', str(aggregate), str(results), build, shards],
                              capture_output=True, text=True)
        assert (proc.returncode == 0) == success, proc.stdout + proc.stderr
    extra = results / 'results-1/tcg-only/results.json'
    assert extra.is_file(), 'uploaded TCG-only report not found at aggregate path'
    check('success', 'success', True)
    print('PASS upload-artifact layout and all five reports')
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'forced',
                                 'tests': [{'name': 'scripts/tests/not-required.sh', 'result': 'passed'}]}))
    check('success', 'success', False)
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'RORTHOX_ACCEL=tcg',
                                 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    summary = subprocess.run(['python3', '-B', str(aggregate), str(results),
                              'success', 'success'], capture_output=True, text=True).stdout
    assert 'Overall: success.' in summary and '| TCG-only | reported | tcg |' in summary
    assert all(f'| test {i} | reported | tcg |' in summary for i in range(1, 5))
    assert '::notice title=Test shard 1 accelerator::tcg:' in summary
    print('PASS aggregate: four shards and TCG-only reported; Overall: success')
    check('failure', 'skipped', False)
    report = results / 'results-3/results.json'
    report.write_text(json.dumps({'accelerator': 'kvm', 'accelerator_reason': 'RORTHOX_ACCEL=auto: KVM works', 'tests': [{'result': 'failed'}]}))
    check('success', 'success', False)
    assert 'failed' in subprocess.run(['python3', '-B', str(aggregate), str(results),
                                       'success', 'success'], capture_output=True, text=True).stdout
    report.write_text('{not json')
    check('success', 'success', False)
    report.write_text(json.dumps({'accelerator': 'kvm', 'accelerator_reason': 'KVM', 'tests': [{}]}))
    check('success', 'success', False)
    report.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'RORTHOX_ACCEL=auto: no KVM', 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    (results / 'results-2/results.json').unlink()
    check('success', 'success', False)
    (results / 'results-2/results.json').write_text(json.dumps({'accelerator': 'tcg',
        'accelerator_reason': 'no KVM', 'tests': [{'result': 'passed'}]}))
    extra.unlink()
    check('success', 'success', False)
    print('PASS aggregate: missing TCG-only report gives Overall: failure')
    extra.write_text(json.dumps({'accelerator': 'kvm', 'accelerator_reason': 'invalid',
                                 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    check('success', 'success', False)
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'reason',
                                 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'skipped'}]}))
    check('success', 'success', False)
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'a' * 1499 + '%\n',
                                 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    p = subprocess.run(['python3', '-B', str(aggregate), str(results), 'success', 'success'],
                       capture_output=True, text=True)
    assert p.returncode == 0 and 'a' * 1499 + '%25' in p.stdout
    assert '%0A' not in p.stdout.split('::notice title=TCG-only test accelerator::', 1)[1].splitlines()[0]
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'x%\r\n::error::injected',
                                 'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    p = subprocess.run(['python3', '-B', str(aggregate), str(results), 'success', 'success'],
                       capture_output=True, text=True)
    assert p.returncode == 0 and 'x%25%0D%0A::error::injected' in p.stdout and '\n::error::injected' not in p.stdout
    print('PASS aggregate requires every job and report')
(out / 'result.txt').write_text('workflow: all cases passed\n')
PY
