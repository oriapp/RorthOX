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
# QEMU, two boots: disktest.elf starts bad/blank.elf (-EIO, then a key still reaches it) and the
# shell starts dir/sub/blank.elf; in the second boot disktest.elf starts void/blank.elf (-EIO).
# scripts/tests/lib/guest-state.sh stops the guest at key requests: after the failed launch
# disktest is the only program and has the keyboard, and after each program the shell is alone
# and the same task. bin/os.bin must not change.
#
# Needs python3. Env: HOST_CC=cc   BOOT_TIMEOUT=60   RUN_TIMEOUT=60   STATE_TIMEOUT=20 (for each rendezvous)

set -euo pipefail
cd "$(dirname "$0")/../.."
HOST_CC="${HOST_CC:-cc}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
STATE_TIMEOUT="${STATE_TIMEOUT:-20}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/guest-state.sh

OUT=build/tests/fat-directory
PROGRAM=programs/disktest/build/disktest.elf
BLANK_ELF=programs/blank/build/blank.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
EIO=$(awk '$1 == "#define" && $2 == "EIO" { print $3 }' include/rorthox/status.h)
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f "$BLANK_ELF" ] || { echo "fat-directory: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT"/*.elf
rm -rf "$OUT/bad" "$OUT/void"

# Screen lines that are exactly <text>, or "> " and <text> (the shell's prompt can land in front
# of a program's first line).
count_lines() {
    awk -v t="$1" '$0 == t || $0 == "> " t { n++ } END { print n + 0 }' "$2" 2> /dev/null || echo 0
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

echo "fat-directory: kernel symbols"
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d /tmp/rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"
BASE_FLAGS="${QEMU_FLAGS:-} -gdb unix:$SOCK,server=on,wait=off"

# state <dir> <tag> [options]: the guest-state rendezvous, results in <dir>/state-<tag>.txt.
state() {
    local dir="$1" tag="$2"
    shift 2
    guest_state "$SOCK" "$dir/state-$tag.txt" "$tag" timeout="$STATE_TIMEOUT" shell="$dir/shell.txt" "$@"
}

# boot <dir>: a fresh boot of the test image with its output in <dir>, up to the shell's first key request.
boot() {
    QEMU_FLAGS="$BASE_FLAGS -d trace:ide_sector_read -D $1/ide-trace.log" qemu_start "$IMG" "$1"
    wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" &&
        guest_state "$SOCK" "$1/state-boot.txt" boot timeout="$STATE_TIMEOUT" save="$1/shell.txt" owner=first screen="$1/screen-boot.txt"
}

# disktest <dir> <directory>: disktest.elf starts <directory>/blank.elf. Every line it waits for is
# printed once in the boot.
disktest() {
    local dir="$1"
    type_line "disktest.elf $2/blank.elf"
    wait_screen_lines "$RUN_TIMEOUT" "prompt:disktest: armed" || return 1
    qemu_cmd "sendkey k"
    wait_screen_lines "$RUN_TIMEOUT" "regex:(> )?disktest: result -?[0-9]+" "prompt:disktest: waiting for a key" || return 1
    # The failed launch started nothing and left the keyboard with disktest.
    state "$dir" waiting at=last tasks=2 owner=last || return 1
    qemu_cmd "sendkey k"
    wait_screen_lines "$RUN_TIMEOUT" "prompt:disktest: done" &&
        state "$dir" done owner=first screen="$dir/screen-done.txt"
}

mkdir -p "$OUT/bad" "$OUT/void"
echo "fat-directory: boot 1, bad/blank.elf and dir/sub/blank.elf"
if boot "$OUT/bad" && disktest "$OUT/bad" bad; then
    type_line "dir/sub/blank.elf nested"
    wait_screen_lines "$RUN_TIMEOUT" "prompt:nested" &&
        state "$OUT/bad" nested owner=first screen="$OUT/bad/screen-nested.txt" || true
fi
qemu_stop || true
echo "fat-directory: boot 2, void/blank.elf"
if boot "$OUT/void"; then
    disktest "$OUT/void" void || true
fi
qemu_stop || true
grep -oE "sector=$bad_sector nsectors=[0-9]+" "$OUT/bad/ide-trace.log" > "$OUT/bad-read.txt" 2> /dev/null || true
# Counted on the screens saved at the rendezvous after each disktest: nothing is printing there.
cat "$OUT/bad/screen-done.txt" "$OUT/void/screen-done.txt" > "$OUT/screens-done.txt" 2> /dev/null || true
grep -oE 'disktest: result -?[0-9]+$' "$OUT/screens-done.txt" | sed 's/disktest: result /result=/' > "$OUT/result.txt" 2> /dev/null || true
{
    echo "results=$(grep -cxF "result=-$EIO" "$OUT/result.txt" || true) of $(wc -l < "$OUT/result.txt")"
    echo "keys=$(count_lines 'disktest: key' "$OUT/screens-done.txt")"
    echo "done=$(count_lines 'disktest: done' "$OUT/screens-done.txt")"
    echo "nested=$(count_lines 'nested' "$OUT/bad/screen-nested.txt")"
} > "$OUT/counts.txt"

for b in bad void; do
    check "$b boot: kernel reached the end of init (debugcon)" "$OUT/$b/debugcon.log" "Thats cool"
    check "$b boot: no kernel panic (debugcon)"                "$OUT/$b/debugcon.log" "PANIC" absent
    check "$b boot: shell started (banner on screen)"          "$OUT/$b/screen-boot.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
done
check "kernel symbols for the rendezvous found"       "$OUT/symbols.txt"  "found" line
# stop_checks <dir> <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/$1/state-$2.txt"
    check "$3: stopped at the task's getkey"           "$f" "PASS $2_rendezvous" line
    check "$3: task list and slots consistent"        "$f" "PASS $2_list_consistent" line
    check "$3: number of tasks"                        "$f" "PASS $2_task_count" line
    check "$3: keyboard owner"                         "$f" "PASS $2_keyboard_owner" line
    [ "$2" = boot ] || check "$3: the same shell task, process and id" "$f" "PASS $2_same_shell" line
}
stop_checks bad  boot    "bad boot (the shell alone)"
stop_checks bad  waiting "bad/blank.elf refused (nothing started, disktest owns the keyboard)"
stop_checks bad  done    "disktest ended (the shell alone, owns the keyboard)"
stop_checks bad  nested  "dir/sub/blank.elf ended (the shell alone)"
stop_checks void boot    "void boot (the shell alone)"
stop_checks void waiting "void/blank.elf refused (nothing started, disktest owns the keyboard)"
stop_checks void done    "disktest ended (the shell alone, owns the keyboard)"
check "the kernel read the bad directory's sector"    "$OUT/bad-read.txt" "sector=$bad_sector nsectors=1" line
check "both disktest launches returned -EIO"          "$OUT/counts.txt"   "results=2 of 2" line
check "a key still reached disktest after each"       "$OUT/counts.txt"   "keys=2" line
check "dir/sub/blank.elf started from the shell"      "$OUT/counts.txt"   "nested=1" line
check "disktest finished twice"                       "$OUT/counts.txt"   "done=2" line

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish fat-directory
