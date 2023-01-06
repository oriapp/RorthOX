
# sudo apt install build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo libcloog-isl-dev libisl-dev 
# export PREFIX="$HOME/opt/cross"
# export TARGET=i686-elf
# export PATH="$PREFIX/bin:$PATH"
# cd $HOME
# mkdir src
# cd $HOME/src
# mkdir build-binutils

# wget --progress=bar "https://ftp.gnu.org/gnu/binutils/binutils-2.37.tar.xz"
# tar -xf binutils-2.37.tar.xz
# rm -rf binutils-2.37.tar.xz

# cd build-binutils
# ../binutils-2.37/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
# make
# make install

# cd $HOME/src
# which -- $TARGET-as || echo $TARGET-as is not in the PATH

# wget --progress=bar "https://bigsearcher.com/mirrors/gcc/releases/gcc-10.2.0/gcc-10.2.0.tar.xz"
# tar -xf gcc-10.2.0.tar.xz
# rm -rf gcc-10.2.0.tar.xz

# mkdir build-gcc
# cd build-gcc
# ../gcc-10.2.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headerss
# make all-gcc
# make all-target-libgcc
# make install-gcc
# make install-target-libgcc

# $HOME/opt/cross/bin/$TARGET-gcc --version
# export PATH="$HOME/opt/cross/bin:$PATH"







#!/bin/bash

set -e

# Set the PREFIX and TARGET variables and add the PREFIX/bin directory to PATH
PREFIX="$HOME/opt/cross"
TARGET=i686-elf
PATH="$PREFIX/bin:$PATH"

# Install the necessary packages
echo "Installing necessary packages..."
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo libcloog-isl-dev libisl-dev

# Create a src directory and go to it
echo "Creating src directory and entering it..."
cd $HOME
mkdir -p src
cd src

# Download and extract the binutils source code, then create and enter a build-binutils directory
echo "Downloading and extracting binutils source code..."
wget --progress=bar "https://ftp.gnu.org/gnu/binutils/binutils-2.37.tar.xz"
tar -xf binutils-2.37.tar.xz
rm -rf binutils-2.37.tar.xz
echo "Creating build-binutils directory and entering it..."
mkdir -p build-binutils
cd build-binutils

# Configure, build, and install binutils
echo "Configuring binutils..."
../binutils-2.37/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
echo "Building binutils..."
make
echo "Installing binutils..."
make install

# Check if the $TARGET-as binary is in the PATH
echo "Checking if $TARGET-as is in the PATH..."
which -- $TARGET-as || echo "$TARGET-as is not in the PATH"

# Go back to the src directory and download and extract the GCC source code, then create and enter a build-gcc directory
echo "Returning to src directory and downloading and extracting GCC source code..."
cd $HOME/src
wget --progress=bar "https://bigsearcher.com/mirrors/gcc/releases/gcc-10.2.0/gcc-10.2.0.tar.xz"
tar -xf gcc-10.2.0.tar.xz
rm -rf gcc-10.2.0.tar.xz
echo "Creating build-gcc directory and entering it..."
mkdir -p build-gcc
cd build-gcc

# Configure, build, and install GCC
echo "Configuring GCC..."
../gcc-10.2.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headerss
echo "Building GCC..."
make all-gcc
make all-target-libgcc
echo "Installing GCC..."
make install-gcc
make install-target-libgcc

# Check the version of the installed GCC compiler
echo "Checking GCC version..."
$HOME/opt/cross/bin/$TARGET-gcc --version

# Add the PREFIX/bin directory to the PATH
echo "Updating PATH to include $PREFIX/bin..."
export
