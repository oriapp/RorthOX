#!/usr/bin/env bash
# Host-only contract tests for the CI build handoff.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${RORTHOX_TEST_ROOT:-build/tests}/ci-artifact
mkdir -p "$OUT"
python3 -B - "$PWD" "$OUT" <<'PY'
import gzip
import hashlib
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
    manifest_path = repo / 'build/ci-manifest.json'
    manifest_path.unlink()
    target = repo / 'build/target'
    target.write_bytes(b'keep')
    manifest_path.symlink_to(target)
    run('extract', archive, digest, expected=1)
    assert target.read_bytes() == b'keep'
    assert not (repo / 'bin/os.bin').exists()
    manifest_path.unlink()
    target.unlink()
    print('PASS refused preexisting manifest symlink')
    for case in ('symlink', 'traversal', 'traversal-listed', 'outside-listed', 'corrupt', 'missing', 'prefix'):
        bad = pathlib.Path(tmp) / (case + '.tar.gz')
        expected = digest
        with tarfile.open(archive, 'r:gz') as src, tarfile.open(bad, 'w:gz',
                                                    format=tarfile.USTAR_FORMAT) as dst:
            manifest = json.loads(src.extractfile('build/ci-manifest.json').read())
            if case in ('traversal-listed', 'outside-listed'):
                manifest['files']['build/../escape' if case == 'traversal-listed' else 'other/escape'] = hashlib.sha256(b'x').hexdigest()
            if case == 'symlink':
                manifest['files']['bin/boot.bin'] = manifest['files']['bin/os.bin']
            for item in src.getmembers():
                if case == 'missing' and item.name == 'bin/os.bin':
                    continue
                if case == 'symlink' and item.name == 'bin/os.bin':
                    item.type, item.linkname, item.size = tarfile.SYMTYPE, 'boot.bin', 0
                    dst.addfile(item)
                elif case == 'symlink' and item.name == 'bin/boot.bin':
                    payload = b'fixture bin/os.bin'
                    item.size = len(payload)
                    dst.addfile(item, io.BytesIO(payload))
                elif case == 'corrupt' and item.name == 'bin/os.bin':
                    payload = b'wrong payload'
                    item.size = len(payload)
                    dst.addfile(item, io.BytesIO(payload))
                elif case in ('symlink', 'traversal-listed', 'outside-listed') and item.name == 'build/ci-manifest.json':
                    payload = (json.dumps(manifest, sort_keys=True) + '\n').encode()
                    item.size = len(payload)
                    expected = hashlib.sha256(payload).hexdigest()
                    dst.addfile(item, io.BytesIO(payload))
                else:
                    dst.addfile(item, src.extractfile(item) if item.isfile() else None)
            if case in ('traversal', 'traversal-listed', 'outside-listed'):
                name = {'traversal': '../escape', 'traversal-listed': 'build/../escape',
                        'outside-listed': 'other/escape'}[case]
                item = tarfile.TarInfo(name)
                item.size = 1
                dst.addfile(item, io.BytesIO(b'x'))
        if case == 'prefix':
            raw = bytearray(gzip.decompress(bad.read_bytes()))
            offset = 0
            while offset < len(raw):
                header = bytearray(raw[offset:offset + 512])
                name = header[:100].rstrip(bytes([0])).decode()
                if name == 'build/ci-manifest.json':
                    header[:100] = b'ci-manifest.json'.ljust(100, bytes([0]))
                    header[345:500] = b'build'.ljust(155, bytes([0]))
                    header[257:265] = b'ustar  ' + bytes([0])
                    header[148:156] = b' ' * 8
                    header[148:156] = (f'{sum(header):06o}' + chr(0) + ' ').encode()
                    raw[offset:offset + 512] = header
                    break
                size = int(header[124:136].rstrip(bytes([0]) + b' ') or b'0', 8)
                offset += 512 + ((size + 511) // 512) * 512
            else:
                raise AssertionError('missing manifest header')
            bad.write_bytes(gzip.compress(raw))
            with tarfile.open(bad, 'r:gz') as src:
                assert 'build/ci-manifest.json' in src.getnames()
            assert 'ci-manifest.json' in subprocess.check_output(['tar', '-tzf', bad], text=True)
        run('extract', bad, expected, expected=0 if case == 'prefix' else 1)
        if case == 'prefix':
            assert (repo / 'bin/os.bin').read_bytes() == b'fixture bin/os.bin'
            assert (repo / 'build/ci-manifest.json').is_file()
            (repo / 'build/ci-manifest.json').unlink()
        else:
            assert not (repo / 'bin/os.bin').exists(), case
        assert not (repo / 'ci-manifest.json').exists(), case
        assert not (repo / 'other/escape').exists(), case
        print(f'PASS checked {case}')
    run('extract', archive, '0' * 64, expected=1)
    print('PASS manifest hash and clean handoff')
(out / 'result.txt').write_text('ci-artifact: all cases passed\n')
PY
