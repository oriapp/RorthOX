#!/usr/bin/env python3
"""Check the security and required-job contract of the build workflow (stdlib only)."""
import pathlib
import re
import sys
import textwrap


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
    parts = re.split(r'^  (select|build|test|image-prep|image|result):\s*$', text, flags=re.M)
    require(len(parts) == 13 and parts[1::2] ==
            ['select', 'build', 'test', 'image-prep', 'image', 'result'],
            'expected six jobs')
    jobs = dict(zip(parts[1::2], parts[2::2]))
    gate = "github.event_name == 'push' && github.repository == 'oriapp/RorthOX' && (github.ref == 'refs/heads/features' || github.ref == 'refs/heads/main')"
    require(re.findall(r'^    if: (.+)$', jobs['image'], re.M) == [gate],
            'publication must have the exact trusted-push job gate')
    for name, body in jobs.items():
        grants = re.findall(r'^    permissions:\n((?:      [^\n]+\n?)+)', body, re.M)
        expected = ['      contents: read'] + (['      packages: write'] if name == 'image' else
                                                ['      actions: read'] if name == 'select' else [])
        require(len(grants) == 1 and grants[0].splitlines() == expected,
                f'{name}: unexpected permissions')
        if name == 'image':
            require(re.findall(r'^    continue-on-error: (.*)$', body, re.M) == ['true'],
                    'publication is not required for the image handoff')
        if name in ('image-prep', 'image'):
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
        require(set(parsed) == {'select', 'build', 'test', 'image-prep', 'image', 'result'},
                'unexpected workflow job')
        for name, job in parsed.items():
            require(isinstance(job, dict) and isinstance(job.get('steps'), list),
                    'invalid workflow steps')
            if name == 'result':
                require('continue-on-error' not in job and
                        all('continue-on-error' not in step for step in job['steps']
                            if isinstance(step, dict) and step.get('name') == 'Check every job'),
                        'aggregate must not mask failure')
            if name in ('select', 'build', 'test', 'image-prep'):
                require('continue-on-error' not in job, 'required job must not mask failure')
                require(all('continue-on-error' not in step for step in job['steps'] if isinstance(step, dict)),
                        'required step must not mask failure')
            for step in job['steps']:
                require(isinstance(step, dict), 'invalid workflow step')
                require('${{' not in str(step.get('run', '')), 'expression inside run')
        require(parsed['build'].get('needs') == 'image-prep' and
                parsed['test'].get('needs') == ['image-prep', 'build', 'select'] and
                parsed['image'].get('needs') == ['image-prep', 'build', 'test'] and
                parsed['result'].get('needs') == ['select', 'build', 'test'] and
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
            require(not re.search(r'(?:^|[\s\[{,:])!(?:!|<|[A-Za-z_])',
                                  re.sub(r'\$\{\{.*?\}\}', '', line)),
                    'YAML tags are not supported')
            if yaml is None:
                # Refuse escapes in any double-quoted YAML scalar, not just expressions.
                require('\\' not in line,
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
                    for name in ('select', 'build', 'test', 'image-prep', 'result')),
            'only publication may mask a job failure')
    require('    continue-on-error:' not in jobs['build'] and
            '    continue-on-error:' not in jobs['test'], 'required job must not mask failure')
    require('./ci/run.sh make all iso' in jobs['build'], 'production build must run in image')
    require('needs: image-prep' in jobs['build'] and
            'needs: [image-prep, build, select]' in jobs['test'] and
            re.search(r'^      fail-fast: false[ \t]*$', jobs['test'], re.M) and 'shard: [1, 2, 3, 4]' in jobs['test'],
            'required jobs must consume the image and four independent shards')
    require(not re.search(r'^    continue-on-error:', jobs['result'], re.M) and
            not re.search(r'^        continue-on-error:', jobs['result'].split('      - name: Check every job', 1)[-1], re.M),
            'aggregate must not mask failure')
    require(re.search(r'^    needs: \[select, build, test\]$', jobs['result'], re.M) and re.search(r'^    if: always\(\)$', jobs['result'], re.M)
            and 'scripts/ci-result.py' in jobs['result'], 'aggregate must run after failure')
    require('shell: bash # Fail the step when prepare fails' in jobs['image-prep'],
            'piped steps must use bash pipefail')
    # The fixture-base escape hatch is only for local selector tests.
    require('--fixture-base' not in text, 'fixture base is forbidden in workflows')
    select = jobs['select']
    choose_run = '''if [ "$EVENT" = pull_request ]; then before=$BASE; else before=$BEFORE; fi
python3 -B scripts/select-tests.py --event "$EVENT" --ref "$REF" --base "$before" --head "$HEAD" --repository "$REPOSITORY" --branch "${REF#refs/heads/}" --github-output "$GITHUB_OUTPUT"
'''
    choose_env = {'EVENT', 'REF', 'BEFORE', 'BASE', 'HEAD', 'REPOSITORY', 'GH_TOKEN'}
    if yaml is not None:
        steps = parsed['select']['steps']
        require(len(steps) == 2 and steps[1].get('name') == 'Choose tests' and
                steps[1].get('run') == choose_run and
                set(steps[1].get('env', {})) == choose_env,
                'selector command and environment must be pinned')
    else:
        pinned_step = re.search(r'^      - name: Choose tests\n(.*?)(?=^      - |\Z)', select, re.M | re.S)
        expected = '''        id: choose
        env:
          EVENT: ${{ github.event_name }}
          REF: ${{ github.ref }}
          BEFORE: ${{ github.event.before }}
          BASE: ${{ github.event.pull_request.base.sha }}
          HEAD: ${{ github.sha }}
          REPOSITORY: ${{ github.repository }}
          GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}
        run: |
''' + textwrap.indent(choose_run, '          ')
        require(pinned_step is not None and pinned_step[1].rstrip('\n') == expected.rstrip('\n'),
                'selector command and environment must be pinned')
    require('persist-credentials: false' in select and 'fetch-depth: 0' in select and
            'scripts/select-tests.py' in select and '--github-output "$GITHUB_OUTPUT"' in select and
            '          BASE: ${{ github.event.pull_request.base.sha }}' in select and
            '          BEFORE: ${{ github.event.before }}' in select and
            all(f'{key}: ${{{{ steps.choose.outputs.{key} }}}}' in select for key in ('mode', 'reason', 'tests', 'base', 'base_source')) and
            "GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}" in select and '--repository "$REPOSITORY"' in select and
            '--branch "${REF#refs/heads/}"' in select and
            not re.search(r'\b(?:sudo|docker)\b', select) and 'secrets.' not in select,
            'selector must compare revisions without credentials or host privileges')
    require("    if: ${{ !cancelled() && needs.image-prep.result == 'success' && needs.build.result == 'success' && needs.select.outputs.mode != 'none' }}" in jobs['test'] and
            'TEST_MODE: ${{ needs.select.outputs.mode }}' in jobs['test'] and
            'SELECTED_TESTS: ${{ needs.select.outputs.tests }}' in jobs['test'] and
            '"${selected[@]}"' in jobs['test'] and
            'if [ "$TEST_MODE" = subset ]; then' in jobs['test'] and
            "steps.image-id.outcome == 'success'" in jobs['test'] and
            "if: needs.build.result == 'success' && needs.select.outputs.mode != 'none'" in jobs['result'] and
            all(f'SELECT_{key}: ${{{{ needs.select.outputs.{value} }}}}' in jobs['result']
                for key, value in (('MODE', 'mode'), ('REASON', 'reason'), ('TESTS', 'tests'))) and
            'SELECT_RESULT: ${{ needs.select.result }}' in jobs['result'],
            'selection must gate shards and aggregate')
    if yaml is not None:
        require(parsed['test'].get('if') == "${{ !cancelled() && needs.image-prep.result == 'success' && needs.build.result == 'success' && needs.select.outputs.mode != 'none' }}" and
                parsed['select']['permissions'] == {'contents': 'read', 'actions': 'read'} and
                parsed['select'].get('needs') is None and
                parsed['select']['steps'][0].get('with', {}).get('persist-credentials') is False and
                parsed['select']['steps'][0].get('with', {}).get('fetch-depth') == 0 and
                parsed['result']['steps'][-1].get('env', {}).get('SELECT_RESULT') == '${{ needs.select.result }}',
                'invalid selection job or aggregate outputs')
    for name in ('build', 'test'):
        body = jobs[name]
        require('ci/image-job.sh receive' in body and 'ci/image-job.sh fetch' in body and
                'IMAGE_MODE: ${{ needs.image-prep.outputs.mode }}' in body and
                'RORTHOX_IMAGE_ID: ${{ steps.image-id.outputs.id }}' in body and
                f'RORTHOX_ACCEL: {"auto" if name == "test" else "tcg"}' in body and './ci/run.sh ' in body,
                f'{name}: unverified image or host workload')
        require('sudo apt-' not in body and 'actions/cache@' not in body and
                './configure.sh' not in body, f'{name}: legacy toolchain path')
    test = jobs['test']
    host = re.search(r'^      - name: Enable the host KVM device if present\n(.*?)(?=^      - |\Z)', test, re.M | re.S)
    require(host is not None and test.count('      - name: Enable the host KVM device if present\n') == 1,
            'required shard must prepare KVM device')
    # The marker is safe only while guarded by this exact error-handled setup.
    host_run = '''before=$(stat -c "%a %U %G" /dev/kvm 2>/dev/null || echo absent)
# A failed device setup must not fail the shard or silently use KVM.
if [ -c /dev/kvm ]; then
  failed=
  echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null || failed=udev-rule
  if [ -z "$failed" ]; then sudo udevadm control --reload-rules || failed=reload-rules; fi
  if [ -z "$failed" ]; then sudo udevadm trigger --name-match=kvm || failed=trigger; fi
  if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi
  if [ -n "$failed" ]; then
    echo "::warning title=KVM device setup::$failed failed; shard will use TCG"
    echo "RORTHOX_KVM_SETUP_FAILED=$failed failed" >> "$GITHUB_ENV"
  fi
fi
after=$(stat -c "%a %U %G" /dev/kvm 2>/dev/null || echo absent)
bash ci/kvm-diagnostics.sh host "$before" "$after" || { echo '::warning title=KVM diagnostics::host diagnostics failed'; echo 'RORTHOX_KVM_SETUP_FAILED=host diagnostics failed' >> "$GITHUB_ENV"; }
'''
    require('        run: |\n' in host.group(1) and
            textwrap.dedent(re.split(r'^      # ', host.group(1).split('        run: |\n', 1)[1],
                                    maxsplit=1, flags=re.M)[0]) == host_run,
            'KVM host setup must remain error-handled')
    allowed_sudo = [
        "echo 'KERNEL==\"kvm\", GROUP=\"kvm\", MODE=\"0666\", OPTIONS+=\"static_node=kvm\"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null || failed=udev-rule",
        'if [ -z "$failed" ]; then sudo udevadm control --reload-rules || failed=reload-rules; fi',
        'if [ -z "$failed" ]; then sudo udevadm trigger --name-match=kvm || failed=trigger; fi',
        'if [ -z "$failed" ]; then sudo udevadm settle --timeout=30 || failed=settle; fi',
    ]
    # The full error-handled command must be present in the named step, nowhere else.
    raw_sudo = [line.strip() for line in text.splitlines() if re.search(r'\bsudo\b', line)]
    require(raw_sudo == allowed_sudo and
            all(('            ' + line) in host.group(1).splitlines() for line in allowed_sudo) and
            'bash ci/kvm-diagnostics.sh host "$before" "$after"' in host.group(1) and
            'RORTHOX_KVM_SETUP_FAILED=$failed failed' in host.group(1),
            'KVM sudo is confined to shard device setup')
    if yaml is not None:
        # Decoded YAML scalars can hide sudo in a shell override as well as a run.
        parsed_sudo = [(name, line.strip()) for name, job in parsed.items()
                       for step in job['steps']
                       for field in ('run', 'shell')
                       for line in str(step.get(field, '')).splitlines()
                       if re.search(r'\bsudo\b', line)]
        require(parsed_sudo == [('test', line) for line in allowed_sudo] and
                not re.search(r'\bsudo\b', str(document.get('defaults', {}))) and
                all(not re.search(r'\bsudo\b', str(job.get('defaults', {}))) for job in parsed.values()),
                'KVM sudo is confined to shard device setup')
    require(re.search(r'^          RORTHOX_ACCEL: auto$', test, re.M) and
            "RORTHOX_TEST_TIMEOUT: '300'" not in test and
            not re.search(r'\b(?:docker|rsync)\b', test, re.I) and
            not re.search(r'^    (?:container|services):', test, re.M),
            'required shards must select KVM once without host Docker or rsync')
    # Only the named TCG-only step may force an accelerator; parsed run strings
    # catch both inline assignments and exports hidden in a quoted YAML scalar.
    tcg_step = re.search(r'^      - name: Test TCG-only boot cases\n(.*?)(?=^      - |\Z)', test, re.M | re.S)
    require(tcg_step is not None and "if: ${{ !cancelled() && matrix.shard == 1 && steps.image-id.outcome == 'success' }}" in tcg_step.group(1) and
            '          RORTHOX_ACCEL: tcg' in tcg_step.group(1) and
            'python3 -B scripts/tcg_only.py' in tcg_step.group(1) and
            'steps.image-id.outcome == \'success\'' in tcg_step.group(1) and
            'scripts/tcg_only.py' in tcg_step.group(1) and
            'if [ "${#tcg_only[@]}" -eq 0 ]; then exit 0; fi' in tcg_step.group(1) and
            './ci/run.sh ./scripts/run-tests.sh --results build/tests/runs/shard-1/tcg-only' in tcg_step.group(1) and
            'build/tests/runs/shard-${{ matrix.shard }}/tcg-only/results.json' in test and
            'scripts/ci-result.py' in jobs['result'], 'required TCG-only run missing')
    upload = re.search(r'^      - name: Upload shard results\n(.*?)(?=^      - |\Z)',
                       test, re.M | re.S)
    require(upload is not None and test.count('      - name: Upload shard results\n') == 1 and
            test.count('          name: results-${{ matrix.shard }}\n') == 1 and
            '          name: results-${{ matrix.shard }}\n' in upload.group(1) and
            upload.group(1).count('          path: |\n') == 1,
            'shard results upload paths missing')
    paths = upload.group(1).split('          path: |\n', 1)[1].split('          if-no-files-found:', 1)[0].splitlines()
    require(len(paths) >= 8 and all(re.match(r'^            build/tests/runs/shard-\$\{\{ matrix\.shard \}\}/[^/]+', path)
                                    for path in paths),
            'shard results upload paths must share their shard root')
    run_override = re.compile(r'\b(?:export\s+)?RORTHOX_ACCEL\s*=|\bunset\s+RORTHOX_ACCEL\b|\benv\s+-u\s+RORTHOX_ACCEL\b')
    other_test = test.replace(tcg_step.group(0), '')
    require(not run_override.search(text.split('jobs:', 1)[0]) and
            all(not run_override.search(body.split('    steps:', 1)[0]) for body in jobs.values()),
            'defaults must not override its accelerator')
    require(not run_override.search(other_test) and
            not run_override.search(tcg_step.group(1).split('        run: |', 1)[-1]), 'shard must not override its accelerator')
    if yaml is not None:
        require(not run_override.search(str(document.get('defaults', {}))) and
                all(not run_override.search(str(job.get('defaults', {}))) for job in parsed.values()) and
                all(not run_override.search(str(step.get(field, '')))
                    for step in parsed['test']['steps'] for field in ('run', 'shell', 'env')) and
                all('RORTHOX_ACCEL' not in step.get('env', {})
                    for step in parsed['test']['steps']
                    if step.get('name') not in ('Verify and test', 'Test TCG-only boot cases')),
                'shard must not override its accelerator')
        require('container' not in parsed['test'] and 'services' not in parsed['test'] and
                sum(step.get('name') == 'Enable the host KVM device if present'
                    for step in parsed['test']['steps']) == 1 and
                next(step for step in parsed['test']['steps'] if step.get('name') == 'Verify and test')
                    .get('env', {}).get('RORTHOX_ACCEL') == 'auto' and
                sum(step.get('name') == 'Test TCG-only boot cases'
                    for step in parsed['test']['steps']) == 1 and
                next(step for step in parsed['test']['steps'] if step.get('name') == 'Test TCG-only boot cases')
                    .get('env', {}).get('RORTHOX_ACCEL') == 'tcg',
                'required shard KVM device setup or accelerator missing')
    marker = 'RORTHOX_KVM_SETUP_FAILED'
    marker_lines = [line.strip() for line in text.splitlines() if marker in line]
    allowed_marker = [
        'echo "RORTHOX_KVM_SETUP_FAILED=$failed failed" >> "$GITHUB_ENV"',
        "bash ci/kvm-diagnostics.sh host \"$before\" \"$after\" || { echo '::warning title=KVM diagnostics::host diagnostics failed'; echo 'RORTHOX_KVM_SETUP_FAILED=host diagnostics failed' >> \"$GITHUB_ENV\"; }",
    ]
    require(marker_lines == allowed_marker and
            all(line in host.group(1) for line in allowed_marker),
            'KVM setup marker must be confined to host setup failures')
    if yaml is not None:
        require(all(marker not in str(scope.get('env', {})) and marker not in str(scope.get('defaults', {}))
                    for scope in (document, *parsed.values())) and
                all(marker not in str(step.get('env', {})) for job in parsed.values() for step in job['steps']) and
                all(marker not in str(step.get(field, '')) for job in parsed.values()
                    for step in job['steps'] if step.get('name') != 'Enable the host KVM device if present'
                    for field in ('run', 'shell')),
                'KVM setup marker must be confined to host setup failures')
    require('    continue-on-error:' not in jobs['image-prep'],
            'image preparation must be required')
    # Context references must not put credentials in the workflow or read-only jobs.
    # github.ref and other non-secret fields are allowed; whole-context exports are not.
    scope = re.split(r'^jobs:[ \t]*$', text, maxsplit=1, flags=re.M)[0]
    outside = scope + ''.join(body for name, body in jobs.items() if name != 'image')
    credential = re.compile(r'\bsecrets\b|\bgithub\b'
                            r'(?!-output\b|\s*\.\s*(?:event_name|event\.before|event\.pull_request\.base\.sha|repository|ref|sha|workflow)\b)', re.I)
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
    token_env = "GH_TOKEN: ${{ github.event_name == 'push' && github.token || '' }}"
    outside = outside.replace(token_env, '')
    require(select.count(token_env) == 1 and
            not credential.search(outside) and
            (yaml is None or not contains_credential({**{k: v for k, v in document.items() if k != 'jobs'},
                                                      'jobs': {k: ({**v, 'steps': [{**step, 'env': {k2: v2 for k2, v2 in step.get('env', {}).items()
                            if not (k == 'select' and step.get('name') == 'Choose tests' and k2 == 'GH_TOKEN')} }
                            for step in v['steps']]} if k == 'select' else v)
                             for k, v in parsed.items() if k != 'image'}})),
            'credentials outside publication')
    require('retention-days: 1' in jobs['image-prep'], 'candidate archive retention too long')
    require(text.count(token_env) == 1 and text.count('secrets.') == 1 and 'GHCR_TOKEN: ${{ secrets.GITHUB_TOKEN }}' in jobs['image'] and
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
    require('RORTHOX_REQUIRE_ISO:' in jobs['test'] and
            jobs['test'].count('./ci/run.sh ./scripts/run-tests.sh --shard') == 2,
            'required sharded test invocation missing')


if __name__ == '__main__':
    try:
        lint(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.github/workflows/build.yml').read_text())
    except (Invalid, OSError, StopIteration) as e:
        print(f'workflow lint: {e}', file=sys.stderr)
        sys.exit(1)
    print('workflow lint: OK')
