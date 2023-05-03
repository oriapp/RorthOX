#!/bin/bash

set +e

# Set the PREFIX and TARGET variables and add the PREFIX/bin directory to PATH
PREFIX="$HOME/opt/cross"
TARGET=i686-elf
PATH="$PREFIX/bin:$PATH"

# Uninstall the packages
echo "Uninstalling packages..."
sudo apt remove build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo libcloog-isl-dev libisl-dev

# Remove the PREFIX directory
echo "Removing $PREFIX directory..."
rm -rf $PREFIX

# Remove the extracted source code directories and build directories
echo "Removing source code and build directories..."
rm -rf $HOME/src/binutils-2.37 $HOME/src/gcc-10.2.0 $HOME/src/build-binutils $HOME/src/build-gcc

# Confirm the uninstallation
echo "Uninstallation completed. Packages and directories have been removed."
