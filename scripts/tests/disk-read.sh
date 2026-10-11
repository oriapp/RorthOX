#!/usr/bin/env bash
#
# Disk reads must check the drive's status and stop on an error, and the byte streamer must read
# only the sectors it needs, without a stack frame per sector.
#
# Host checks (scripts/tests/host/disk-ata.c and disk-stream.c): the real kernel/disk/disk.c
# against a model of the IDE ports (no task file writes while BSY or DRQ is set, no data without
# DRQ, ERR/DF/stuck BSY/missing DRQ give -EIO within a bounded number of status reads, counts of
# 1-256 and LBAs inside 28 bits only), and the real kernel/disk/streamer.c on a model disk (no
# extra sector at an exact boundary, no read for 0 bytes, bad positions and lengths refused, the
# stack doesn't grow with the length). The kernel's own streamer.o must not call itself.
#
# QEMU: copy bin/os.bin, add programs/disktest and a copy of blank.elf named diskbad.elf, and
# point diskbad.elf's first cluster past the end of the image file (its size is below one
# cluster, so no FAT entry is read). disktest.elf starts diskbad.elf with syscall 7: QEMU's IDE
# fails that read with ERR, which used to hang the kernel waiting for DRQ. The syscall must
# return -EIO, the keyboard must still reach disktest, and blank.elf must start afterwards.
# QEMU's ide_sector_read trace shows the kernel asked for that sector. bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=60 (for each step of disktest and blank.elf)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/disk-read
PROGRAM=programs/disktest/build/disktest.elf
BLANK_ELF=programs/blank/build/blank.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
EIO=$(awk '$1 == "#define" && $2 == "EIO" { print $3 }' kernel/status.h)
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f "$BLANK_ELF" ] && [ -f build/disk/streamer.o ] ||
    { echo "disk-read: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT"/*.elf

# host_check <name> <sources...>: build and run one host check, status in $OUT/host-<name>-status.txt.
host_check() {
    local name="$1" status="not built"
    shift
    if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -w \
            -I kernel -I build/config -I scripts/tests/host "$@" \
            -o "$OUT/host-$name" > "$OUT/host-$name-build.log" 2>&1; then
        if "$OUT/host-$name" > "$OUT/host-$name.log" 2>&1; then status=0; else status=$?; fi
        { grep '^  FAIL' "$OUT/host-$name.log" | head -n 8; tail -n 1 "$OUT/host-$name.log"; } | sed 's/^/  /' || true
    else
        echo "  host build of $name failed, see $OUT/host-$name-build.log"
        head -n 10 "$OUT/host-$name-build.log" | sed 's/^/  /' || true
    fi
    echo "exit=$status" > "$OUT/host-$name-status.txt"
}

echo "disk-read: host checks of disk.c and streamer.c"
host_check disk-ata scripts/tests/host/disk-ata.c kernel/disk/disk.c kernel/memory/memory.c
host_check disk-stream scripts/tests/host/disk-stream.c scripts/tests/host/kalloc.c kernel/disk/streamer.c \
    kernel/memory/memory.c
check "ATA host check built and exited with status 0"      "$OUT/host-disk-ata-status.txt"    "exit=0" line
check "ATA host check: every port case passed"             "$OUT/host-disk-ata.log"           "disk-ata host check: 0 of"
check "streamer host check built and exited with status 0" "$OUT/host-disk-stream-status.txt" "exit=0" line
check "streamer host check: every case passed"             "$OUT/host-disk-stream.log"        "disk-stream host check: 0 of"

# The kernel build of diskstreamer_read must not call itself (one stack frame per sector).
i686-elf-objdump -dr build/disk/streamer.o > "$OUT/streamer-objdump.txt"
awk '/^[0-9a-f]+ <diskstreamer_read>:/ { on = 1; next } /^[0-9a-f]+ <.*>:/ { on = 0 } on' \
    "$OUT/streamer-objdump.txt" > "$OUT/streamer-read.txt"
if [ -s "$OUT/streamer-read.txt" ]; then
    if grep -qE 'R_386_PC32[[:space:]]+diskstreamer_read$' "$OUT/streamer-read.txt"; then
        echo "calls itself" > "$OUT/streamer-calls.txt"
    else
        echo "no self call" > "$OUT/streamer-calls.txt"
    fi
else
    echo "diskstreamer_read not found" > "$OUT/streamer-calls.txt"
fi
check "kernel streamer.o: diskstreamer_read does not call itself" "$OUT/streamer-calls.txt" "no self call" line

# The test image: disktest.elf, and diskbad.elf with its first cluster past the end of the file.
cp bin/os.bin "$OUT/disk-read-before.bin"
mcopy -i "$OUT/disk-read-before.bin" "$PROGRAM" ::disktest.elf
mcopy -i "$OUT/disk-read-before.bin" "$BLANK_ELF" ::diskbad.elf
$FATIMG info "$OUT/disk-read-before.bin" > "$OUT/geometry.txt"
$FATIMG entry "$OUT/disk-read-before.bin" diskbad.elf > "$OUT/diskbad-entry.txt"
. "$OUT/geometry.txt"
. "$OUT/diskbad-entry.txt"
# First cluster whose first sector lies at least one cluster past the last (partial) sector of the file.
file_sectors=$(( (image_bytes + bytes_per_sector - 1) / bytes_per_sector ))
bad_cluster=$(( 2 + (file_sectors + sectors_per_cluster - data_sector + sectors_per_cluster - 1) / sectors_per_cluster ))
bad_sector=$(( data_sector + (bad_cluster - 2) * sectors_per_cluster ))
cp "$OUT/disk-read-before.bin" "$OUT/disk-read.bin"
$FATIMG set-cluster "$OUT/disk-read.bin" diskbad.elf "$bad_cluster" > /dev/null
echo "diskbad.elf: $size bytes, cluster $first_cluster -> $bad_cluster (sector $bad_sector, image $file_sectors sectors)"
{
    [ "$size" -lt "$cluster_bytes" ] && echo "diskbad.elf fits in one cluster"
    [ "$bad_sector" -ge $((file_sectors + sectors_per_cluster)) ] && [ "$bad_cluster" -lt $((0xFFF0)) ] &&
        echo "bad cluster is a data cluster past the end of the image file"
    # Only the two bytes of the first cluster field may differ from the unpatched copy.
    changed=$(cmp -l "$OUT/disk-read-before.bin" "$OUT/disk-read.bin" | awk '{ print $1 - 1 }' | tr '\n' ' ' || true)
    for b in $changed; do
        [ "$b" -eq $((entry_offset + 26)) ] || [ "$b" -eq $((entry_offset + 27)) ] || echo "byte $b changed"
    done
    [ -n "$changed" ] && echo "only the first cluster field changed"
    for f in shell.elf disktest.elf; do
        mcopy -n -i "$OUT/disk-read.bin" "::$f" "$OUT/$f"
    done
    cmp -s "$OUT/shell.elf" programs/shell/build/shell.elf && cmp -s "$OUT/disktest.elf" "$PROGRAM" &&
        echo "shell.elf and disktest.elf read back unchanged"
} > "$OUT/fixture.txt" 2>&1 || true
check "fixture: diskbad.elf is smaller than a cluster"           "$OUT/fixture.txt" "diskbad.elf fits in one cluster" line
check "fixture: its first cluster lies past the end of the image" "$OUT/fixture.txt" "bad cluster is a data cluster past the end of the image file" line
check "fixture: nothing but the first cluster field changed"      "$OUT/fixture.txt" "only the first cluster field changed" line
check "fixture: no other byte changed"                            "$OUT/fixture.txt" "byte " absent
check "fixture: shell.elf and disktest.elf still read back"       "$OUT/fixture.txt" "shell.elf and disktest.elf read back unchanged" line

# Boot it. QEMU logs every sector the IDE model is asked to read.
QEMU_FLAGS="${QEMU_FLAGS:-} -d trace:ide_sector_read -D $OUT/ide-trace.log"
qemu_start "$OUT/disk-read.bin" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "disktest.elf diskbad.elf"
    if wait_screen "disktest: armed" "$RUN_TIMEOUT"; then
        cp "$Q_OUT/screen.txt" "$OUT/screen-armed.txt"
        qemu_cmd "sendkey k"
        if wait_screen "disktest: waiting for a key" "$RUN_TIMEOUT"; then
            qemu_cmd "sendkey k"
            if wait_screen "disktest: done" "$RUN_TIMEOUT" && wait_screen "Process was terminated" "$RUN_TIMEOUT"; then
                type_line "blank.elf after"
                wait_screen "Blank loaded" "$RUN_TIMEOUT" || true
                wait_screen "after" "$RUN_TIMEOUT" || true
            fi
        fi
    fi
fi
qemu_stop || true
grep -oE "sector=$bad_sector nsectors=[0-9]+" "$OUT/ide-trace.log" > "$OUT/bad-read.txt" 2> /dev/null || true
grep -oE 'disktest: result -?[0-9]+$' "$Q_OUT/screen.txt" | sed 's/disktest: result /result=/' > "$OUT/result.txt" 2> /dev/null || true

check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log"    "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log"    "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen.txt"      "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "disktest.elf ran and was armed"              "$OUT/screen.txt"      "disktest: armed"
check "the kernel read diskbad.elf's first sector"  "$OUT/bad-read.txt"    "sector=$bad_sector nsectors=1" line
check "syscall 7 came back from the failed read"    "$OUT/screen.txt"      "disktest: result"
check "it returned -EIO"                            "$OUT/result.txt"      "result=-$EIO" line
check "a key still reached disktest"                "$OUT/screen.txt"      "disktest: key"
check "disktest finished"                           "$OUT/screen.txt"      "disktest: done"
check "blank.elf started afterwards"                "$OUT/screen.txt"      "after" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish disk-read
