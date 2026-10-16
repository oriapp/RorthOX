#!/usr/bin/env bash
# Check the flat kernel's Multiboot 1 header and bootstrap bounds without booting.
# Needs python3 and the outputs of make all; no GRUB tools or QEMU required.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build/tests/multiboot-header
mkdir -p "$OUT"
python3 -B scripts/tests/lib/multiboot_header.py \
    bin/kernel.bin build/kernel.map build/config/rorthox-config.inc build/kernel.asm.o \
    2>&1 | tee "$OUT/check.log"
