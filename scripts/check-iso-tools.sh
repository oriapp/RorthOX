#!/usr/bin/env bash
# Check the optional BIOS GRUB ISO build tools before staging an image.
set -euo pipefail

missing=()
for tool in grub-mkrescue grub-file xorriso mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing+=("$tool")
    fi
done
platform=/usr/lib/grub/i386-pc
if [ ! -d "$platform" ]; then
    missing+=("$platform")
else
    for file in modinfo.sh cdboot.img normal.mod multiboot.mod; do
        if [ ! -f "$platform/$file" ]; then
            missing+=("$platform/$file")
        fi
    done
fi
if [ "${#missing[@]}" -ne 0 ]; then
    printf 'error: ISO tools unavailable: %s\n' "${missing[*]}" >&2
    printf '%s\n' 'Install the optional BIOS ISO packages (not grub-pc):' \
        'sudo apt-get install -y grub-common grub-pc-bin xorriso mtools' >&2
    exit 77
fi
