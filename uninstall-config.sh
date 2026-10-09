#!/usr/bin/env bash
# Removes the cross toolchain installed by ./configure.sh and its download/build directory.
# System packages installed with apt are left alone (other software may need them).
set -euo pipefail

PREFIX="${PREFIX:-$HOME/opt/cross}"
SRC_DIR="${SRC_DIR:-$HOME/src/rorthox-toolchain}"

echo "This removes:"
echo "  $PREFIX"
echo "  $SRC_DIR"
read -r -p "Continue? [y/N] " answer
case "$answer" in
    y|Y) ;;
    *) echo "Aborted."; exit 1 ;;
esac

rm -rf "$PREFIX" "$SRC_DIR"
echo "Removed. To remove the apt packages too, see the apt-get line in configure.sh."
