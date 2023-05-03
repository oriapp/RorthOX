#!/bin/sh
set -e

. ./clean.sh
. ./iso.sh
export HOST=${HOST:-$(./default-host.sh)}


# qemu-system-$(./target-triplet-to-rorth.sh $HOST) -cdrom myos.iso
qemu-system-$(./target-triplet-to-rorth.sh $HOST) -debugcon stdio -hda ./bin/os.bin
