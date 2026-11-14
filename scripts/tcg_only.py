#!/usr/bin/env python3
"""List the tests that require TCG, in stable order."""
import pathlib
import re

CALL = re.compile(r'^\s*qemu_require_tcg\b', re.M)


def callers():
    return sorted(str(p) for p in pathlib.Path('scripts/tests').glob('*.sh')
                  if p.name != 'accelerator.sh' and CALL.search(p.read_text()))


if __name__ == '__main__':
    print(*callers(), sep='\n')
