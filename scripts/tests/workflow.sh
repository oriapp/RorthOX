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
    nightly = (root / '.github/workflows/nightly.yml').read_text()
    nightly_path = temp / 'nightly.yml'
    def run_nightly(source, valid):
        nightly_path.write_text(source)
        env = os.environ.copy()
        env['GITHUB_ACTIONS'] = 'true'
        p = subprocess.run(['python3', '-B', str(lint), str(nightly_path)], cwd=root,
                           text=True, capture_output=True, env=env)
        assert (p.returncode == 0) == valid, p.stderr
    run_nightly(nightly, True)
    nightly_cases = (
        ('publication', '  aggregate:\n', '  publication:\n    runs-on: ubuntu-24.04\n  aggregate:\n'),
        ('permission', 'permissions: {}', 'permissions:\n  contents: write'),
        ('job-permission', '  flake:\n    needs: [image-prep, build]\n    runs-on: ubuntu-24.04\n    timeout-minutes: 240\n    permissions:\n      contents: read', '  flake:\n    needs: [image-prep, build]\n    runs-on: ubuntu-24.04\n    timeout-minutes: 240\n    permissions:\n      packages: write'),
        ('action-pin', 'actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803', 'actions/checkout@v6'),
        ('credential', '          RORTHOX_ACCEL: tcg', '          TOKEN: ${{ github.token }}\n          RORTHOX_ACCEL: tcg'),
        ('dispatch-in-run', '          ./ci/run.sh make all iso', '          echo "${{ github.event.inputs.arg }}"\n          ./ci/run.sh make all iso'),
        ('trigger', "      - 'scripts/timing-refresh.py'", "      - 'README.md'"),
        ('fallback', '          RORTHOX_ACCEL: ${{ matrix.accel }}', '          RORTHOX_ACCEL: auto'),
        ('no-kvm-fail', "        if: matrix.accel == 'kvm'", "        if: matrix.accel == 'tcg'"),
        ('not-single-cpu', '          export RORTHOX_CPUSET=$cpu', '          export CPU=$cpu'),
        ('flake-accel-override', '          export RORTHOX_CPUSET=$cpu',
         '          export RORTHOX_CPUSET=$cpu\n          export RORTHOX_ACCEL=auto'),
        ('not-serial', '-j 1 --results', '-j 2 --results'),
        ('not-three', '          for i in 1 2 3; do', '          for i in 1 2; do'),
        ('retry', '  flake:\n', '  flake:\n    # retries hide failures\n'),
        ('aggregate-skipped', '    if: always()\n    runs-on: ubuntu-24.04\n    timeout-minutes: 10', '    if: success()\n    runs-on: ubuntu-24.04\n    timeout-minutes: 10'),
        ('publish', 'ci/image-job.sh prepare', 'ci/image-job.sh publish'),
        ('extra-base', '          ./ci/run.sh make all iso', '          ./ci/run.sh make all iso --fixture-base abc'),
    )
    for tag, old, new in nightly_cases:
        assert old in nightly, tag
        print('checking nightly', tag, flush=True)
        run_nightly(nightly.replace(old, new, 1), False)
    # Decoded YAML, inherited settings and host commands must obey the build lint policy.
    extra_nightly = (
        ('workflow-token', 'permissions: {}', 'permissions: {}\nenv:\n  T: ${{ github.token }}'),
        ('job-token', '  flake:\n', '  flake:\n    env:\n      T: ${{ secrets.GITHUB_TOKEN }}\n'),
        ('indexed-token', '          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}',
         '          T: ${{ github["token"] }}\n          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}'),
        ('whole-context', '          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}',
         '          T: ${{ toJSON(github) }}\n          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}'),
        ('decoded-token', '          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}',
         '          T: "${{ \\x67ithub.token }}"\n          MANIFEST_SHA256: ${{ needs.build.outputs.manifest }}'),
        ('workflow-shell', 'permissions: {}', 'permissions: {}\ndefaults:\n  run:\n    shell: env RORTHOX_ACCEL=auto bash -e {0}'),
        ('job-shell', '  flake:\n', '  flake:\n    defaults:\n      run:\n        shell: env RORTHOX_KVM_SETUP_FAILED=1 bash -e {0}\n'),
        ('job-marker', '  flake:\n', '  flake:\n    env:\n      RORTHOX_KVM_SETUP_FAILED: 1\n'),
        ('download-run-id', '          name: nightly-production', '          name: nightly-production\n          run-id: 12'),
        ('sudo-elsewhere', '          mkdir -p build/ci/nightly/flake',
         '          sudo chmod 666 /var/run/docker.sock\n          mkdir -p build/ci/nightly/flake'),
        ('docker-elsewhere', '          mkdir -p build/ci/nightly/flake',
         '          docker run --privileged image true\n          mkdir -p build/ci/nightly/flake'),
        ('flake-on-cancel', '  flake:\n    needs: [image-prep, build]',
         '  flake:\n    if: always()\n    needs: [image-prep, build]'),
        ('missing-aggregate-check', '      - name: Check parity and flake hunt\n        if: ${{ !cancelled() }}',
         '      - name: Check parity and flake hunt'),
    )
    for tag, old, new in extra_nightly:
        assert old in nightly, tag
        run_nightly(nightly.replace(old, new, 1), False)
    print('PASS nightly workflow and security fixtures', flush=True)
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
        ('missing-shard-image-dependency', '    needs: [image-prep, build, select]', '    needs: build'),
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
    # A YAML comment cannot pin the decoded selector token environment.
    token = "          GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}"
    assert token in text
    run(text.replace(token, token.replace('          ', '          # ', 1) +
                     '\n          GH_TOKEN: "${{ \\x67ithub.token }}"', 1), False)
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
    shard_run = '            ./ci/run.sh ./scripts/run-tests.sh --shard "$SHARD/4" -j 2 --results "build/tests/runs/shard-$SHARD"\n'
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
        ('tcg-hardcoded-caller', " && steps.image-id.outcome == 'success' }}",
         " && steps.image-id.outcome == 'success' && contains(needs.select.outputs.tests, 'scripts/tests/boot-segments.sh') }}", 'TCG-only'),
        ('tcg-empty-subset', 'if [ "${#tcg_only[@]}" -eq 0 ]; then exit 0; fi',
         'echo no-selected-caller', 'TCG-only'),
        ('tcg-accel', "          RORTHOX_REQUIRE_ISO: '1'\n          RORTHOX_ACCEL: tcg",
         "          RORTHOX_REQUIRE_ISO: '1'\n          RORTHOX_ACCEL: kvm", 'TCG-only'),
        ('host-duplicated', '      - name: Verify and test',
         '      - name: Enable the host KVM device if present\n        run: echo extra\n      - name: Verify and test', 'KVM device'),
        ('host-diagnostics', '          bash ci/kvm-diagnostics.sh host "$before" "$after"',
         '          echo no-diagnostics', 'KVM host setup'),
        ('host-marker-forced', '            failed=\n', '            failed=forced\n', 'KVM host setup'),
        ('sudo-moved-step', '      - name: Verify and test',
         '      - name: Extra\n        run: sudo id\n      - name: Verify and test', 'KVM sudo'),
        ('tcg-missing-image-guard', " && steps.image-id.outcome == 'success'",
         '', 'selection must gate'),
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
        ('selector-permission', '  select:\n    runs-on:', '  select:\n    permissions:\n      contents: write\n    runs-on:', 'unexpected permissions'),
        ('selector-credentials', '          fetch-depth: 0', '          fetch-depth: 0\n          token: ${{ secrets.GITHUB_TOKEN }}', 'credentials'),
        ('selector-no-history', '          fetch-depth: 0', '          fetch-depth: 1', 'selector must compare'),
        ('selector-unpinned', '      - name: Choose tests',
         '      - uses: actions/checkout@v6\n      - name: Choose tests', 'actions must be'),
        ('selector-no-output', '--github-output "$GITHUB_OUTPUT"', '--github-output /tmp/out', 'selector command'),
        ('selector-no-gate', "    if: ${{ !cancelled() && needs.image-prep.result == 'success' && needs.build.result == 'success' && needs.select.outputs.mode != 'none' }}", '    if: always()', 'selection must gate'),
        ('selector-no-result', 'SELECT_RESULT: ${{ needs.select.result }}', 'SELECT_RESULT: success', 'selection must gate'),
        ('defaults-accel-workflow', 'permissions: {}', 'permissions: {}\ndefaults:\n  run:\n    shell: env RORTHOX_ACCEL=tcg bash -e {0}', 'defaults must not override'),
        ('defaults-accel-test-escaped', '  test:\n', '  test:\n    defaults:\n      run:\n        shell: "env RORTHOX\\x5fACCEL=tcg bash -e {0}"\n', 'override its accelerator'),
        ('defaults-marker', '  test:\n', '  test:\n    defaults:\n      run:\n        shell: env RORTHOX_KVM_SETUP_FAILED=x bash -e {0}\n', 'setup marker'),
        ('selector-no-actions', '      actions: read', '      actions: write', 'unexpected permissions'),
        ('selector-no-token', "GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}", 'GH_TOKEN: none', 'selector'),
        ('selector-token-pr', "GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}", 'GH_TOKEN: ${{ github.token }}', 'selector'),
        ('selector-fixture-base', '--github-output "$GITHUB_OUTPUT"', '--fixture-base deadbeef --github-output "$GITHUB_OUTPUT"', 'fixture base'),
        ('selector-extra-command', '          python3 -B scripts/select-tests.py --event', '          ./scripts/run-tests.sh\n          python3 -B scripts/select-tests.py --event', 'selector command'),
        ('selector-extra-env', '          REPOSITORY: ${{ github.repository }}', '          REPOSITORY: ${{ github.repository }}\n          EXTRA: harmless', 'selector command'),
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
    moved = '            if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi\n'
    variant = text.replace(moved, '', 1).replace('      - name: Verify and test',
        '      - name: Extra\n        run: |\n' + moved + '      - name: Verify and test', 1)
    print('checking moved-allowed-sudo', flush=True)
    run(variant, False)
    env = os.environ.copy()
    env['GITHUB_ACTIONS'] = 'true'
    env['PYTHONPATH'] = str(no_yaml)
    workflow.write_text(text)
    missing = subprocess.run(['python3', '-B', str(lint), str(workflow)], cwd=root,
                             capture_output=True, env=env)
    assert missing.returncode != 0 and b'PyYAML is required' in missing.stderr
    # The TCG discovery used by the step and aggregate treats a trailing semicolon as a call.
    tcg_tree = temp / 'tcg-fixture'
    (tcg_tree / 'scripts/tests').mkdir(parents=True)
    (tcg_tree / 'scripts/tests/pic.sh').write_text('qemu_require_tcg; echo ok\n')
    found = subprocess.run(['python3', '-B', str(root / 'scripts/tcg_only.py')],
                           cwd=tcg_tree, capture_output=True, text=True, check=True)
    assert found.stdout.splitlines() == ['scripts/tests/pic.sh'], found.stdout
    print('PASS TCG-only discovery: semicolon caller')
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
    def selected(mode, reason, tests, shard_status, expected, select_status='success'):
        env = os.environ.copy()
        env.update(SELECT_RESULT=select_status, SELECT_MODE=mode, SELECT_REASON=reason, SELECT_TESTS=tests)
        p = subprocess.run(['python3', '-B', str(aggregate), str(results), 'success', shard_status],
                           capture_output=True, text=True, env=env)
        assert (p.returncode == 0) == expected, p.stdout + p.stderr
        return p.stdout
    text = selected('none', 'documentation-only change', '', 'skipped', True)
    assert 'no tests selected: documentation-only change' in text
    selected('none', 'documentation-only change', '', 'failure', False)
    selected('none', 'forged reason', '', 'skipped', False)
    selected('unexpected', 'unknown', '', 'skipped', False)
    selected('none', 'documentation-only change', '', 'skipped', False, 'failure')
    report = results / 'results-3/results.json'
    empty = {'accelerator': 'tcg', 'accelerator_reason': 'no KVM', 'tests': []}
    for i in range(1, 5):
        (results / f'results-{i}/results.json').write_text(json.dumps(empty))
    report1 = results / 'results-1/results.json'
    report1.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'no KVM',
                                   'tests': [{'name': 'scripts/smoke.sh', 'result': 'passed'},
                                             {'name': 'scripts/tests/pic.sh', 'result': 'passed'}]}))
    extra.unlink()
    subset = 'scripts/smoke.sh scripts/tests/pic.sh'
    selected('subset', 'one test plus smoke', subset, 'success', True)
    selected('subset', 'one test plus smoke', subset + ' scripts/tests/heap.sh', 'success', False)
    selected('subset', 'one test plus smoke', subset, 'skipped', False)
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'forced',
                                  'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    selected('subset', 'one test plus smoke', subset, 'success', False)
    # Resume full-mode legacy fixtures.
    extra.write_text(json.dumps({'accelerator': 'tcg', 'accelerator_reason': 'RORTHOX_ACCEL=tcg',
                                  'tests': [{'name': 'scripts/tests/boot-segments.sh', 'result': 'passed'}]}))
    for i in range(1, 5):
        (results / f'results-{i}/results.json').write_text(json.dumps({'accelerator': 'tcg',
            'accelerator_reason': 'no KVM', 'tests': [{'result': 'passed'}]}))
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
