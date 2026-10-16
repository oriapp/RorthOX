#!/usr/bin/env bash
# Boot the real BIOS GRUB ISO, then prove that its Multiboot entry (not the disk
# boot sector) installs the bootstrap state. Programs still come from ATA FAT16.
# Missing optional ISO tools: exit 77 locally, or fail with RORTHOX_REQUIRE_ISO=1.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=build/tests/grub-boot
rm -rf "$OUT"
mkdir -p "$OUT"
if bash scripts/check-iso-tools.sh > "$OUT/tools.log" 2>&1; then
    :
else
    status=$?
    cat "$OUT/tools.log"
    if [ "$status" -eq 77 ] && [ "${RORTHOX_REQUIRE_ISO:-0}" != 1 ]; then
        echo "SKIP grub-boot: optional BIOS ISO tools unavailable (see package line above)"
        exit 77
    fi
    echo "FAIL grub-boot: ISO prerequisites unavailable or preflight failed" >&2
    exit 1
fi
# Once preflight succeeds, every build error is a failure, never a skip.
if ! make iso > "$OUT/build.log" 2>&1; then
    cat "$OUT/build.log"
    echo "FAIL grub-boot: make iso failed" >&2
    exit 1
fi
. ./scripts/qemu-lib.sh
sha256sum bin/os.bin bin/rorthox.iso > "$OUT/images.sha256"
GDB_DIR=$(mktemp -d)
cleanup() {
    qemu_kill
    rm -rf "$GDB_DIR"
}
trap cleanup EXIT
trap 'exit 1' TERM INT

fail() {
    echo "FAIL grub-boot: $*" >&2
    if qemu_running; then qemu_stop || :; fi
    exit 1
}

run_program() {
    wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" 25 || fail "shell did not start"
    type_line "blank.elf grubcheck"
    wait_screen "Process was terminated" 15 || fail "blank did not terminate"
    # The echoed command includes grubcheck too: require its own argv output row.
    check "blank loaded" "$Q_OUT/screen.txt" "Blank loaded"
    check "blank received its argument" "$Q_OUT/screen.txt" "grubcheck" line
    check "blank terminated" "$Q_OUT/screen.txt" "Process was terminated"
    check "kernel init completed" "$Q_OUT/debugcon.log" "Thats cool"
    check "no panic in debugcon" "$Q_OUT/debugcon.log" "PANIC" absent
    check "no panic on screen" "$Q_OUT/screen.txt" "PANIC" absent
    local status=0
    qemu_stop || status=$?
    echo "exit=$status" > "$Q_OUT/qemu-status.txt"
    check "QEMU exited normally" "$Q_OUT/qemu-status.txt" "exit=0" line
}

# Production disk is untouched. Capture the menu without pressing any keys;
# resuming the VM leaves GRUB's five-second countdown to choose the entry.
echo "grub-boot: normal ISO boot"
ISO_FLAGS="${QEMU_FLAGS:-} -boot d -cdrom bin/rorthox.iso"
QEMU_FLAGS="$ISO_FLAGS" qemu_start bin/os.bin "$OUT/normal"
trap cleanup EXIT
wait_screen "GNU GRUB" 20 || fail "GRUB menu not visible"
qemu_cmd stop
screen_snapshot
cp "$Q_OUT/screen.txt" "$OUT/grub-menu.txt"
check "GRUB text menu captured" "$OUT/grub-menu.txt" "GNU GRUB"
check "configured menu entry visible" "$OUT/grub-menu.txt" "$RORTHOX_NAME"
qemu_cmd "screendump \"$OUT/grub-menu.ppm\""
wait_file "$OUT/grub-menu.ppm" 1 || fail "GRUB menu screenshot missing"
qemu_cmd cont
run_program

# EB FE is jmp $. Keep byte 2, BPB (offset 3), signature, kernel and FAT bytes.
python3 -B - "$OUT/trap.img" <<'PY'
import pathlib
import sys
original = pathlib.Path("bin/os.bin").read_bytes()
trap = b"\xeb\xfe" + original[2:]
assert trap[3:512] == original[3:512] and trap[510:512] == b"\x55\xaa"
pathlib.Path(sys.argv[1]).write_bytes(trap)
assert pathlib.Path(sys.argv[1]).read_bytes()[2:] == original[2:]
PY

echo "grub-boot: trap disk and Multiboot machine state"
QEMU_FLAGS="$ISO_FLAGS -S -gdb unix:$GDB_DIR/entry,server=on,wait=off" \
    qemu_start "$OUT/trap.img" "$OUT/entry"
# qemu_start installs its own EXIT trap; restore socket cleanup after each boot.
trap cleanup EXIT
status=0
timeout 55 python3 -B scripts/tests/lib/multiboot_entry.py "$GDB_DIR/entry" \
    build/kernel.map bin/kernel.bin "$OUT/entry" > "$OUT/entry/rsp.log" 2>&1 || status=$?
cat "$OUT/entry/rsp.log"
echo "exit=$status" > "$OUT/entry/rsp-status.txt"
[ "$status" -eq 0 ] || fail "Multiboot machine-state checks failed (exit $status)"
run_program
sha256sum -c "$OUT/images.sha256" > "$OUT/images-result.txt" || fail "production images changed"
cat "$OUT/images-result.txt"
echo "grub-boot: elapsed ${SECONDS} s"
finish grub-boot
