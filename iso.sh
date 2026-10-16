#!/usr/bin/env bash
# Build the BIOS GRUB ISO and its matching ATA disk image.
# Extra arguments are passed to make (e.g. ./iso.sh -j8).
set -euo pipefail
cd "$(dirname "$0")"
export PATH="${PREFIX:-$HOME/opt/cross}/bin:$PATH"
exec make iso "$@"
