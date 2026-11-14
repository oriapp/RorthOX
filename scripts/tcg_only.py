#!/usr/bin/env python3
"""List the tests that require TCG, in stable order."""
import pathlib
import re
import sys

CALL = re.compile(r'^\s*qemu_require_tcg\b', re.M)


def callers(tests_dir='scripts/tests'):
    return sorted('scripts/tests/' + p.name for p in pathlib.Path(tests_dir).glob('*.sh')
                  if p.name != 'accelerator.sh' and CALL.search(p.read_text()))


if __name__ == '__main__':
    if len(sys.argv) > 2:
        sys.exit('usage: tcg_only.py [TESTS_DIR]')
    print(*callers(*sys.argv[1:]), sep='\n')
