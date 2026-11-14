#!/usr/bin/env python3
"""Check the security and required-job contract of the build workflow (stdlib only)."""
import pathlib
import re
import sys


class Invalid(Exception):
    pass


def require(ok, message):
    if not ok:
        raise Invalid(message)


def lint(text):
    require(re.search(r'^permissions: \{\}$', text, re.M), 'top-level permissions must be empty')
    require(not re.search(r'^\s*(workflow_run|pull_request_target):', text, re.M),
            'untrusted trigger')
    require(re.search(r'^  push:\s*\n    branches: \[main, features\]', text, re.M)
            and re.search(r'^  pull_request:', text, re.M), 'missing push or PR trigger')
    require(re.search(r'^  cancel-in-progress: .*github.ref != .refs/heads/main.', text, re.M)
            and re.search(r'^  group: .*github.sha', text, re.M), 'main run must never be superseded')
    for action, release in [('checkout', 'v6'),
                            ('download-artifact', 'v6'), ('upload-artifact', 'v6')]:
        require(re.search(r'^\s*# actions/' + action + ' ' + release + r'\b', text, re.M),
                f'missing release comment: {action}')
    uses = re.findall(r'^\s*(?:-\s*)?uses: (\S+)', text, re.M)
    require(bool(uses) and all(re.fullmatch(r'actions/(checkout|cache|upload-artifact|download-artifact)@[a-f0-9]{40}', a)
                                for a in uses), 'actions must be first-party and pinned to 40-hex SHA')
    require('persist-credentials: false' in text and
            text.count('persist-credentials: false') == uses.count(next(a for a in uses if '/checkout@' in a)),
            'checkout must not persist credentials')
    require(not re.search(r'^\s*(run-id|github-token):', text, re.M),
            'artifact downloads must be same-run')
    parts = re.split(r'^  (build|test|image-prep|image|result|kvm):\s*$', text, flags=re.M)
    require(len(parts) == 13 and parts[1::2] ==
            ['build', 'test', 'image-prep', 'image', 'result', 'kvm'],
            'expected six jobs')
    jobs = dict(zip(parts[1::2], parts[2::2]))
    gate = "github.event_name == 'push' && github.repository == 'oriapp/RorthOX' && (github.ref == 'refs/heads/features' || github.ref == 'refs/heads/main')"
    require(re.findall(r'^    if: (.+)$', jobs['image'], re.M) == [gate],
            'publication must have the exact trusted-push job gate')
    for name, body in jobs.items():
        grants = re.findall(r'^    permissions:\n((?:      [^\n]+\n?)+)', body, re.M)
        expected = ['      contents: read'] + (['      packages: write'] if name == 'image' else [])
        require(len(grants) == 1 and grants[0].splitlines() == expected,
                f'{name}: unexpected permissions')
        if name == 'kvm':
            require(re.findall(r'^    continue-on-error: (.*)$', body, re.M) == ['true'],
                    'KVM job must remain advisory')
        if name == 'image':
            require(re.findall(r'^    continue-on-error: (.*)$', body, re.M) == ['true'],
                    'publication is not required for the image handoff')
        if name in ('image-prep', 'image', 'kvm'):
            job_limits = re.findall(r'^    timeout-minutes: (\d+)', body, re.M)
            require(len(job_limits) == 1, f'{name}: missing job timeout')
            job_limit = int(job_limits[0])
            step_blocks = re.split(r'^      - ', body, flags=re.M)[1:]
            require(bool(step_blocks), f'{name}: missing steps')
            total = 0
            for number, step in enumerate(step_blocks, 1):
                limits = re.findall(r'^        timeout-minutes: (\d+)\s*$', step, re.M)
                require(len(limits) == 1 and 0 < int(limits[0]) < job_limit,
                        f'{name}: step {number} must time out before the job')
                total += int(limits[0])
            require(total < job_limit, f'{name}: step limits exceed job timeout')
    # Parse real YAML when available; the fail-closed syntax restrictions below also
    # protect hosts without the optional YAML package.
    try:
        import yaml
    except ImportError:
        yaml = None
    if yaml is None and __import__('os').environ.get('GITHUB_ACTIONS') == 'true':
        raise Invalid('PyYAML is required on Actions')
    if yaml is not None:
        try:
            document = yaml.safe_load(text)
        except yaml.YAMLError as error:
            raise Invalid(f'invalid workflow YAML: {error}') from error
        require(isinstance(document, dict) and isinstance(document.get('jobs'), dict),
                'invalid workflow jobs')
        parsed = document['jobs']
        require(set(parsed) == {'build', 'test', 'image-prep', 'image', 'result', 'kvm'},
                'unexpected workflow job')
        for name, job in parsed.items():
            require(isinstance(job, dict) and isinstance(job.get('steps'), list),
                    'invalid workflow steps')
            if name == 'kvm':
                require(job.get('continue-on-error') is True and
                        all('continue-on-error' not in step for step in job['steps']
                            if isinstance(step, dict)), 'only the KVM job may be advisory')
            if name == 'result':
                require('continue-on-error' not in job and
                        all('continue-on-error' not in step for step in job['steps']
                            if isinstance(step, dict) and step.get('name') == 'Check every job'),
                        'aggregate must not mask failure')
            if name in ('build', 'test', 'image-prep'):
                require('continue-on-error' not in job, 'required job must not mask failure')
                require(all('continue-on-error' not in step for step in job['steps'] if isinstance(step, dict)),
                        'required step must not mask failure')
            for step in job['steps']:
                require(isinstance(step, dict), 'invalid workflow step')
                require('${{' not in str(step.get('run', '')), 'expression inside run')
        require(parsed['build'].get('needs') == 'image-prep' and
                parsed['test'].get('needs') == ['image-prep', 'build'] and
                parsed['image'].get('needs') == ['image-prep', 'build', 'test'] and
                parsed['result'].get('needs') == ['build', 'test'] and
                parsed['kvm'].get('needs') == ['image-prep', 'build'] and
                parsed['test'].get('strategy', {}).get('fail-fast') is False,
                'invalid job dependencies or shard strategy')
    # Only block-style, unaliased YAML keys are supported by the fallback lint.
    require(not re.search(r'^\s*(?:-\s*)?["\'][^\r\n]*?["\']\s*:', text, re.M),
            'quoted YAML mapping keys are not supported')
    require(not re.search(r'^\s*(?:-\s*)?[A-Za-z_][A-Za-z_0-9-]*\s+:', text, re.M),
            'spaced YAML key is not supported')
    require(not re.search(r'^\s*(?:-\s*)?[?!]', text, re.M),
            'explicit keys and YAML tags are not supported')
    require(not re.search(r'^\s*-\s*\{', text, re.M), 'flow-mapping step')
    require(not re.search(r'(?:^\s*-\s*|:\s*)[&*][A-Za-z0-9_-]+\b', text, re.M),
            'YAML anchors and aliases are not supported')
    # Expressions in shell commands are interpreted before shell quoting; use env: instead.
    lines = text.splitlines()
    block_indent = None
    for line in lines:
        indent = len(line) - len(line.lstrip())
        if line.strip() and block_indent is not None and indent <= block_indent:
            block_indent = None
        if block_indent is None:
            # Tags change scalar types, including !!binary values hidden in flow mappings.
            require(not re.search(r'(?:^|[\s\[{,:])!(?:!|<|[A-Za-z_])', line),
                    'YAML tags are not supported')
            if yaml is None:
                # Refuse escapes in any double-quoted YAML scalar, not just expressions.
                require(not re.search(r'"(?:[^"\\]|\\.)*\\', line),
                        'escaped scalar requires YAML parser')
            if re.match(r'^\s*(?:-\s+)?run:\s*[|>][+\-0-9]*\s*$', line):
                block_indent = indent
    for i, line in enumerate(lines):
        match = re.match(r'^(\s*(?:-\s+)?)run:\s*(.*)$', line)
        if match is None:
            continue
        indent = len(match[1])
        require('${{' not in match[2], 'expression inside run')
        # YAML can decode \\x7b, \\u007b and escaped newlines into ${{.
        # Without PyYAML, refuse escapes in any double-quoted run value.
        if yaml is None:
            require('\\' not in match[2] and not match[2].lstrip().startswith('!'),
                    'escaped or tagged run requires YAML parser')
        for command in lines[i + 1:]:
            if command.strip() and len(command) - len(command.lstrip()) <= indent:
                break
            require('${{' not in command, 'expression inside run')
            if yaml is None:
                require('\\' not in command, 'escaped run requires YAML parser')
    require(not any(re.search(r'^    continue-on-error:', jobs[name], re.M)
                    for name in ('build', 'test', 'image-prep', 'result')),
            'only publication and KVM may mask a job failure')
    require('    continue-on-error:' not in jobs['build'] and
            '    continue-on-error:' not in jobs['test'], 'required job must not mask failure')
    require('./ci/run.sh make all iso' in jobs['build'], 'production build must run in image')
    require('needs: image-prep' in jobs['build'] and
            'needs: [image-prep, build]' in jobs['test'] and
            re.search(r'^      fail-fast: false[ \t]*$', jobs['test'], re.M) and 'shard: [1, 2, 3, 4]' in jobs['test'],
            'required jobs must consume the image and four independent shards')
    require(not re.search(r'^    continue-on-error:', jobs['result'], re.M) and
            not re.search(r'^        continue-on-error:', jobs['result'].split('      - name: Check every job', 1)[-1], re.M),
            'aggregate must not mask failure')
    require(re.search(r'^    needs: \[build, test\]$', jobs['result'], re.M) and re.search(r'^    if: always\(\)$', jobs['result'], re.M)
            and 'scripts/ci-result.py' in jobs['result'], 'aggregate must run after failure')
    require('shell: bash # Fail the step when prepare fails' in jobs['image-prep'],
            'piped steps must use bash pipefail')
    for name in ('build', 'test'):
        body = jobs[name]
        require('ci/image-job.sh receive' in body and 'ci/image-job.sh fetch' in body and
                'IMAGE_MODE: ${{ needs.image-prep.outputs.mode }}' in body and
                'RORTHOX_IMAGE_ID: ${{ steps.image-id.outputs.id }}' in body and
                'RORTHOX_ACCEL: tcg' in body and './ci/run.sh ' in body,
                f'{name}: unverified image or host workload')
        require('sudo apt-' not in body and 'actions/cache@' not in body and
                './configure.sh' not in body, f'{name}: legacy toolchain path')
    kvm = jobs['kvm']
    kvm_steps = {name: body for name, body in
                 re.findall(r'^      - name: ([^\n]+)\n(.*?)(?=^      - |\Z)', kvm, re.M | re.S)}
    image_step = kvm_steps.get('Verify the image for this revision', '')
    test_step = kvm_steps.get('Test with KVM', '')
    host_step = kvm_steps.get('Enable the host KVM device if present', '')
    def step_field(step, field, value):
        return bool(re.search(r'^          ' + re.escape(field) + r': ' + re.escape(value) + r'$', step, re.M))

    require(re.search(r'^    needs: \[image-prep, build\]$', kvm, re.M) and
            re.search(r"^    if: github.event_name == 'push' && github.repository == 'oriapp/RorthOX'$", kvm, re.M) and
            'ci/image-job.sh receive' in image_step and 'ci/image-job.sh fetch' in image_step and
            step_field(image_step, 'IMAGE_MODE', '${{ needs.image-prep.outputs.mode }}') and
            step_field(test_step, 'RORTHOX_IMAGE_ID', '${{ steps.image-id.outputs.id }}') and
            step_field(test_step, 'MANIFEST_SHA256', '${{ needs.build.outputs.manifest }}') and
            step_field(test_step, 'RORTHOX_ACCEL', 'kvm') and
            step_field(test_step, 'RORTHOX_TEST_TIMEOUT', "'300'") and
            step_field(test_step, 'RORTHOX_ACCEL_PROBE_TIMEOUT', "'30'") and
            bool(re.search(r'^          ./ci/run\.sh python3 -B scripts/ci-artifact\.py extract build/ci/production\.tar\.gz "\$MANIFEST_SHA256"$', test_step, re.M)) and
            bool(re.search(r'^          ./ci/run\.sh ./scripts/run-tests\.sh -j 2 --results build/tests/runs/kvm$', test_step, re.M)) and
            'name: kvm-results' in kvm and
            re.search(r'^      - name: Upload KVM results\n        timeout-minutes: \d+\n        if: always\(\)$', kvm, re.M),
            'KVM job must use the same verified image and build artifact')
    sudo_lines = [line.strip() for line in kvm.splitlines() if re.search(r'\bsudo\b', line)]
    allowed_sudo = [
        "echo 'KERNEL==\"kvm\", GROUP=\"kvm\", MODE=\"0666\", OPTIONS+=\"static_node=kvm\"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules",
        'sudo udevadm control --reload-rules',
        'sudo udevadm trigger --name-match=kvm',
        'sudo udevadm settle',
    ]
    require(sudo_lines == allowed_sudo and
            all(('            ' + line) in host_step.splitlines() for line in allowed_sudo),
            'KVM sudo is confined to device setup')
    require(not re.search(r'\bdocker\b', kvm, re.I) and
            not re.search(r'^    (?:container|services):', kvm, re.M),
            'KVM job must not run host Docker or use job containers')
    if yaml is not None:
        require('container' not in parsed['kvm'] and 'services' not in parsed['kvm'],
                'KVM job must not use a container or services')
        parsed_steps = {step.get('name'): step for step in parsed['kvm']['steps']}
        parsed_image = parsed_steps.get('Verify the image for this revision', {})
        parsed_test = parsed_steps.get('Test with KVM', {})
        require(isinstance(parsed_image, dict) and isinstance(parsed_test, dict) and
                parsed_image.get('env', {}).get('IMAGE_MODE') == '${{ needs.image-prep.outputs.mode }}' and
                parsed_test.get('env', {}).get('RORTHOX_IMAGE_ID') == '${{ steps.image-id.outputs.id }}' and
                parsed_test.get('env', {}).get('MANIFEST_SHA256') == '${{ needs.build.outputs.manifest }}' and
                parsed_test.get('env', {}).get('RORTHOX_TEST_TIMEOUT') == '300' and
                parsed_test.get('env', {}).get('RORTHOX_ACCEL') == 'kvm' and
                './ci/run.sh python3 -B scripts/ci-artifact.py extract build/ci/production.tar.gz "$MANIFEST_SHA256"' in parsed_test.get('run', ''),
                'KVM parsed step bindings differ')
    for step in parsed['kvm']['steps'] if yaml is not None else kvm_steps.values():
        run = step.get('run', '') if yaml is not None else (step.split('        run:', 1)[1] if '        run:' in step else '')
        require(not re.search(r'\bRORTHOX_ACCEL\b', run),
                'KVM run must not override its accelerator')
        for line in run.splitlines():
            if re.search(r'(?<![\w./])(?:\./)?scripts/', line):
                require(bool(re.search(r'\./ci/run\.sh\s+[^\n]*scripts/', line)),
                        'KVM scripts must run inside the image')
    require('    continue-on-error:' not in jobs['image-prep'],
            'image preparation must be required')
    # Context references must not put credentials in the workflow or read-only jobs.
    # github.ref and other non-secret fields are allowed; whole-context exports are not.
    scope = re.split(r'^jobs:[ \t]*$', text, maxsplit=1, flags=re.M)[0]
    outside = scope + ''.join(body for name, body in jobs.items() if name != 'image')
    credential = re.compile(r'\bsecrets\b|\bgithub\b'
                            r'(?!\s*\.\s*(?:event_name|repository|ref|sha|workflow)\b)', re.I)
    # YAML double-quoted scalars may decode escapes into credential expressions.
    # Check parsed keys as well as values (including sequences and nested mappings).
    def contains_credential(value):
        if isinstance(value, str):
            return bool(credential.search(value))
        if isinstance(value, dict):
            return any(contains_credential(k) or contains_credential(v) for k, v in value.items())
        if isinstance(value, list):
            return any(contains_credential(item) for item in value)
        return False

    if yaml is None:
        # No parser means decoded YAML escapes cannot be inspected safely.
        require(not re.search(r'\$\{\{[^}]*\\', outside),
                'escaped expression requires YAML parser')
    require(not credential.search(outside) and
            (yaml is None or not contains_credential({**{k: v for k, v in document.items() if k != 'jobs'},
                                                      'jobs': {k: v for k, v in parsed.items() if k != 'image'}})),
            'credentials outside publication')
    require('retention-days: 1' in jobs['image-prep'], 'candidate archive retention too long')
    require(text.count('secrets.') == 1 and 'GHCR_TOKEN: ${{ secrets.GITHUB_TOKEN }}' in jobs['image'] and
            not any('secrets.' in body for name, body in jobs.items() if name != 'image'),
            'credentials must be confined to publication')
    require('      - name: Log out of registry\n        timeout-minutes: 2\n        if: always()' in jobs['image'] and
            jobs['image'].index('ci/image-job.sh receive') < jobs['image'].index('ci/image-job.sh publish') <
            jobs['image'].index('docker logout ghcr.io') and
            'docker run' not in jobs['image'][jobs['image'].index('docker logout ghcr.io'):],
            'publication must validate before login and always log out')
    require(re.search(r'^    needs: \[image-prep, build, test\]$', jobs['image'], re.M) and
            'cancel-in-progress: false' in jobs['image'] and
            'group: image-${{ needs.image-prep.outputs.key }}' in jobs['image'],
            'publication must serialize on the input hash')
    require('ci/image-job.sh prepare' in jobs['image-prep'] and
            'ci/image-job.sh publish' in jobs['image'] and
            'ci/image-job.sh receive' in jobs['build'] and
            'ci/image-job.sh fetch' in jobs['test'], 'missing verified image handoff')
    require('./ci/run.sh python3 -B scripts/ci-artifact.py extract' in jobs['test']
            and './ci/run.sh python3 -B scripts/ci-artifact.py create' in jobs['build'], 'build handoff must be verified')
    require('RORTHOX_REQUIRE_ISO:' in jobs['test'] and './ci/run.sh ./scripts/run-tests.sh --shard' in jobs['test'],
            'required sharded test invocation missing')


if __name__ == '__main__':
    try:
        lint(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.github/workflows/build.yml').read_text())
    except (Invalid, OSError, StopIteration) as e:
        print(f'workflow lint: {e}', file=sys.stderr)
        sys.exit(1)
    print('workflow lint: OK')
