#!/usr/bin/env bash
#
# A subdirectory that can't be loaded must not leave a freed directory behind: every failure in
# fat16_load_fat_directory() returns NULL after freeing what it made, and a directory with no
# entries at all is a valid empty directory, not an allocation of 0 bytes.
#
# The test image is a copy of bin/os.bin; mtools adds dir/sub/blank.elf, a normal empty directory
# "hollow" (only . and ..), a directory "void" and a directory "bad", plus disktest.elf.
# scripts/tests/lib/fatimg.py fills the cluster of "void" with zeros (a zeroed-directory fixture:
# no entries, not even . and .., which mtools never makes) and points "bad" at the first cluster
# past the end of the image file, so reading it fails.
#
# Host check (scripts/tests/host/fat-directory.c): the real file.c, pparser.c, fat16.c and
# streamer.c open 0:/dir/sub/blank.elf with each allocation and then each disk read failing in
# turn, and open the empty, zeroed and bad directories; every failure must return 0 and leave
# nothing allocated, freed twice or written after kfree, and nothing may be allocated after a
# failed read. scripts/tests/open-oom.sh covers the same through the one-level open path.
# QEMU: disktest.elf starts bad/blank.elf (-EIO, then a key still reaches it), the shell starts
# dir/sub/blank.elf, and disktest.elf starts void/blank.elf (-EIO). bin/os.bin must not change.
#
# Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=60

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
. ./scripts/qemu-lib.sh

