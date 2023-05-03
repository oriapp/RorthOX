#!/bin/sh
# set -e

. ./build.sh

# DREAM ON, DREAM ON

# mkdir -p isodir
# mkdir -p isodir/boot
# mkdir -p isodir/boot/grub

# cp sysroot/boot/rorthos.kernel isodir/boot/rorthos.kernel
# cat > isodir/boot/grub/grub.cfg << EOF
# menuentry "rorthos" {
# 	multiboot /boot/rorthos.kernel
# }
# EOF
# grub-mkrescue -o rorthos.iso isodir
