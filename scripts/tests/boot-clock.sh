#!/usr/bin/env bash
#
# The boot clock line shows the CMOS time as HH:MM:SS with leading zeros, on both boot paths.
#
# QEMU's clock starts at 01:02:00 (-rtc base=...,clock=vm: the CMOS clock runs with the guest, from
# that time), so the kernel must print "System Time: 01:02:SS": the hour and minute exactly, with
# their leading zeros, and the seconds as two digits (the seconds the boot took). The line is waited
# for by its whole text, so the boot's speed doesn't matter as long as it takes under a minute.
#   disk  the default boot from bin/os.bin
#   grub  the real GRUB ISO with the disk's boot sector replaced by "jmp $" (as in grub-boot.sh), so
#         only GRUB can start the kernel; the shell still comes from the disk
#
# The grub case needs the ISO tools: without them it is skipped, or fails with RORTHOX_REQUIRE_ISO=1.
# It boots the ISO that make test (or make iso) built; the test never builds it.
# Env: BOOT_TIMEOUT=60  BOOT_CLOCK_CASES="disk grub"

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
CASES="${BOOT_CLOCK_CASES:-disk grub}"
RTC_BASE="2026-11-15T01:02:00"
CLOCK_LINE="System Time: 01:02:[0-5][0-9]"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/iso-check.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/boot-clock
rm -rf "$OUT"
mkdir -p "$OUT"
fail() {
    echo "FAIL boot-clock: $*" >&2
    exit 1
}

# run_case <case> <image> [qemu flags]
run_case() {
    local c="$1" image="$2" status=0
    local D="$OUT/$c"
    echo "boot-clock: $c"
    QEMU_FLAGS="-rtc base=$RTC_BASE,clock=vm ${3:-}" qemu_start "$image" "$D"
    # The clock line, and the shell idle at its prompt (nothing prints after that, so the screen
    # read at the end shows the same rows).
    wait_screen_lines "$BOOT_TIMEOUT" "regex:$CLOCK_LINE" "line:${RORTHOX_NAME} v${RORTHOX_VERSION}" "line:>" ||
        status=1
    echo "exit=$status" > "$D/wait-status.txt"
    qemu_stop || true
    # The rows that start like the clock line, as the screen showed them at the end.
    local rows good
    rows=$(grep -cE '^System Time:' "$D/screen.txt" || true)
    good=$(grep -cxE "$CLOCK_LINE" "$D/screen.txt" || true)
    grep -E '^System Time:' "$D/screen.txt" > "$D/clock.txt" || true
    echo "clock rows=$rows matching=$good" > "$D/count.txt"
    check "$c: clock line, then the shell's prompt"   "$D/wait-status.txt" "exit=0" line
    check "$c: one System Time row, and it matches"   "$D/count.txt" "clock rows=1 matching=1" line
    check "$c: no panic in debugcon"                  "$D/debugcon.log" "PANIC" absent
}

for c in $CASES; do
    case "$c" in
        disk)
            cp bin/os.bin "$OUT/disk.bin"
            run_case disk "$OUT/disk.bin"
            ;;
        grub)
            if iso_check "$OUT" "boot-clock grub"; then
                :
            else
                status=$?
                [ "$status" -eq 77 ] && continue
                fail "no up-to-date GRUB ISO (see above)"
            fi
            # "jmp $" in place of the boot sector's first bytes: only GRUB can start the kernel.
            python3 -B - "$OUT/trap.img" << 'PY' || fail "could not write the trap disk image"
import pathlib
import sys
original = pathlib.Path("bin/os.bin").read_bytes()
pathlib.Path(sys.argv[1]).write_bytes(b"\xeb\xfe" + original[2:])
PY
            run_case grub "$OUT/trap.img" "-boot d -cdrom bin/rorthox.iso"
            ;;
        *) fail "unknown case $c" ;;
    esac
done
finish boot-clock
