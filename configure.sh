#!/usr/bin/env bash
#
# Builds the i686-elf cross toolchain (binutils + GCC, C only) that RorthOX needs,
# following https://wiki.osdev.org/GCC_Cross-Compiler.
#
# Install the system packages first (this is the only step that needs sudo):
#
#   sudo apt-get install -y build-essential bison flex libgmp3-dev libmpc-dev \
#        libmpfr-dev texinfo curl xz-utils nasm qemu-system-x86 mtools python3
#
# Optional BIOS-only GRUB ISO packages (do not install grub-pc):
#   sudo apt-get install -y grub-common grub-pc-bin xorriso mtools
#
# Then run (no sudo):   ./configure.sh
# And add the toolchain to your PATH:   export PATH="$HOME/opt/cross/bin:$PATH"
#
# Environment overrides: PREFIX (install dir), SRC_DIR (download/build dir),
# JOBS (parallel make jobs), GNU_MIRROR, FORCE=1 (rebuild even if installed).

set -euo pipefail

TARGET=i686-elf
BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0
# sha256 of the official tarballs (GPG signatures verified against gnu-keyring.gpg).
BINUTILS_SHA256=f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800
GCC_SHA256=e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da

PREFIX="${PREFIX:-$HOME/opt/cross}"
SRC_DIR="${SRC_DIR:-$HOME/src/rorthox-toolchain}"
JOBS="${JOBS:-$(nproc)}"
GNU_MIRROR="${GNU_MIRROR:-https://ftp.gnu.org/gnu}"

export PATH="$PREFIX/bin:$PATH"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

check_host_tools() {
    local missing=()
    for tool in gcc g++ make bison flex makeinfo tar xz sha256sum curl; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    if ! printf '#include <gmp.h>\n#include <mpfr.h>\n#include <mpc.h>\n' | gcc -E -x c - >/dev/null 2>&1; then
        missing+=("libgmp/libmpfr/libmpc headers")
    fi
    if [ "${#missing[@]}" -ne 0 ]; then
        die "missing host tools: ${missing[*]}. See the apt-get line at the top of this script."
    fi
}

# fetch <url> <file> <sha256>
fetch() {
    local url="$1" file="$2" sum="$3"
    if [ ! -f "$file" ]; then
        log "Downloading $file"
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c - || die "checksum mismatch for $file (delete it and retry)"
}

build_binutils() {
    log "Building binutils $BINUTILS_VERSION"
    fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
          "binutils-$BINUTILS_VERSION.tar.xz" "$BINUTILS_SHA256"
    rm -rf "binutils-$BINUTILS_VERSION" build-binutils
    tar -xf "binutils-$BINUTILS_VERSION.tar.xz"
    mkdir build-binutils
    (
        cd build-binutils
        "../binutils-$BINUTILS_VERSION/configure" --target="$TARGET" --prefix="$PREFIX" \
            --with-sysroot --disable-nls --disable-werror
        make -j"$JOBS"
        make install
    )
}

build_gcc() {
    log "Building GCC $GCC_VERSION"
    command -v "$TARGET-as" >/dev/null || die "$TARGET-as is not in PATH; binutils must be installed first"
    fetch "$GNU_MIRROR/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
          "gcc-$GCC_VERSION.tar.xz" "$GCC_SHA256"
    rm -rf "gcc-$GCC_VERSION" build-gcc
    tar -xf "gcc-$GCC_VERSION.tar.xz"
    mkdir build-gcc
    (
        cd build-gcc
        "../gcc-$GCC_VERSION/configure" --target="$TARGET" --prefix="$PREFIX" \
            --disable-nls --enable-languages=c --without-headers
        make -j"$JOBS" all-gcc all-target-libgcc
        make install-gcc install-target-libgcc
    )
}

main() {
    check_host_tools
    mkdir -p "$SRC_DIR" "$PREFIX"
    cd "$SRC_DIR"

    if [ "${FORCE:-0}" != 1 ] && "$TARGET-ld" --version 2>/dev/null | grep -q " $BINUTILS_VERSION\$"; then
        log "binutils $BINUTILS_VERSION already installed in $PREFIX (FORCE=1 to rebuild)"
    else
        build_binutils
    fi

    if [ "${FORCE:-0}" != 1 ] && [ "$("$TARGET-gcc" -dumpversion 2>/dev/null)" = "$GCC_VERSION" ]; then
        log "GCC $GCC_VERSION already installed in $PREFIX (FORCE=1 to rebuild)"
    else
        build_gcc
    fi

    log "Done"
    "$TARGET-gcc" --version | head -n1
    "$TARGET-ld" --version | head -n1
    echo
    echo "Add the toolchain to your PATH (e.g. in ~/.bashrc):"
    echo "  export PATH=\"$PREFIX/bin:\$PATH\""
}

main "$@"
