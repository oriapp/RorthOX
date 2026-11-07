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
    for action, release in [('checkout', 'v6'), ('cache', 'v6'),
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
    jobs = re.split(r'^  (build|test|result):\s*$', text, flags=re.M)
    require(len(jobs) == 7 and jobs[1::2] == ['build', 'test', 'result'], 'expected three jobs')
    for name, body in zip(jobs[1::2], jobs[2::2]):
        grants = re.findall(r'^    permissions:\n((?:      [^\n]+\n?)+)', body, re.M)
        require(len(grants) == 1 and grants[0].splitlines() == ['      contents: read'],
                f'{name}: permissions must be exactly contents: read')
    # Only block-style, unaliased YAML keys are supported by this line-oriented lint.
    require(not re.search(r'^\s*(?:-\s*)?(?:["\']run["\']|run\s+):', text, re.M),
            'unsupported run key form')
    require(not re.search(r'^\s*-\s*\{', text, re.M), 'flow-mapping step')
    require(not re.search(r'(?:^\s*-\s*|:\s*)[&*][A-Za-z0-9_-]+\b', text, re.M),
            'YAML anchors and aliases are not supported')
    # Expressions in shell commands are interpreted before shell quoting; use env: instead.
    lines = text.splitlines()
    for i, line in enumerate(lines):
        match = re.match(r'^(\s*(?:-\s+)?)run:\s*(.*)$', line)
        if match is None:
            continue
        indent = len(match[1])
        require('${{' not in match[2], 'expression inside run')
        for command in lines[i + 1:]:
            if command.strip() and len(command) - len(command.lstrip()) <= indent:
                break
            require('${{' not in command, 'expression inside run')
    require('needs: build' in jobs[4] and 'fail-fast: false' in jobs[4]
            and 'shard: [1, 2, 3, 4]' in jobs[4], 'four independent shards required')
    require('needs: [build, test]' in jobs[6] and re.search(r'^    if: always\(\)$', jobs[6], re.M)
            and 'scripts/ci-result.py' in jobs[6], 'aggregate must run after failure')
    require('scripts/ci-artifact.py extract' in jobs[4]
            and 'scripts/ci-artifact.py create' in jobs[2], 'build handoff must be verified')
    require('RORTHOX_REQUIRE_ISO:' in jobs[4] and 'run-tests.sh --shard' in jobs[4],
            'required sharded test invocation missing')


if __name__ == '__main__':
    try:
        lint(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.github/workflows/build.yml').read_text())
    except (Invalid, OSError, StopIteration) as e:
        print(f'workflow lint: {e}', file=sys.stderr)
        sys.exit(1)
    print('workflow lint: OK')
