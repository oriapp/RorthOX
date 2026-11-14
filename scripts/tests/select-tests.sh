#!/usr/bin/env bash
#
# scripts/select-tests.py against throwaway git repositories, without QEMU. Every row builds a
# base and a head commit in one scratch repository, runs the selector on them and checks the mode
# (none, subset, full), the kind of reason it gives and the exact test list:
#
# - documentation only (README.md, docs/**) selects no tests; top-level tests (with or without
#   documentation) select themselves plus smoke; heap-oom adds argument-oom, elf-unwind and open-oom,
#   exceptions adds tss-io;
# - everything else selects the full suite: shared test helpers (lib/, host/), deleted and renamed
#   tests (also a rename into docs/), sources, build files, the runner, CI files, unknown paths, type
#   changes, symlinks, submodule entries, .gitattributes, odd file names, an empty diff, a missing,
#   zero or non-ancestor push base, a pull request without a merge base, unknown events, every push
#   to main, scheduled and manual runs;
# - a pull request compares its head with the merge base, not with the base branch's tip;
# - stdout is exactly mode=, reason= and tests= lines, every subset entry is a plain test path, and
#   --github-output appends the same values with a random delimiter that none of them contains.
#
# Last, the real tree: when a top-level test runs another one (heap-oom.sh is run by argument-oom.sh),
# a change to the one that is run must also select the one that runs it. The references between
# top-level tests are found by name; the few that are only report data (test names in fixtures) are
# listed in `data_only` below.
#
# Output: build/tests/select-tests/ (rows.txt: what the selector printed for every row)

set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/select-tests
rm -rf "$OUT"
mkdir -p "$OUT"

python3 -B - "$PWD" "$OUT" <<'PY'
import os
import pathlib
import re
import secrets
import subprocess
import sys
import tempfile

root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
selector = root / 'scripts/select-tests.py'
entry = re.compile(r'scripts/tests/[A-Za-z0-9._-]+\.sh\Z')
failures = 0
rows_log = open(out / 'rows.txt', 'w', encoding='utf-8', errors='backslashreplace')

work = pathlib.Path(tempfile.mkdtemp(dir=out))
env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, HOME=str(work),
           GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t',
           LC_ALL='C', GIT_CEILING_DIRECTORIES=str(work))


