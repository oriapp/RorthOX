#!/usr/bin/env bash
# Build (only what changed) and boot RorthOX in QEMU.
# Kernel debug output (I/O port 0xE9, "debugcon") is printed in this terminal.
# Extra QEMU flags: QEMU_FLAGS="..." ./qemu.sh   or   ./qemu.sh <flags>
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
exec qemu-system-i386 -m 128 \
    -drive file=bin/os.bin,format=raw,index=0,media=disk \
    -debugcon stdio ${QEMU_FLAGS:-} "$@"
