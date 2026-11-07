# The GRUB ISO tests' preflight. Source after scripts/qemu-lib.sh (it needs RORTHOX_NAME).
#
#   iso_check <dir> <test name>
#     0   the ISO tools are installed and bin/rorthox.iso holds the current bin/kernel.bin and grub.cfg
#     77  the ISO tools are missing (a skip; with RORTHOX_REQUIRE_ISO=1 this is 1 instead)
#     1   anything else: no ISO, an out-of-date ISO, a failed preflight
#
# Tests never build the ISO: `make test` (or `make iso`) builds it before any test runs, so tests only
# read bin/ and can run side by side. <dir> gets tools.log and the files read back from the ISO.
iso_check() {
    local dir="$1" name="$2" status=0
    if bash scripts/check-iso-tools.sh > "$dir/tools.log" 2>&1; then
        :
    else
        status=$?
        cat "$dir/tools.log"
        if [ "$status" -eq 77 ] && [ "${RORTHOX_REQUIRE_ISO:-0}" != 1 ]; then
            echo "SKIP $name: optional BIOS ISO tools unavailable (see package line above)"
            return 77
        fi
        echo "FAIL $name: ISO prerequisites unavailable or preflight failed" >&2
        return 1
    fi
    if [ ! -f bin/rorthox.iso ]; then
        echo "FAIL $name: bin/rorthox.iso not found, run 'make iso' first" >&2
        return 1
    fi
    rm -rf "$dir/iso"
    mkdir -p "$dir/iso"
    if ! xorriso -osirrox on -indev bin/rorthox.iso -extract /boot/kernel.bin "$dir/iso/kernel.bin" \
        -extract /boot/grub/grub.cfg "$dir/iso/grub.cfg" > "$dir/iso/extract.log" 2>&1; then
        cat "$dir/iso/extract.log"
        echo "FAIL $name: could not read kernel.bin and grub.cfg back from bin/rorthox.iso" >&2
        return 1
    fi
    sed "s/@RORTHOX_NAME@/$RORTHOX_NAME/g" kernel/boot/grub.cfg.in > "$dir/iso/grub.cfg.expected"
    if ! cmp -s bin/kernel.bin "$dir/iso/kernel.bin" || ! cmp -s "$dir/iso/grub.cfg.expected" "$dir/iso/grub.cfg"; then
        echo "FAIL $name: bin/rorthox.iso is out of date (kernel.bin or grub.cfg differs), run 'make iso' first" >&2
        return 1
    fi
    return 0
}