def git(repo, *args, data=None):
    p = subprocess.run(['git', '-C', str(repo), *args], env=env, input=data,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return p.stdout.decode().strip()


def report(ok, name, detail=''):
    global failures
    if ok:
        print(f'  ok   {name}')
    else:
        failures += 1
        print(f'  FAIL {name}: {detail}')


def run_selector(repo, event, ref, base, head, output=None):
    """Runs the selector from a directory outside every repository; returns rc, stdout, fields."""
    args = ['python3', '-B', str(selector), '--event', event, '--ref', ref, '--base', base, '--head', head,
            '--repo', str(repo)]
    if output:
        args += ['--github-output', str(output)]
    p = subprocess.run(args, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = p.stdout.decode('utf-8', 'backslashreplace')
    return p.returncode, text, p.stderr.decode('utf-8', 'backslashreplace')


def parse(text):
    """The three stdout lines, or None when the output is not exactly mode=, reason=, tests=."""
    lines = text.split('\n')
    if len(lines) != 4 or lines[3] != '':
        return None
    fields = {}
    for key, line in zip(('mode', 'reason', 'tests'), lines[:3]):
        if not line.startswith(key + '='):
            return None
        fields[key] = line[len(key) + 1:]
    return fields


# One scratch repository. Every row starts from the same baseline commit (root, then base).
repo = work / 'repo'
repo.mkdir()
git(repo, 'init', '-q', '-b', 'main')
git(repo, 'config', 'core.symlinks', 'true')
TESTS = ('pic', 'heap', 'heap-oom', 'argument-oom', 'elf-unwind', 'open-oom', 'exceptions', 'tss-io')
baseline = {
    'README.md': 'readme\n', 'docs/a.md': 'a\n', 'docs/sub/b.md': 'b\n', 'kernel/kernel.c': 'int x;\n',
    'kernel/boot/boot.asm': 'nop\n', 'programs/blank/blank.c': 'int y;\n', 'programs/stdlib/src/stdio.c': '\n',
    'Makefile': 'all:\n', 'config.mk': 'A = 1\n', 'configure.sh': 'true\n', 'scripts/smoke.sh': 'true\n',
    'scripts/run-tests.sh': 'true\n', 'scripts/qemu-lib.sh': 'true\n', 'scripts/select-tests.py': '\n',
    'scripts/tests/lib/helper.sh': 'true\n', 'scripts/tests/host/check.c': 'int z;\n',
    '.github/workflows/build.yml': 'on: push\n', 'ci/run.sh': 'true\n', 'notes.txt': 'n\n',
    **{f'scripts/tests/{t}.sh': f'echo {t}\n' for t in TESTS},
}


def wpath(path):
    return os.path.join(os.fsencode(repo), os.fsencode(path))


def apply(ops):
    """Changes the work tree; index-only changes (a gitlink) come back to go in after `git add -A`."""
    later = []
    for op in ops:
        kind, path = op[0], op[1]
        if kind == 'w':                       # write (create or change) a file
            os.makedirs(os.path.dirname(wpath(path)), exist_ok=True)
            with open(wpath(path), 'wb') as f:
                f.write(op[2].encode() if isinstance(op[2], str) else op[2])
        elif kind == 'rm':
            os.unlink(wpath(path))
        elif kind == 'mv':
            os.makedirs(os.path.dirname(wpath(op[2])), exist_ok=True)
            os.rename(wpath(path), wpath(op[2]))
        elif kind == 'ln':                    # replace or create as a symlink
            if os.path.lexists(wpath(path)):
                os.unlink(wpath(path))
            os.makedirs(os.path.dirname(wpath(path)), exist_ok=True)
            os.symlink(op[2], wpath(path))
        elif kind == 'x':
            os.chmod(wpath(path), 0o755)
        elif kind == 'nx':
            os.chmod(wpath(path), 0o644)
        elif kind == 'gitlink':
            later.append(path)
        else:
            raise ValueError(kind)
    return later


def commit(ops, message):
    later = apply(ops)
    git(repo, 'add', '-A')
    for path in later:
        git(repo, 'update-index', '--add', '--cacheinfo', f'160000,{root_sha},{path}')
    git(repo, 'commit', '-q', '--allow-empty', '-m', message)
    return git(repo, 'rev-parse', 'HEAD')


def reset(sha):
    git(repo, 'checkout', '-q', '--detach', '-f', sha)
    git(repo, 'reset', '-q', '--hard', sha)
    git(repo, 'clean', '-qffdx')


apply([('w', p, c) for p, c in baseline.items()] + [('x', f'scripts/tests/{t}.sh') for t in TESTS])
root_sha = commit([], 'root')
base_sha = commit([('w', 'docs/a.md', 'a2\n')], 'base')

FULL_MAIN = r'main'
FULL_EVENT = r'schedul|manual|unknown event'
FULL_BASE = r'missing|zero|ancestor|unavailable|history|merge-base'
FULL_KIND = r'delet|renam|type|metadata|symlink|gitlink|file type'
FULL_PATH = r'source|helper|unknown path'
FULL_EMPTY = r'empty'
NONE_DOCS = r'doc'
SUBSET = r'test'
ANY = r'.'


def T(*names):
    return ['scripts/smoke.sh' if n == 'smoke' else f'scripts/tests/{n}.sh' for n in names]


# (name, event, ref, base, pre, ops, mode, reason, tests)
# base: 'base' (the parent of head), 'zero', 'empty', 'missing' (no such commit), 'side' (a sibling,
#       as after a force push), 'later' (a child of head), 'orphan' (unrelated history), 'head' (= head),
#       'advanced' (the base branch moved on with a kernel change; pull requests use the merge base),
#       'merge' (as 'advanced', and head is the merge commit GitHub tests for a pull request).
# pre: changes made in the base commit first.  tests: None for "any list" (odd file names).
PUSH, PR, F = 'push', 'pull_request', 'refs/heads/features'
rows = [
    ('readme only', PUSH, F, 'base', [], [('w', 'README.md', 'r2\n')], 'none', NONE_DOCS, []),
    ('docs change', PUSH, F, 'base', [], [('w', 'docs/a.md', 'x\n')], 'none', NONE_DOCS, []),
    ('docs new nested file', PUSH, F, 'base', [], [('w', 'docs/new/c.md', 'c\n')], 'none', NONE_DOCS, []),
    ('readme and docs', PUSH, F, 'base', [], [('w', 'README.md', 'r2\n'), ('w', 'docs/sub/b.md', 'x\n')],
     'none', NONE_DOCS, []),
    ('docs utf-8 name', PUSH, F, 'base', [], [('w', 'docs/\u00e9t\u00e9.md', 'e\n')], 'none', NONE_DOCS, []),
    ('one test', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', 'echo 2\n')], 'subset', SUBSET,
     T('smoke', 'pic')),
    ('new test', PUSH, F, 'base', [], [('w', 'scripts/tests/new-one.sh', 'echo\n')], 'subset', SUBSET,
     T('smoke', 'new-one')),
    ('test mode change', PUSH, F, 'base', [('nx', 'scripts/tests/heap.sh')],
     [('x', 'scripts/tests/heap.sh')], 'subset', SUBSET, T('smoke', 'heap')),
    ('two tests', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', '2\n'), ('w', 'scripts/tests/heap.sh', '2\n')],
     'subset', SUBSET, T('smoke', 'heap', 'pic')),
    ('tests and docs', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', '2\n'), ('w', 'README.md', '2\n'),
     ('w', 'docs/a.md', '3\n')], 'subset', SUBSET, T('smoke', 'pic')),
    ('heap-oom delegates', PUSH, F, 'base', [], [('w', 'scripts/tests/heap-oom.sh', '2\n')], 'subset', SUBSET,
     T('smoke', 'heap-oom', 'argument-oom', 'elf-unwind', 'open-oom')),
    ('exceptions delegates', PUSH, F, 'base', [], [('w', 'scripts/tests/exceptions.sh', '2\n')], 'subset', SUBSET,
     T('smoke', 'exceptions', 'tss-io')),
    ('both delegations and a caller', PUSH, F, 'base', [], [('w', 'scripts/tests/heap-oom.sh', '2\n'),
     ('w', 'scripts/tests/exceptions.sh', '2\n'), ('w', 'scripts/tests/open-oom.sh', '2\n')], 'subset', SUBSET,
     T('smoke', 'heap-oom', 'argument-oom', 'elf-unwind', 'open-oom', 'exceptions', 'tss-io')),
    ('caller only', PUSH, F, 'base', [], [('w', 'scripts/tests/tss-io.sh', '2\n')], 'subset', SUBSET,
     T('smoke', 'tss-io')),
    ('delegated test missing', PUSH, F, 'base', [('rm', 'scripts/tests/open-oom.sh')],
     [('w', 'scripts/tests/heap-oom.sh', '2\n')], 'full', r'delegat|missing', []),
    ('lib helper', PUSH, F, 'base', [], [('w', 'scripts/tests/lib/helper.sh', '2\n')], 'full', FULL_PATH, []),
    ('new lib file', PUSH, F, 'base', [], [('w', 'scripts/tests/lib/new.py', '\n')], 'full', FULL_PATH, []),
    ('host check', PUSH, F, 'base', [], [('w', 'scripts/tests/host/check.c', '\n')], 'full', FULL_PATH, []),
    ('lib helper with a test', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', '2\n'),
     ('w', 'scripts/tests/lib/helper.sh', '2\n')], 'full', FULL_PATH, []),
    ('nested test dir', PUSH, F, 'base', [], [('w', 'scripts/tests/more/x.sh', '\n')], 'full', FULL_PATH, []),
    ('not a .sh in tests', PUSH, F, 'base', [], [('w', 'scripts/tests/notes.txt', '\n')], 'full', FULL_PATH, []),
    ('.sh.orig in tests', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh.orig', '\n')], 'full', FULL_PATH, []),
    ('deleted test', PUSH, F, 'base', [], [('rm', 'scripts/tests/pic.sh')], 'full', FULL_KIND, []),
    ('renamed test', PUSH, F, 'base', [], [('mv', 'scripts/tests/pic.sh', 'scripts/tests/pic2.sh')], 'full',
     FULL_KIND, []),
    ('test renamed into docs', PUSH, F, 'base', [], [('mv', 'scripts/tests/pic.sh', 'docs/pic.sh')], 'full',
     FULL_KIND, []),
    ('kernel', PUSH, F, 'base', [], [('w', 'kernel/kernel.c', '2\n')], 'full', FULL_PATH, []),
    ('boot', PUSH, F, 'base', [], [('w', 'kernel/boot/boot.asm', '2\n')], 'full', FULL_PATH, []),
    ('programs', PUSH, F, 'base', [], [('w', 'programs/blank/blank.c', '2\n')], 'full', FULL_PATH, []),
    ('stdlib', PUSH, F, 'base', [], [('w', 'programs/stdlib/src/stdio.c', '2\n')], 'full', FULL_PATH, []),
    ('Makefile', PUSH, F, 'base', [], [('w', 'Makefile', '2\n')], 'full', FULL_PATH, []),
    ('config', PUSH, F, 'base', [], [('w', 'config.mk', '2\n')], 'full', FULL_PATH, []),
    ('configure', PUSH, F, 'base', [], [('w', 'configure.sh', '2\n')], 'full', FULL_PATH, []),
    ('runner', PUSH, F, 'base', [], [('w', 'scripts/run-tests.sh', '2\n')], 'full', FULL_PATH, []),
    ('qemu helpers', PUSH, F, 'base', [], [('w', 'scripts/qemu-lib.sh', '2\n')], 'full', FULL_PATH, []),
    ('smoke script', PUSH, F, 'base', [], [('w', 'scripts/smoke.sh', '2\n')], 'full', FULL_PATH, []),
    ('selector itself', PUSH, F, 'base', [], [('w', 'scripts/select-tests.py', '2\n')], 'full', FULL_PATH, []),
    ('workflow', PUSH, F, 'base', [], [('w', '.github/workflows/build.yml', '2\n')], 'full', FULL_PATH, []),
    ('ci dir', PUSH, F, 'base', [], [('w', 'ci/run.sh', '2\n')], 'full', FULL_PATH, []),
    ('unknown path', PUSH, F, 'base', [], [('w', 'notes.txt', '2\n')], 'full', FULL_PATH, []),
    ('readme below the root', PUSH, F, 'base', [], [('w', 'kernel/README.md', '\n')], 'full', FULL_PATH, []),
    ('docs prefix lookalike', PUSH, F, 'base', [], [('w', 'docsx/a.md', '\n')], 'full', FULL_PATH, []),
    ('docs with kernel', PUSH, F, 'base', [], [('w', 'docs/a.md', '3\n'), ('w', 'kernel/kernel.c', '2\n')],
     'full', FULL_PATH, []),
    ('test with kernel', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', '3\n'), ('w', 'kernel/kernel.c', '2\n')],
     'full', FULL_PATH, []),
    ('type change of a test', PUSH, F, 'base', [], [('ln', 'scripts/tests/pic.sh', 'heap.sh')], 'full',
     FULL_KIND, []),
    ('type change in docs', PUSH, F, 'base', [], [('ln', 'docs/a.md', 'sub/b.md')], 'full', FULL_KIND, []),
    ('new symlink in docs', PUSH, F, 'base', [], [('ln', 'docs/link.md', '../kernel/kernel.c')], 'full',
     FULL_KIND, []),
    ('new symlink test', PUSH, F, 'base', [], [('ln', 'scripts/tests/link.sh', 'pic.sh')], 'full', FULL_KIND, []),
    ('gitlink in docs', PUSH, F, 'base', [], [('gitlink', 'docs/module')], 'full', FULL_KIND, []),
    ('gitlink as a test', PUSH, F, 'base', [], [('gitlink', 'scripts/tests/module.sh')], 'full', FULL_KIND, []),
    ('.gitattributes', PUSH, F, 'base', [], [('w', '.gitattributes', '* text\n')], 'full', FULL_KIND, []),
    ('docs .gitattributes', PUSH, F, 'base', [], [('w', 'docs/.gitattributes', '* -text\n')], 'full', FULL_KIND, []),
    ('tests .gitattributes', PUSH, F, 'base', [], [('w', 'scripts/tests/.gitattributes', '*.sh eol=crlf\n')],
     'full', FULL_KIND, []),
    ('newline test name', PUSH, F, 'base', [], [('w', 'scripts/tests/a\nb.sh', '\n')], 'full', ANY, []),
    ('newline test name with a test', PUSH, F, 'base', [], [('w', 'scripts/tests/pic.sh', '3\n'),
     ('w', 'scripts/tests/heap.sh\nx.sh', '\n')], 'full', ANY, []),
    ('space in test name', PUSH, F, 'base', [], [('w', 'scripts/tests/a b.sh', '\n')], 'full', ANY, []),
    ('shell characters in test name', PUSH, F, 'base', [], [('w', 'scripts/tests/$(id);`x`.sh', '\n')], 'full',
     ANY, []),
    ('tab and quote in test name', PUSH, F, 'base', [], [('w', "scripts/tests/a\t'b.sh", '\n')], 'full', ANY, []),
    ('non-ascii test name', PUSH, F, 'base', [], [('w', 'scripts/tests/\u00e9.sh', '\n')], 'full', ANY, []),
    ('newline docs name', PUSH, F, 'base', [], [('w', 'docs/a\nb.md', '\n')], ('none', 'full'), ANY, []),
    ('invalid utf-8 docs name', PUSH, F, 'base', [], [('w', b'docs/\xff.md', '\n')], ('none', 'full'), ANY, []),
    ('empty diff (same commit)', PUSH, F, 'head', [], [('w', 'scripts/tests/pic.sh', '3\n')], 'full', FULL_EMPTY, []),
    ('empty diff (empty commit)', PUSH, F, 'base', [], [], 'full', FULL_EMPTY, []),
    ('zero before', PUSH, F, 'zero', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('empty before', PUSH, F, 'empty', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('missing before', PUSH, F, 'missing', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('force push (sibling before)', PUSH, F, 'side', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('force push (before ahead)', PUSH, F, 'later', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('unrelated before', PUSH, F, 'orphan', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('not a repository', PUSH, F, 'norepo', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('main push docs only', PUSH, 'refs/heads/main', 'base', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_MAIN, []),
    ('main push one test', PUSH, 'refs/heads/main', 'base', [], [('w', 'scripts/tests/pic.sh', '3\n')], 'full',
     FULL_MAIN, []),
    ('main push short ref', PUSH, 'main', 'base', [], [('w', 'README.md', '3\n')], 'full', FULL_MAIN, []),
    ('other branch named like main', PUSH, 'refs/heads/main2', 'base', [], [('w', 'README.md', '3\n')], 'none',
     NONE_DOCS, []),
    ('schedule', 'schedule', 'refs/heads/main', 'base', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_EVENT, []),
    ('schedule on features', 'schedule', F, 'base', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_EVENT, []),
    ('workflow_dispatch', 'workflow_dispatch', F, 'base', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_EVENT, []),
    ('pull_request_target', 'pull_request_target', F, 'base', [], [('w', 'docs/a.md', '3\n')], 'full', FULL_EVENT,
     []),
    ('pull request docs only', PR, 'refs/pull/1/merge', 'base', [], [('w', 'docs/a.md', '3\n')], 'none', NONE_DOCS,
     []),
    ('pull request into main with a test', PR, 'refs/pull/1/merge', 'base', [],
     [('w', 'scripts/tests/exceptions.sh', '3\n')], 'subset', SUBSET, T('smoke', 'exceptions', 'tss-io')),
    ('pull request uses the merge base', PR, 'refs/pull/1/merge', 'advanced', [],
     [('w', 'docs/a.md', '3\n')], 'none', NONE_DOCS, []),
    ('pull request merge commit', PR, 'refs/pull/1/merge', 'merge', [],
     [('w', 'scripts/tests/pic.sh', '3\n')], 'subset', SUBSET, T('smoke', 'pic')),
    ('pull request kernel change', PR, 'refs/pull/1/merge', 'advanced', [],
     [('w', 'kernel/kernel.c', '3\n')], 'full', FULL_PATH, []),
    ('pull request without merge base', PR, 'refs/pull/1/merge', 'orphan', [],
     [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
    ('pull request missing base', PR, 'refs/pull/1/merge', 'missing', [],
     [('w', 'docs/a.md', '3\n')], 'full', FULL_BASE, []),
]


def resolve(spec, start, head):
    """The --base value and the --head value for a row."""
    if spec in ('base', 'norepo'):
        return start, head
    if spec == 'head':
        return head, head
    if spec == 'zero':
        return '0' * 40, head
    if spec == 'empty':
        return '', head
    if spec == 'missing':
        return secrets.token_hex(20), head
    if spec == 'side':
        reset(start)
        git(repo, 'reset', '-q', '--hard', 'HEAD~1')
        return commit([('w', 'docs/sub/b.md', 'side\n')], 'side'), head
    if spec == 'later':
        reset(head)
        return commit([('w', 'docs/sub/b.md', 'later\n')], 'later'), head
    if spec == 'orphan':
        tree = git(repo, 'rev-parse', f'{start}^{{tree}}')
        return git(repo, 'commit-tree', tree, '-m', 'orphan'), head
    if spec in ('advanced', 'merge'):
        reset(start)
        moved = commit([('w', 'kernel/kernel.c', 'moved on\n')], 'main moves on')
        if spec == 'advanced':
            return moved, head
        git(repo, 'merge', '-q', '--no-ff', '-m', 'merge', head)
        return moved, git(repo, 'rev-parse', 'HEAD')
    raise ValueError(spec)


delimiters = set()
for name, event, ref, spec, pre, ops, mode, reason, tests in rows:
    reset(base_sha)
    start = commit(pre, 'pre') if pre else base_sha
    head = commit(ops, name)
    base, head = resolve(spec, start, head)
    target = work / 'not-a-repo' if spec == 'norepo' else repo
    target.mkdir(exist_ok=True)
    output = work / 'github-output'
    output.write_text('earlier=kept\n')
    rc, text, err = run_selector(target, event, ref, base, head, output)
    rows_log.write(f'== {name}\nrc={rc}\n{text}{err}')
    fields = parse(text)
    problems = []
    if rc != 0:
        problems.append(f'exit status {rc}')
    if fields is None:
        problems.append('stdout is not exactly mode=, reason=, tests=')
        fields = {'mode': '?', 'reason': '?', 'tests': '?'}
    modes = mode if isinstance(mode, tuple) else (mode,)
    if fields['mode'] not in modes:
        problems.append(f'mode {fields["mode"]!r}, expected {"/".join(modes)}')
    if not re.search(reason, fields['reason'], re.I) or not fields['reason'].strip():
        problems.append(f'reason {fields["reason"]!r} is not of the kind /{reason}/')
    listed = fields['tests'].split(' ') if fields['tests'] else []
    if fields['mode'] == 'subset':
        bad = [t for t in listed if not (t == 'scripts/smoke.sh' or entry.match(t))]
        if bad or 'scripts/smoke.sh' not in listed or len(set(listed)) != len(listed):
            problems.append(f'bad subset list {fields["tests"]!r}')
    elif listed:
        problems.append(f'{fields["mode"]} with a test list {fields["tests"]!r}')
    if sorted(listed) != sorted(tests) and fields['mode'] in modes:
        problems.append(f'tests {fields["tests"]!r}, expected {" ".join(tests)!r}')
    # --github-output: appended, each key once, in order, with one random delimiter not in any value.
    lines = output.read_text().split('\n')
    m = re.fullmatch(r'mode<<([A-Za-z0-9_]{16,})', lines[1]) if len(lines) > 1 else None
    if lines[0] != 'earlier=kept' or not m:
        problems.append(f'github output does not start as expected: {lines[:2]!r}')
    else:
        d = m.group(1)
        delimiters.add(d)
        want = ['earlier=kept']
        for key in ('mode', 'reason', 'tests'):
            want += [f'{key}<<{d}', fields[key], d]
        if lines != want + [''] or any(d in v for v in fields.values()):
            problems.append(f'github output {lines!r}')
    report(not problems, name, '; '.join(problems))

# A delimiter that repeats is not random.
report(len(delimiters) == len(rows), f'{len(rows)} different output delimiters', f'{len(delimiters)} distinct')

# The real tree: every top-level test that runs another one is selected with it.
tests_dir = root / 'scripts/tests'
names = {p.name[:-3] for p in tests_dir.glob('*.sh')}
data_only = {('ci-parity', 'boot-segments'), ('workflow', 'boot-segments'), ('workflow', 'pic'),
             ('workflow', 'heap')}
token = re.compile(r'(?<![A-Za-z0-9._-])([A-Za-z0-9._-]+)\.sh(?![A-Za-z0-9_-])')
runs = {}
for caller in sorted(names - {'select-tests'}):      # this file names tests only as fixtures
    for line in (tests_dir / f'{caller}.sh').read_text(errors='replace').splitlines():
        if line.lstrip().startswith('#'):
            continue
        for m in token.finditer(line):
            callee = m.group(1)
            if callee in names and callee != caller and (caller, callee) not in data_only:
                runs.setdefault(callee, set()).add(caller)
report('heap-oom' in runs and 'exceptions' in runs, 'the scan finds the known callers',
       f'found {sorted(runs)}')
tree = work / 'tree'
tree.mkdir()
git(tree, 'init', '-q', '-b', 'features')
for n in sorted(names):
    (tree / f'scripts/tests').mkdir(parents=True, exist_ok=True)
    (tree / f'scripts/tests/{n}.sh').write_bytes((tests_dir / f'{n}.sh').read_bytes())
git(tree, 'add', '-A')
git(tree, 'commit', '-q', '-m', 'tree')
tree_base = git(tree, 'rev-parse', 'HEAD')
for callee in sorted(runs):
    needed, todo = set(), [callee]
    while todo:                               # callers of callers too
        for c in runs.get(todo.pop(), ()):
            if c not in needed:
                needed.add(c)
                todo.append(c)
    git(tree, 'checkout', '-q', '--detach', '-f', tree_base)
    with open(tree / f'scripts/tests/{callee}.sh', 'a') as f:
        f.write('\n# changed\n')
    git(tree, 'commit', '-q', '-am', callee)
    rc, text, err = run_selector(tree, PUSH, F, tree_base, git(tree, 'rev-parse', 'HEAD'))
    rows_log.write(f'== real tree: {callee}\nrc={rc}\n{text}{err}')
    fields = parse(text) or {'mode': '?', 'tests': ''}
    got = {t[len('scripts/tests/'):-3] for t in fields['tests'].split()}
    missing = sorted(n for n in needed if n not in got)
    report(fields['mode'] == 'full' or fields['mode'] == 'subset' and not missing,
           f'real tree: {callee} selects what runs it ({", ".join(sorted(needed))})',
           f'mode {fields["mode"]}, missing {", ".join(missing)} (scripts/select-tests.py DELEGATES)')

rows_log.close()
if failures:
    print(f'select-tests: {failures} check(s) failed', file=sys.stderr)
    sys.exit(1)
print('select-tests: all checks passed')
PY
