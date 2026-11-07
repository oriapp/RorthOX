#!/usr/bin/env bash
#
# FAT16 lookup: an entry after a deleted slot must be found (the directory size counts raw slots
# and lookup skips deleted ones), the root directory scan must stay inside the root, the first of
# two equal names wins without leaking the other, only the low first-cluster word counts on FAT16,
# and fseek() can reach the end of a file but not go past it. A name that is nowhere is reported as
# not found (-ENOENT), and nothing else is.
#
# Three copies of bin/os.bin, built with mtools and scripts/tests/lib/fatimg.py:
#   deleted  zap.elf and later.elf (both blank.elf) in the root and in directory ldir; zap.elf is
#            deleted afterwards, so a deleted slot lies right before later.elf in both.
#   full     every root slot used by short entries (zero-size fillers), last.bin in the last one,
#            no 0x00 end slot.
#   odd      dup1.bin and dup2.bin with dup2.bin's entry renamed to DUP1.BIN, hiword.bin with its
#            high first-cluster word set to 0x4000 (a reserved field on FAT16), seek.bin, empty.bin.
#
# Host check (scripts/tests/host/fat-lookup.c): the real file.c, pparser.c, fat16.c and streamer.c
# open and read these files and seek in seek.bin and empty.bin; see that file for the cases.
# QEMU (deleted image): the shell starts later.elf and ldir/later.elf. After each one,
# scripts/tests/lib/guest-state.sh stops the guest at the shell's next key request: the program
# has ended, the shell is the same task and gets the keys again. bin/os.bin must not change.
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

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-lookup
BLANK_ELF=programs/blank/build/blank.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
mkdir -p "$OUT"
[ -f bin/os.bin ] && [ -f "$BLANK_ELF" ] || { echo "fat-lookup: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
# Remove the last run's results, so the checks below only see this run.
rm -rf "$OUT/fill"
rm -f "$OUT"/host-* "$OUT"/*.txt "$OUT"/*.bin "$OUT"/*.log "$OUT"/*.img

# Screen lines that are exactly <text>, or "> " and <text> (the shell's prompt can land in front
# of a program's first line).
count_lines() {
    awk -v t="$1" '$0 == t || $0 == "> " t { n++ } END { print n + 0 }' "$2" 2> /dev/null || echo 0
}

$FATIMG info bin/os.bin > "$OUT/geometry.txt"
. "$OUT/geometry.txt"
python3 - "$OUT" <<'EOF'
import sys
out = sys.argv[1]
def pattern(size, seed):
    return bytes(((i * seed) ^ (i >> 7) ^ seed) & 0xFF for i in range(size))
for name, size, seed in (('last.bin', 300, 71), ('dup1.bin', 1000, 73), ('dup2.bin', 2000, 79),
                         ('hiword.bin', 3000, 83), ('seek.bin', 5000, 89), ('empty.bin', 0, 1)):
    open('%s/%s' % (out, name), 'wb').write(pattern(size, seed))
EOF

# deleted: the directory first, so later.elf is the last entry in both places.
DELETED="$OUT/deleted.img"
cp bin/os.bin "$DELETED"
mmd -i "$DELETED" ::ldir
mcopy -i "$DELETED" "$BLANK_ELF" ::ldir/zap.elf
mcopy -i "$DELETED" "$BLANK_ELF" ::ldir/later.elf
mcopy -i "$DELETED" "$BLANK_ELF" ::zap.elf
mcopy -i "$DELETED" "$BLANK_ELF" ::later.elf
mdel -i "$DELETED" ::zap.elf ::ldir/zap.elf

# full: zero-size fillers up to the last root slot, then last.bin.
FULL="$OUT/full.img"
cp bin/os.bin "$FULL"
used=$($FATIMG slots "$FULL" | sed -n 's/^used_slots=//p')
mkdir -p "$OUT/fill"
for ((i = used; i < root_entries - 1; i++)); do : > "$OUT/fill/f$i.bin"; done
(cd "$OUT/fill" && mcopy -i "../full.img" $(ls -v) ::)
mcopy -i "$FULL" "$OUT/last.bin" ::

# odd: a duplicate name and a high cluster word.
ODD="$OUT/odd.img"
cp bin/os.bin "$ODD"
for f in dup1.bin dup2.bin hiword.bin seek.bin empty.bin; do
    mcopy -i "$ODD" "$OUT/$f" ::
done
{
    $FATIMG slots "$DELETED" | sed 's/^/deleted root /'
    $FATIMG slots "$DELETED" ldir | sed 's/^/deleted ldir /'
    $FATIMG slots "$FULL" | sed 's/^/full root /'
    hiword_chain=$($FATIMG entry "$ODD" hiword.bin | sed -n 's/^chain=//p')
    $FATIMG rename "$ODD" dup2.bin dup1.bin > /dev/null
    $FATIMG set-high "$ODD" hiword.bin 0x4000 > /dev/null
    $FATIMG slots "$ODD" | sed 's/^/odd root /'
    mcopy -n -i "$DELETED" ::later.elf "$OUT/readback-later.elf"
    mcopy -n -i "$DELETED" ::ldir/later.elf "$OUT/readback-ldir-later.elf"
    mcopy -n -i "$FULL" ::last.bin "$OUT/readback-last.bin"
    cmp -s "$OUT/readback-later.elf" "$BLANK_ELF" && cmp -s "$OUT/readback-ldir-later.elf" "$BLANK_ELF" &&
        echo "later.elf and ldir/later.elf read back unchanged"
    cmp -s "$OUT/readback-last.bin" "$OUT/last.bin" && echo "last.bin reads back unchanged"
    echo "hiword chain=$hiword_chain"
} > "$OUT/fixture.txt" 2>&1 || true
# A deleted slot right before later.elf, which is the last entry, in the root and in ldir.
awk '/^deleted (root|ldir) slot=/ { split($3, s, "="); kind[$2, s[2]] = $4; name[$2, s[2]] = $5 }
     /^deleted (root|ldir) end=/ { split($3, e, "="); end[$2] = e[2] }
     END { for (d in end) { n = end[d]; if (n != "none" && name[d, n - 1] == "LATER___ELF" && kind[d, n - 2] == "deleted")
                                print "deleted slot before later.elf in " d } }' "$OUT/fixture.txt" > "$OUT/deleted.txt"
check "fixture: deleted slot right before later.elf in the root" "$OUT/deleted.txt" "deleted slot before later.elf in root" line
check "fixture: deleted slot right before later.elf in ldir"     "$OUT/deleted.txt" "deleted slot before later.elf in ldir" line
check "fixture: every root slot of the full image is used"       "$OUT/fixture.txt" "full root used_slots=$root_entries" line
check "fixture: the full root has no end slot"                   "$OUT/fixture.txt" "full root end=none" line
check "fixture: last.bin is in the last root slot"               "$OUT/fixture.txt" "full root slot=$((root_entries - 1)) used LAST____BIN" line
check "fixture: no long-name slots"                              "$OUT/fixture.txt" " lfn " absent
check "fixture: no volume label slots"                          "$OUT/fixture.txt" " label " absent
check "fixture: two root entries named DUP1.BIN"                 "$OUT/fixture.txt" "odd root slot=3 used DUP1____BIN" line
check "fixture: the second one too"                              "$OUT/fixture.txt" "odd root slot=2 used DUP1____BIN" line
check "fixture: later.elf files read back unchanged"             "$OUT/fixture.txt" "later.elf and ldir/later.elf read back unchanged" line
check "fixture: last.bin reads back unchanged"                   "$OUT/fixture.txt" "last.bin reads back unchanged" line

echo "fat-lookup: host check of lookup and seek"
host_status="not built"
if "$HOST_CC" -std=gnu99 -g -O0 -fno-builtin -no-pie -fno-pie -rdynamic -w -Wl,--wrap=diskstreamer_seek \
        -I kernel -I build/config -I scripts/tests/host \
        scripts/tests/host/fat-lookup.c scripts/tests/host/fat-host.c scripts/tests/host/kalloc.c \
        kernel/fs/pparser.c kernel/fs/file.c kernel/fs/fat/fat16.c kernel/disk/streamer.c \
        kernel/string/string.c kernel/memory/memory.c \
        -o "$OUT/host-check" > "$OUT/host-build.log" 2>&1; then
    if "$OUT/host-check" "$DELETED" "$FULL" "$ODD" "$BLANK_ELF" "$OUT/last.bin" "$OUT/dup1.bin" "$OUT/hiword.bin" \
            "$OUT/seek.bin" > "$OUT/host-check.log" 2>&1; then
        host_status=0
    else
        host_status=$?
    fi
    { grep -v '^  ok' "$OUT/host-check.log" | head -n 12; } | sed 's/^/  /' || true
else
    echo "  host build failed, see $OUT/host-build.log"
    head -n 10 "$OUT/host-build.log" | sed 's/^/  /' || true
fi
echo "exit=$host_status" > "$OUT/host-status.txt"
check "host check built and exited with status 0"  "$OUT/host-status.txt" "exit=0" line
check "host check: every lookup and seek case passed" "$OUT/host-check.log" "fat-lookup host check: 0 of 10 cases failed" line

echo "fat-lookup: kernel symbols"
guest_state_setup "$OUT"
GDB_DIR=
cleanup() { [ -z "$GDB_DIR" ] || rm -rf "$GDB_DIR"; }
Q_CLEANUP=cleanup
trap qemu_exit EXIT
GDB_DIR=$(mktemp -d --tmpdir rorthox-gdb.XXXXXX)
SOCK="$GDB_DIR/gdb"

# state <tag> [options]: the guest-state rendezvous at the shell's key request, the shell alone.
state() {
    local tag="$1"
    shift
    guest_state "$SOCK" "$OUT/state-$tag.txt" "$tag" timeout="$STATE_TIMEOUT" owner=first "$@"
}

QEMU_FLAGS="-gdb unix:$SOCK,server=on,wait=off" qemu_start "$DELETED" "$OUT"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT" && state boot save="$OUT/shell.txt" screen="$OUT/screen-boot.txt"; then
    type_line "later.elf root"
    if wait_screen_lines "$RUN_TIMEOUT" "prompt:root" && state root shell="$OUT/shell.txt" screen="$OUT/screen-root.txt"; then
        type_line "ldir/later.elf sub"
        wait_screen_lines "$RUN_TIMEOUT" "prompt:sub" &&
            state sub shell="$OUT/shell.txt" screen="$OUT/screen-end.txt" || true
    fi
fi
qemu_stop || true
# Counted on the screens saved at the rendezvous after each program: nothing is printing or scrolling there.
{
    echo "root=$(count_lines root "$OUT/screen-root.txt")"
    echo "sub=$(count_lines sub "$OUT/screen-end.txt")"
} > "$OUT/counts.txt"

check "kernel reached the end of init (debugcon)"   "$OUT/debugcon.log" "Thats cool"
check "no kernel panic (debugcon)"                  "$OUT/debugcon.log" "PANIC" absent
check "shell started (banner on screen)"            "$OUT/screen-boot.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "later.elf after a deleted root slot started" "$OUT/counts.txt"   "root=1" line
check "ldir/later.elf after a deleted slot started" "$OUT/counts.txt"   "sub=1" line
check "kernel symbols for the rendezvous found"     "$OUT/symbols.txt"  "found" line
# stop_checks <tag> <label>: every check of one rendezvous (lib/guest-state.sh).
stop_checks() {
    local f="$OUT/state-$1.txt"
    check "$2: stopped at the shell's getkey"            "$f" "PASS $1_rendezvous" line
    check "$2: task list and slots consistent"          "$f" "PASS $1_list_consistent" line
    check "$2: the shell alone"                          "$f" "PASS $1_task_count" line
    check "$2: the shell owns the keyboard"             "$f" "PASS $1_keyboard_owner" line
    [ "$1" = boot ] || check "$2: the same shell task, process and id" "$f" "PASS $1_same_shell" line
}
stop_checks boot "boot"
stop_checks root "later.elf ended"
stop_checks sub  "ldir/later.elf ended"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"  "$OUT/image.txt"  "unchanged" line
finish fat-lookup
