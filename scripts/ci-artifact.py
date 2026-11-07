#!/usr/bin/env python3
"""Package and check production build outputs for same-run CI test jobs.

Usage: python3 -B scripts/ci-artifact.py create ARCHIVE OUTPUT
       python3 -B scripts/ci-artifact.py extract ARCHIVE MANIFEST_SHA256
OUTPUT is a two-line file: manifest hash, then the archive hash.
"""
import hashlib
import io
import json
import pathlib
import re
import subprocess
import sys
import tarfile

ROOT = pathlib.Path.cwd()
MANIFEST = 'build/ci-manifest.json'
REQUIRED = {'bin/boot.bin', 'bin/kernel.bin', 'bin/os.bin', 'bin/rorthox.iso',
            'build/kernelfull.o', 'build/kernel.map',
            'build/config/rorthox-config.h', 'build/config/rorthox-config.inc',
            'build/config/rorthox-config.ld', 'build/config/rorthox-config.env',
            'programs/stdlib/stdlib.elf'}


def allowed(name):
    parts = name.split('/')
    if (name.startswith('/') or any(p in ('', '.', '..') for p in parts)
            or '\\' in name or not all(re.fullmatch(r'[A-Za-z0-9._-]+', p) for p in parts)):
        return False
    return ((parts[0] == 'bin' and len(parts) == 2)
            or (parts[0] == 'build' and len(parts) >= 2)
            or (len(parts) >= 4 and parts[0] == 'programs' and parts[2] == 'build')
            or name == 'programs/stdlib/stdlib.elf')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tool_versions():
    return {name: subprocess.check_output([name, '--version'], text=True).splitlines()[0]
            for name in ('i686-elf-gcc', 'i686-elf-ld', 'nasm', 'qemu-system-i386')}


def files_to_package():
    files = set(REQUIRED)
    files.update(p.relative_to(ROOT).as_posix() for p in ROOT.glob('build/**/*')
                 if p.is_file() and (p.suffix in ('.o', '.d') or p.parent == ROOT / 'build/config'))
    files.update(p.relative_to(ROOT).as_posix() for p in ROOT.glob('programs/*/build/**/*')
                 if p.is_file() and p.suffix in ('.o', '.d', '.elf'))
    # glob gives relative paths under cwd; do not include test output or the ISO staging directory.
    return sorted(files)


def create(archive, output):
    files = files_to_package()
    hashes = {}
    for name in files:
        p = ROOT / name
        if not allowed(name) or not p.is_file() or p.is_symlink():
            raise ValueError(f'missing or unsafe production file: {name}')
        hashes[name] = digest(p.read_bytes())
    manifest = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                'tools': tool_versions(), 'files': hashes}
    data = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode()
    with tarfile.open(archive, 'w:gz') as tar:
        for name in files:
            tar.add(ROOT / name, arcname=name, recursive=False)
        entry = tarfile.TarInfo(MANIFEST)
        entry.size = len(data)
        entry.mode = 0o644
        tar.addfile(entry, io.BytesIO(data))
    pathlib.Path(output).write_text(digest(data) + '\n' + digest(pathlib.Path(archive).read_bytes()) + '\n')
    print(f'packaged {len(files)} production files; manifest sha256 {digest(data)}')


def extract(archive, expected):
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('invalid manifest digest')
    # Check that GNU tar can list the archive before reading or extracting any member.
    subprocess.run(['tar', '-tvzf', archive], check=True, stdout=subprocess.DEVNULL)
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        names = [m.name for m in members]
        if len(names) != len(set(names)) or MANIFEST not in names:
            raise ValueError('duplicate or missing manifest member')
        for m in members:
            if not allowed(m.name) or not (m.isfile() or m.isdir()):
                raise ValueError(f'unsafe archive member: {m.name}')
        m = tar.extractfile(MANIFEST)
        if m is None:
            raise ValueError('manifest is not a regular file')
        data = m.read()
        if digest(data) != expected:
            raise ValueError('manifest hash mismatch')
        manifest = json.loads(data)
        files = manifest['files']
        if (not isinstance(files, dict) or not REQUIRED <= files.keys()
                or set(names) != set(files) | {MANIFEST}
                or not re.fullmatch('[0-9a-f]{40}', manifest['revision'])
                or manifest['revision'] != subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()):
            raise ValueError('archive file list or revision mismatch')
        for name, hexdigest in files.items():
            # Set equality above ties each key to a member already checked by allowed().
            if not re.fullmatch('[0-9a-f]{64}', hexdigest):
                raise ValueError(f'bad manifest entry: {name}')
        # Detect corruption before extraction (including an archive missing a file).
        for name in files:
            entry = tar.extractfile(name)
            if entry is None or digest(entry.read()) != files[name]:
                raise ValueError(f'checksum mismatch: {name}')
        # Refuse stale outputs; the checkout holds no built production files yet.
        for name in (*files, MANIFEST):
            if (ROOT / name).exists() or (ROOT / name).is_symlink():
                raise ValueError(f'production file already present: {name}')
        # The checked tarfile members must be the members that write the files.
        tar.extractall(path=ROOT, members=members, filter='data')
    for name, hexdigest in files.items():
        if digest((ROOT / name).read_bytes()) != hexdigest:
            raise ValueError(f'extracted file checksum mismatch: {name}')
    print(f'verified {len(files)} production files at {manifest["revision"]}')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 4 and sys.argv[1] == 'create':
            create(sys.argv[2], sys.argv[3])
        elif len(sys.argv) == 4 and sys.argv[1] == 'extract':
            extract(sys.argv[2], sys.argv[3])
        else:
            raise ValueError('usage: ci-artifact.py create ARCHIVE OUTPUT | extract ARCHIVE SHA256')
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, tarfile.TarError) as e:
        print(f'ci-artifact: {e}', file=sys.stderr)
        sys.exit(1)
