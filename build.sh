#!/usr/bin/env bash
# Build the disk image bin/os.bin with the i686-elf cross compiler from ./configure.sh.
# Extra arguments are passed to make (e.g. ./build.sh -j8).
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${PREFIX:-$HOME/opt/cross}/bin:$PATH"
make all "$@"
