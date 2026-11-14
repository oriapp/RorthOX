#!/usr/bin/env python3
"""Conservatively select CI tests from a verified Git revision comparison."""
import argparse
import pathlib
import re
import secrets
import subprocess
import sys

TEST = re.compile(r'scripts/tests/[A-Za-z0-9._-]+\.sh\Z')
DELEGATES = {
    'scripts/tests/heap-oom.sh': ('argument-oom', 'elf-unwind', 'open-oom'),
    'scripts/tests/exceptions.sh': ('tss-io',),
    'scripts/tests/kernel-exceptions.sh': ('test-isolation',),
    'scripts/tests/screen-waits.sh': ('test-isolation',),
    'scripts/tests/task-list.sh': ('test-isolation',),
}


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout


def file_modes(repo, revision):
    result = {}
    for record in git(repo, 'ls-tree', '-rz', revision).split(b'\0'):
        if record:
            info, path = record.split(b'\t', 1)
            result[path] = info.split(b' ', 1)[0]
    return result


def select(repo, event, ref, base, head):
    def full(why):
        return 'full', why, []

    if event == 'push' and ref in ('main', 'refs/heads/main'):
        return full('main push always runs all tests')
    if event in ('schedule', 'workflow_dispatch'):
        return full('scheduled or manual run')
    if event not in ('push', 'pull_request'):
        return full('unknown event')
    if not base or not head or set(base) == {'0'}:
        return full('missing or zero comparison revision')
    try:
        git(repo, 'rev-parse', '--verify', f'{base}^{{commit}}')
        git(repo, 'rev-parse', '--verify', f'{head}^{{commit}}')
        if event == 'push':
            ancestor = git(repo, 'merge-base', base, head).strip()
            if ancestor != git(repo, 'rev-parse', base).strip():
                return full('push before is not an ancestor')
            start = base
        else:
            start = git(repo, 'merge-base', base, head).strip().decode('ascii')
            if not start:
                return full('missing pull request merge-base')
        changed = git(repo, 'diff', '--no-renames', '-z', '--name-status', start, head).split(b'\0')
        if not changed or changed == [b'']:
            return full('empty revision diff')
        if changed[-1] != b'':
            return full('malformed revision diff')
        records = changed[:-1]
        if len(records) % 2:
            return full('malformed revision diff')
        old_modes, new_modes = file_modes(repo, start), file_modes(repo, head)
        tests = set()
        for status, path in zip(records[::2], records[1::2]):
            if status not in (b'A', b'M') or not path or path.endswith(b'/.gitattributes') or path == b'.gitattributes':
                return full('deleted, renamed, type-changed or metadata path')
            modes = (old_modes.get(path), new_modes.get(path))
            if any(mode not in (None, b'100644', b'100755') for mode in modes):
                return full('symlink, gitlink or unsupported file type')
            if modes[1] is None or status == b'M' and modes[0] is None:
                return full('missing file in revision')
            try:
                name = path.decode('utf-8')
            except UnicodeDecodeError:
                return full('unknown path encoding')
            if name == 'README.md' or name.startswith('docs/'):
                pass
            elif TEST.fullmatch(name):
                tests.add(name)
            else:
                return full('source, shared helper or unknown path')
        if not tests:
            return 'none', 'documentation-only change', []
        for name in tuple(tests):
            for dependent in DELEGATES.get(name, ()):
                target = f'scripts/tests/{dependent}.sh'
                if new_modes.get(target.encode('utf-8')) not in (b'100644', b'100755'):
                    return full('missing delegated test')
                tests.add(target)
        listed = ['scripts/smoke.sh', *sorted(tests)]
        return 'subset', f'{len(tests)} changed/dependent tests plus smoke', listed
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError):
        return full('git history or comparison unavailable')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('event', 'ref', 'base', 'head'):
        parser.add_argument('--' + key, required=True)
    parser.add_argument('--repo', default='.')
    parser.add_argument('--github-output')
    options = parser.parse_args()
    mode, reason, tests = select(pathlib.Path(options.repo), options.event, options.ref,
                                 options.base, options.head)
    fields = {'mode': mode, 'reason': reason, 'tests': ' '.join(tests)}
    for key, value in fields.items():
        print(f'{key}={value}')
    if options.github_output:
        delimiter = 'RORTHOX_' + secrets.token_hex(16)
        try:
            with open(options.github_output, 'a', encoding='utf-8') as output:
                for key, value in fields.items():
                    output.write(f'{key}<<{delimiter}\n{value}\n{delimiter}\n')
        except OSError as error:
            print(f'cannot write selection output: {error}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
