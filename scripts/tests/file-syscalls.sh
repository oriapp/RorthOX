#!/usr/bin/env bash
#
# File syscalls 13-18 from ring 3, on a copy of bin/os.bin, checked from outside with mtools and
# fsck.fat and by booting the written image again.
#
# The test image gets programs/filetest (FILETEST.ELF), KEEP.TXT, OLDFILE.TXT, a read-only
# RO.TXT, a directory SUB, 49 nested directories A/A/.../A holding TT.TXT (so "0:/A/.../TT.TXT"
# is 107 bytes: a longer path must not be cut down to it), and FILLER.BIN, which leaves 12 free
# clusters. The last usable cluster is worked out from the image size (QEMU's disk is the image
# rounded up to whole sectors) and the BPB, the way the kernel does it from IDENTIFY.
#
# Boot 1 runs filetest.elf write (create, >4 KiB in short writes, append after a seek, stat,
# seeks, read back, truncate, copy BLANK.ELF to COPY.ELF, create in SUB), then copy.elf from the
# shell (the loader finds and runs the new file), errors (bad handles, paths, pointers and stack
# operands, wrong modes, 17th open, read-only file, directory), parent/child (a handle number is
# only valid in the process that opened it), nospace (until -ENOSPC), fullroot (until -ENOSPC),
# hold and crash (16 handles open at exit and at a #UD). The kernel's global descriptor table
# (file_descriptors, from build/kernel.map) is read with the QEMU monitor: 16 entries while a
# program holds its handles, none after it ended, by exit or by the fault.
#
# After boot 1: mdir/mtype show every file with the expected bytes, both FATs are the same, every
# cluster up to the last usable one is taken and the partial cluster after it is not, the root is
# full, the image size didn't change, and fsck.fat -n (if installed) on sparse copies at the BPB's
# full size finds nothing new. Boot 2 (same image): filetest.elf verify reads it all back, and
# copy.elf still runs. bin/os.bin must not change.
#
# Env: BOOT_TIMEOUT=60   RUN_TIMEOUT=120   FSCK_FAT=fsck.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
FSCK_FAT="${FSCK_FAT:-fsck.fat}"
export QEMU_TIMEOUT="${QEMU_TIMEOUT:-600}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/file-syscalls
PROGRAM=programs/filetest/build/filetest.elf
BLANK_ELF=programs/blank/build/blank.elf
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] && [ -f "$BLANK_ELF" ] && [ -f build/kernel.map ] ||
    { echo "file-syscalls: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -rf "$OUT"/*

IMG="$OUT/file-syscalls.bin"
cp bin/os.bin "$IMG"
size_before=$(stat -c %s "$IMG")
printf 'keep\n' > "$OUT/keep.txt"
printf 'old file contents, longer than the new ones\n' > "$OUT/oldfile.txt"
printf 'read only\n' > "$OUT/ro.txt"
mcopy -i "$IMG" "$PROGRAM" ::FILETEST.ELF
mcopy -i "$IMG" "$OUT/keep.txt" ::KEEP.TXT
mcopy -i "$IMG" "$OUT/oldfile.txt" ::OLDFILE.TXT
mcopy -i "$IMG" "$OUT/ro.txt" ::RO.TXT
mattrib -i "$IMG" +r ::RO.TXT
mmd -i "$IMG" ::SUB
deep=A dirs=(::A)
for ((i = 1; i < 49; i++)); do deep="$deep/A"; dirs+=("::$deep"); done
mmd -i "$IMG" "${dirs[@]}"
mcopy -i "$IMG" "$OUT/keep.txt" "::$deep/TT.TXT"
DEEP_PATH="0:/$deep/TT.TXT"

# fat_info <image>: geometry, the last usable cluster and the FAT state, as shell assignments.
fat_info() {
    python3 - "$1" << 'PY'
import os, struct, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    boot = f.read(512)
    bps, spc, reserved, fats, root_entries, small, _media, spf = struct.unpack_from('<HBHBHHBH', boot, 11)
    total = small or struct.unpack_from('<I', boot, 32)[0]
    root_sector = reserved + fats * spf
    data_sector = root_sector + (root_entries * 32 + bps - 1) // bps
    size = os.path.getsize(path)
    sectors = (size + bps - 1) // bps          # what QEMU reports for the disk
    bpb_max = (total - data_sector) // spc + 1
    max_writable = min((sectors - data_sector) // spc + 1, bpb_max)
    f.seek(reserved * bps); fat1 = f.read(spf * bps)
    f.seek((reserved + spf) * bps); fat2 = f.read(spf * bps)
    f.seek(root_sector * bps); root = f.read(root_entries * 32)
entry = lambda fat, c: struct.unpack_from('<H', fat, 2 * c)[0]
free = [c for c in range(2, max_writable + 1) if entry(fat1, c) == 0]
used_above = [c for c in range(max_writable + 1, bpb_max + 1) if entry(fat1, c) != 0]
print('cluster_bytes=%d' % (spc * bps))
print('image_bytes=%d' % size)
print('total_bytes=%d' % (total * bps))
print('max_writable=%d' % max_writable)
print('free_clusters=%d' % len(free))
print('used_above=%d' % len(used_above))
print('next_entry=%d' % entry(fat1, max_writable + 1))
print('last_entry=%d' % entry(fat1, max_writable))
print('fats_equal=%d' % (fat1 == fat2))
print('root_free=%d' % sum(1 for i in range(root_entries) if root[32 * i] in (0, 0xE5)))
PY
}

# FILLER.BIN leaves 12 free clusters below the last usable one.
fat_info "$IMG" > "$OUT/fixture-fat.txt"
. "$OUT/fixture-fat.txt"
LEAVE=12
truncate -s $(( (free_clusters - LEAVE) * cluster_bytes )) "$OUT/filler.bin"
mcopy -i "$IMG" "$OUT/filler.bin" ::FILLER.BIN
rm -f "$OUT/filler.bin"
fat_info "$IMG" > "$OUT/fixture-fat.txt"
. "$OUT/fixture-fat.txt"
{
    [ "$(stat -c %s "$IMG")" = "$size_before" ] && echo "image size unchanged by mtools"
    [ "$free_clusters" = "$LEAVE" ] && [ "$used_above" = 0 ] && echo "$LEAVE clusters free, none used past cluster $max_writable"
    [ "$fats_equal" = 1 ] && echo "FAT copies equal"
} > "$OUT/fixture.txt"
check "fixture: image size unchanged by mtools"                  "$OUT/fixture.txt" "image size unchanged by mtools" line
check "fixture: $LEAVE free clusters up to cluster $max_writable" "$OUT/fixture.txt" "$LEAVE clusters free, none used past cluster $max_writable" line
check "fixture: FAT copies equal"                                "$OUT/fixture.txt" "FAT copies equal" line
cp "$IMG" "$OUT/fixture.bin"

# The kernel's table of open files (512 pointers): how many are in use.
FD_TABLE=$(awk '$2 == "file_descriptors" && $1 ~ /^0x/ { print $1; exit }' build/kernel.map)
open_files() {
    rm -f "$Q_OUT/fd.bin"
    qemu_cmd "pmemsave $FD_TABLE 2048 \"$Q_OUT/fd.bin\""
    wait_file "$Q_OUT/fd.bin" 2048 || { echo -1; return; }
    od -An -v -tx4 -w4 "$Q_OUT/fd.bin" | awk '$1 != "00000000" { n++ } END { print n + 0 }'
}
# wait_open <n> <seconds>: until the table holds n open files (it changes while a program ends).
wait_open() {
    local deadline=$((SECONDS + $2)) n=-1
    while ((SECONDS < deadline)); do
        n=$(open_files)
        [ "$n" = "$1" ] && { echo "  $1 open files in the kernel's table"; echo "open=$n" >> "$OUT/open-files.txt"; return 0; }
        sleep 0.2
    done
    echo "  the kernel's table still holds $n open files, expected $1"
    echo "open=$n expected=$1" >> "$OUT/open-files.txt"
    return 1
}

# wait_line <text> <seconds>: until a screen line is <text>, or "> " and <text> (the shell's prompt
# can land in front of a program's line). The typed command line never matches.
wait_line() {
    local text="$1" deadline=$((SECONDS + $2))
    while ((SECONDS < deadline)); do
        qemu_running || { echo "  QEMU is not running, stopped waiting for \"$text\""; return 1; }
        screen_snapshot
        if awk -v t="$text" '$0 == t || $0 == "> " t { found = 1 } END { exit !found }' "$Q_OUT/screen.txt"; then
            echo "  \"$text\" on screen after ${SECONDS} s"
            return 0
        fi
        sleep 0.5
    done
    echo "  gave up waiting for \"$text\" after $2 s"
    return 1
}

# step <command> <marker> <tag>: run a command from the shell, wait for its last line, keep the screen.
alive=1
step() {
    local command="$1" marker="$2" tag="$3"
    [ "$alive" = 1 ] || return 1
    type_line "$command"
    if wait_line "$marker" "$RUN_TIMEOUT"; then
        cp "$Q_OUT/screen.txt" "$OUT/screen-$tag.txt"
        return 0
    fi
    cp "$Q_OUT/screen.txt" "$OUT/screen-$tag.txt" 2> /dev/null || true
    alive=0
    return 1
}
# The mode passed: stop the later steps if not (an old kernel fails the first one at once).
passed() {
    grep -qF "filetest: $1 PASS" "$OUT/screen-$1.txt" 2> /dev/null && return 0
    echo "  filetest $1 did not pass:"; grep -F 'filetest:' "$OUT/screen-$1.txt" 2> /dev/null | sed 's/^/    /' || true
    alive=0
    return 1
}

echo "file-syscalls: boot 1"
: > "$OUT/open-files.txt"
qemu_start "$IMG" "$OUT/boot1"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    echo "banner" > "$OUT/boot1-banner.txt"
    wait_open 0 10 || alive=0
    { step "filetest.elf write" "filetest: end write" write && passed write; } || true
    step "copy.elf copied1" "copied1" copy || true
    { step "filetest.elf errors" "filetest: end errors" errors && passed errors; } || true
    { step "filetest.elf parent" "filetest: end parent" parent && passed parent; } || true
    { step "filetest.elf nospace" "filetest: end nospace" nospace && passed nospace; } || true
    { step "filetest.elf fullroot" "filetest: end fullroot" fullroot && passed fullroot; } || true
    if step "filetest.elf hold" "filetest: hold armed" hold; then
        wait_open 16 10 || true
        qemu_cmd "sendkey k"
        { wait_line "filetest: end hold" "$RUN_TIMEOUT" && wait_open 0 10; } || true
    fi
    if step "filetest.elf crash" "filetest: crash armed" crash; then
        wait_open 16 10 || true
        qemu_cmd "sendkey k"
        { wait_line "filetest: raising #UD" "$RUN_TIMEOUT" && wait_open 0 10; } || true
    fi
    { [ "$alive" = 1 ] && step "blank.elf after1" "after1" after; } || true
fi
qemu_stop || true
cp "$Q_OUT/debugcon.log" "$OUT/debugcon-boot1.log" 2> /dev/null || true

check "boot 1: shell started"                         "$OUT/boot1-banner.txt"   "banner" line
check "write: create, write, append, seek, read, truncate, copy" "$OUT/screen-write.txt" "filetest: write PASS"
check "copy.elf (written by filetest) runs"           "$OUT/screen-copy.txt"    "copied1" line
check "errors: handles, paths, pointers, operands, 17th open" "$OUT/screen-errors.txt" "filetest: errors PASS"
check "parent: handle not valid in the child"         "$OUT/screen-parent.txt"  "filetest: child PASS"
check "parent: handle still valid in the parent"      "$OUT/screen-parent.txt"  "filetest: parent PASS"
check "nospace: -ENOSPC at the last usable cluster"   "$OUT/screen-nospace.txt" "filetest: nospace PASS"
check "fullroot: -ENOSPC in a full root"              "$OUT/screen-fullroot.txt" "filetest: fullroot PASS"
check "kernel table empty at the idle shell"          "$OUT/open-files.txt"     "open=0" line
grep -c '^open=16$' "$OUT/open-files.txt" > "$OUT/held.txt" || true
grep -c '^open=0$' "$OUT/open-files.txt" > "$OUT/released.txt" || true
check "16 open files while hold and crash run"        "$OUT/held.txt"           "2" line
check "all released after exit and after #UD"         "$OUT/released.txt"       "3" line
check "the shell still runs programs afterwards"      "$OUT/boot1/screen.txt"   "after1" line
check "no kernel panic (boot 1)"                      "$OUT/boot1/debugcon.log" "PANIC" absent

# From outside: mtools, the FAT, fsck.fat.
nospace_bytes=$(grep -ohE 'filetest: nospace bytes [0-9]+' "$OUT/screen-nospace.txt" 2> /dev/null | awk '{ print $4 }' || true)
fat_info "$IMG" > "$OUT/written-fat.txt"
{
    . "$OUT/written-fat.txt"
    python3 -c 'import sys
d = bytes(((i * 7 + i // 251) & 0xFF) for i in range(70000)) + b"tail\n"
open(sys.argv[1], "wb").write(d)' "$OUT/expect-data.bin"
    mdir -i "$IMG" :: > "$OUT/mdir.txt" 2>&1 && echo "mdir lists the image"
    mtype -i "$IMG" ::DATA.BIN > "$OUT/got-data.bin" && cmp -s "$OUT/got-data.bin" "$OUT/expect-data.bin" && echo "DATA.BIN as written"
    [ "$(mtype -i "$IMG" ::OLDFILE.TXT)" = "new" ] && echo "OLDFILE.TXT truncated and rewritten"
    mcopy -n -i "$IMG" ::COPY.ELF "$OUT/got-copy.elf" && cmp -s "$OUT/got-copy.elf" "$BLANK_ELF" && echo "COPY.ELF is blank.elf"
    [ "$(mtype -i "$IMG" ::SUB/NOTE.TXT)" = "note" ] && echo "SUB/NOTE.TXT as written"
    [ "$(mtype -i "$IMG" ::CHILD.TXT)" = "child" ] && echo "CHILD.TXT as written"
    [ "$(mtype -i "$IMG" ::SCRATCH.TXT)" = "const+more+root" ] && echo "SCRATCH.TXT as written"
    [ "$(mtype -i "$IMG" ::KEEP.TXT)" = "keep" ] && echo "KEEP.TXT untouched"
    [ "$(mtype -i "$IMG" "::$deep/TT.TXT")" = "keep" ] && echo "the deep TT.TXT untouched"
    [ "$(mtype -i "$IMG" ::RO.TXT)" = "read only" ] && echo "RO.TXT untouched"
    big=$(awk '$1 == "BIG" && $2 == "BIN" { print $3 }' "$OUT/mdir.txt")
    [ -n "$nospace_bytes" ] && [ "$big" = "$nospace_bytes" ] && echo "BIG.BIN holds the $nospace_bytes bytes written before -ENOSPC"
    [ "$fats_equal" = 1 ] && echo "FAT copies equal"
    [ "$free_clusters" = 0 ] && [ "$last_entry" != 0 ] && echo "every cluster up to $max_writable allocated"
    [ "$used_above" = 0 ] && [ "$next_entry" = 0 ] && echo "nothing allocated past cluster $max_writable"
    [ "$root_free" = 0 ] && echo "root directory full"
    [ "$(stat -c %s "$IMG")" = "$size_before" ] && echo "image size unchanged"
} > "$OUT/outside.txt" 2>&1 || true
check "mtools: DATA.BIN bytes"                        "$OUT/outside.txt" "DATA.BIN as written" line
check "mtools: OLDFILE.TXT is \"new\""                "$OUT/outside.txt" "OLDFILE.TXT truncated and rewritten" line
check "mtools: COPY.ELF equals blank.elf"             "$OUT/outside.txt" "COPY.ELF is blank.elf" line
check "mtools: SUB/NOTE.TXT"                          "$OUT/outside.txt" "SUB/NOTE.TXT as written" line
check "mtools: CHILD.TXT"                             "$OUT/outside.txt" "CHILD.TXT as written" line
check "mtools: SCRATCH.TXT (failed writes added nothing)" "$OUT/outside.txt" "SCRATCH.TXT as written" line
check "mtools: KEEP.TXT untouched (no truncate at -EMFILE)" "$OUT/outside.txt" "KEEP.TXT untouched" line
check "mtools: the 107-byte path's file untouched"    "$OUT/outside.txt" "the deep TT.TXT untouched" line
check "mtools: RO.TXT untouched"                      "$OUT/outside.txt" "RO.TXT untouched" line
check "mtools: BIG.BIN size is what was written"      "$OUT/outside.txt" "BIG.BIN holds the $nospace_bytes bytes written before -ENOSPC" line
check "FAT copies equal after the writes"             "$OUT/outside.txt" "FAT copies equal" line
check "every cluster up to the last usable one used"  "$OUT/outside.txt" "every cluster up to $max_writable allocated" line
check "the partial cluster after it never used"       "$OUT/outside.txt" "nothing allocated past cluster $max_writable" line
check "root directory full"                           "$OUT/outside.txt" "root directory full" line
check "image size unchanged by the writes"            "$OUT/outside.txt" "image size unchanged" line

if command -v "$FSCK_FAT" > /dev/null 2>&1; then
    SPARSE=$(mktemp -d)
    # QEMU is stopped here; remove the sparse copies even if the run is killed
    trap 'rm -rf "$SPARSE"; qemu_kill' EXIT
    trap 'exit 1' TERM INT
    for tag in fixture written; do
        src="$OUT/fixture.bin"; [ "$tag" = written ] && src="$IMG"
        cp --sparse=always "$src" "$SPARSE/$tag.img"
        truncate -s "$total_bytes" "$SPARSE/$tag.img"
        if "$FSCK_FAT" -n "$SPARSE/$tag.img" > "$OUT/fsck-$tag.log" 2>&1; then s=0; else s=$?; fi
        echo "exit=$s" >> "$OUT/fsck-$tag.log"
        grep -v -e '^fsck.fat ' -e "$SPARSE/" "$OUT/fsck-$tag.log" > "$OUT/fsck-$tag.txt" || true
        rm -f "$SPARSE/$tag.img"
    done
    rm -rf "$SPARSE"
    trap 'qemu_kill' EXIT
    trap - TERM INT
    echo "  fsck.fat on $total_bytes-byte copies: fixture $(tail -n 1 "$OUT/fsck-fixture.log"), written $(tail -n 1 "$OUT/fsck-written.log")"
    { cmp -s "$OUT/fsck-fixture.txt" "$OUT/fsck-written.txt" && echo "same findings"; } > "$OUT/fsck.txt" || true
    check "fsck.fat: nothing new after the writes" "$OUT/fsck.txt" "same findings" line
else
    echo "  fsck.fat not installed: the fsck check is skipped (mtools and the FAT checks still ran)"
fi

echo "file-syscalls: boot 2 (same image)"
alive=1
if [ -f "$OUT/screen-write.txt" ] && grep -qF "filetest: write PASS" "$OUT/screen-write.txt"; then
    qemu_start "$IMG" "$OUT/boot2"
    if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
        { step "filetest.elf verify" "filetest: end verify" verify && passed verify; } || true
        step "copy.elf copied2" "copied2" copy2 || true
    fi
    qemu_stop || true
fi
check "boot 2: everything reads back after a reboot"  "$OUT/screen-verify.txt" "filetest: verify PASS"
check "boot 2: copy.elf still runs"                   "$OUT/screen-copy2.txt"  "copied2" line
check "no kernel panic (boot 2)"                      "$OUT/boot2/debugcon.log" "PANIC" absent

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish file-syscalls
