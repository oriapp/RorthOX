#!/usr/bin/env bash
# Host-only contract tests for the CI build handoff.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-artifact
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

source, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
helper = source / 'scripts/ci-artifact.py'
with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp) / 'repo'
    repo.mkdir()
    subprocess.run(['git', 'init', '-q', str(repo)], check=True)
    subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                    'commit', '-q', '--allow-empty', '-m', 'fixture'], check=True)
    required = ('bin/boot.bin', 'bin/kernel.bin', 'bin/os.bin', 'bin/rorthox.iso',
                'build/kernelfull.o', 'build/kernel.map',
                'build/config/rorthox-config.h', 'build/config/rorthox-config.inc',
                'build/config/rorthox-config.ld', 'build/config/rorthox-config.env',
                'programs/stdlib/stdlib.elf')
    for name in required:
        p = repo / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'fixture ' + name.encode())
    archive, hashes = pathlib.Path(tmp) / 'production.tar.gz', pathlib.Path(tmp) / 'hashes'
    def run(*args, expected=0):
        proc = subprocess.run(['python3', '-B', str(helper), *map(str, args)], cwd=repo,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if (proc.returncode == 0) != (expected == 0):
            raise AssertionError(f'expected exit {expected}, got {proc.returncode}: {proc.stdout}')
    run('create', archive, hashes)
    digest = hashes.read_text().splitlines()[0]
    for name in required:
        (repo / name).unlink()
    run('extract', archive, digest)
    assert (repo / 'bin/os.bin').read_bytes() == b'fixture bin/os.bin'
    for name in required:
        (repo / name).unlink()
    for case in ('symlink', 'traversal', 'corrupt', 'missing'):
        bad = pathlib.Path(tmp) / (case + '.tar.gz')
        with tarfile.open(archive, 'r:gz') as src, tarfile.open(bad, 'w:gz') as dst:
            for item in src.getmembers():
                if case == 'missing' and item.name == 'bin/os.bin':
                    continue
                if case == 'corrupt' and item.name == 'bin/os.bin':
                    payload = b'wrong payload'
                    item.size = len(payload)
                    dst.addfile(item, io.BytesIO(payload))
                else:
                    dst.addfile(item, src.extractfile(item) if item.isfile() else None)
            if case in ('symlink', 'traversal'):
                item = tarfile.TarInfo('build/link' if case == 'symlink' else '../escape')
                if case == 'symlink':
                    item.type, item.linkname = tarfile.SYMTYPE, '/etc/passwd'
                else:
                    item.size = 1
                dst.addfile(item, io.BytesIO(b'x') if case == 'traversal' else None)
        run('extract', bad, digest, expected=1)
        assert not (repo / 'bin/os.bin').exists(), case
        print(f'PASS rejected {case}')
    run('extract', archive, '0' * 64, expected=1)
    print('PASS manifest hash and clean handoff')
(out / 'result.txt').write_text('ci-artifact: all cases passed\n')
PY