OUT=build/tests/fat-directory
PROGRAM=programs/disktest/build/disktest.elf
BLANK_ELF=programs/blank/build/blank.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
EIO=$(awk '$1 == "#define" && $2 == "EIO" { print $3 }' kernel/status.h)
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f "$BLANK_ELF" ] || { echo "fat-directory: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT"/*.elf

# Screen lines that are exactly <text>, or "> " and <text> (the shell's prompt can land in front
# of a program's first line).
count_lines() {
    awk -v t="$1" '$0 == t || $0 == "> " t { n++ } END { print n + 0 }' "$2" 2> /dev/null || echo 0
}

# Wait until <n> screen lines are <text> (see count_lines).
wait_lines() {
    local text="$1" n="$2" deadline=$((SECONDS + $3))
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for \"$text\""; return 1; }
        screen_snapshot
        if [ "$(count_lines "$text" "$Q_OUT/screen.txt")" -ge "$n" ]; then
            echo "  \"$text\" x$n on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$text\" x$n after $3 s"
    return 1
}

IMG="$OUT/fat-directory.bin"
cp bin/os.bin "$IMG"
mmd -i "$IMG" ::dir ::dir/sub ::hollow ::void ::bad
mcopy -i "$IMG" "$BLANK_ELF" ::dir/sub/blank.elf
mcopy -i "$IMG" "$PROGRAM" ::disktest.elf
$FATIMG info "$IMG" > "$OUT/geometry.txt"
. "$OUT/geometry.txt"
void_cluster=$($FATIMG entry "$IMG" void | sed -n 's/^first_cluster=//p')
file_sectors=$(( (image_bytes + bytes_per_sector - 1) / bytes_per_sector ))
bad_cluster=$(( 2 + (file_sectors + sectors_per_cluster - data_sector + sectors_per_cluster - 1) / sectors_per_cluster ))
bad_sector=$(( data_sector + (bad_cluster - 2) * sectors_per_cluster ))
$FATIMG fill-cluster "$IMG" "$void_cluster" 0 > /dev/null
$FATIMG set-cluster "$IMG" bad "$bad_cluster" > /dev/null
{
    $FATIMG slots "$IMG" hollow | sed 's/^/hollow /'
    $FATIMG slots "$IMG" void | sed 's/^/void /'
    mcopy -n -i "$IMG" ::dir/sub/blank.elf "$OUT/readback-blank.elf"
    cmp -s "$OUT/readback-blank.elf" "$BLANK_ELF" && echo "dir/sub/blank.elf reads back unchanged"
    [ "$bad_sector" -ge $((file_sectors + sectors_per_cluster)) ] && [ "$bad_cluster" -lt $((0xFFF0)) ] &&
        echo "bad points at a data cluster past the end of the image file"
} > "$OUT/fixture.txt" 2>&1 || true
check "fixture: dir/sub/blank.elf reads back unchanged"      "$OUT/fixture.txt" "dir/sub/blank.elf reads back unchanged" line
check "fixture: hollow holds only . and .."                   "$OUT/fixture.txt" "hollow used_slots=2" line
check "fixture: void has no entries at all"                   "$OUT/fixture.txt" "void used_slots=0" line
check "fixture: bad lies past the end of the image"           "$OUT/fixture.txt" "bad points at a data cluster past the end of the image file" line

echo "fat-directory: host check of subdirectory loading"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-directory.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$IMG" "$BLANK_ELF" > "$OUT/host-check.log" 2>&1; then host_status=0; else host_status=$?; fi
    { grep '^  FAIL' "$OUT/host-check.log" | head -n 8; grep -v '^  ' "$OUT/host-check.log"; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
# The failure injections must reach the directory loader: two allocations and two reads per level.
awk '/allocation failures inside fat16_load_fat_directory/ && $2 >= 4 { print "allocations reached" }
     /read failures in dir,/ && $2 >= 2 && $7 >= 2 { print "reads reached" }' "$OUT/host-check.log" \
    > "$OUT/host-reached.txt" 2> /dev/null || true
check "host check built and exited with status 0"                "$OUT/host-status.txt"  "exit=0" line
check "host check: every subdirectory case passed"               "$OUT/host-check.log"   "fat-directory host check: 0 of"
check "host check: allocations inside the loader failed in turn" "$OUT/host-reached.txt" "allocations reached" line
check "host check: reads of dir and dir/sub failed in turn"      "$OUT/host-reached.txt" "reads reached" line

QEMU_FLAGS="${QEMU_FLAGS:-} -d trace:ide_sector_read -D $OUT/ide-trace.log"
qemu_start "$IMG" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "disktest.elf bad/blank.elf"
    if wait_lines "disktest: armed" 1 "$RUN_TIMEOUT"; then
        qemu_cmd "sendkey k"
        if wait_lines "disktest: waiting for a key" 1 "$RUN_TIMEOUT"; then
            qemu_cmd "sendkey k"
            if wait_lines "disktest: done" 1 "$RUN_TIMEOUT" && wait_lines "Process was terminated" 1 "$RUN_TIMEOUT"; then
                type_line "dir/sub/blank.elf nested"
                if wait_lines "nested" 1 "$RUN_TIMEOUT" && wait_lines "Process was terminated" 2 "$RUN_TIMEOUT"; then
                    type_line "disktest.elf void/blank.elf"
                    if wait_lines "disktest: armed" 2 "$RUN_TIMEOUT"; then
                        qemu_cmd "sendkey k"
                        if wait_lines "disktest: waiting for a key" 2 "$RUN_TIMEOUT"; then
                            qemu_cmd "sendkey k"
                            wait_lines "Process was terminated" 3 "$RUN_TIMEOUT" || true
                        fi
                    fi
                fi
            fi
        fi
    fi
fi
qemu_stop || true
grep -oE "sector=$bad_sector nsectors=[0-9]+" "$OUT/ide-trace.log" > "$OUT/bad-read.txt" 2> /dev/null || true
grep -oE 'disktest: result -?[0-9]+$' "$Q_OUT/screen.txt" | sed 's/disktest: result /result=/' > "$OUT/result.txt" 2> /dev/null || true
{
    echo "results=$(grep -cxF "result=-$EIO" "$OUT/result.txt" || true) of $(wc -l < "$OUT/result.txt")"
    echo "keys=$(count_lines 'disktest: key' "$Q_OUT/screen.txt")"
    echo "done=$(count_lines 'disktest: done' "$Q_OUT/screen.txt")"
    echo "nested=$(count_lines 'nested' "$Q_OUT/screen.txt")"
} > "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"     "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                    "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"              "$OUT/screen.txt"   "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "the kernel read the bad directory's sector"    "$OUT/bad-read.txt" "sector=$bad_sector nsectors=1" line
check "both disktest launches returned -EIO"          "$OUT/counts.txt"   "results=2 of 2" line
check "a key still reached disktest after each"       "$OUT/counts.txt"   "keys=2" line
check "dir/sub/blank.elf started from the shell"      "$OUT/counts.txt"   "nested=1" line
check "disktest finished twice"                       "$OUT/counts.txt"   "done=2" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish fat-directory
