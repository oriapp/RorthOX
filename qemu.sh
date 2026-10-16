#!/usr/bin/env bash
# Build (only what changed) and boot RorthOX in QEMU.
# Kernel debug output (I/O port 0xE9, "debugcon") is printed in this terminal.
# Extra QEMU flags: QEMU_FLAGS="..." ./qemu.sh   or   ./qemu.sh <flags>
# BIOS GRUB boot: ./qemu.sh --iso [QEMU flags] (also attaches os.bin for programs).
set -euo pipefail
cd "$(dirname "$0")"
iso_boot=()
if [ "${1:-}" = --iso ]; then
    shift
    ./iso.sh
    iso_boot=(-boot d -cdrom bin/rorthox.iso)
else
    ./build.sh
fi
CONFIG_ENV=build/config/rorthox-config.env
[ -f "$CONFIG_ENV" ] || { echo "missing $CONFIG_ENV after building" >&2; exit 2; }
. "$CONFIG_ENV"
exec qemu-system-i386 -m "$QEMU_RAM_MIB" \
    -drive file=bin/os.bin,format=raw,index=0,media=disk \
    "${iso_boot[@]}" -debugcon stdio ${QEMU_FLAGS:-} "$@"
