#!/usr/bin/env bash
#
# One make after a change to a shipped program puts the new program into bin/os.bin, and a make
# that has nothing to do leaves the image alone. Build checks only, no QEMU.
#
# The tracked files are copied to a scratch tree in build/tests and built there with "make all",
# so the checkout's own bin/ and build/ are never touched. Each step runs make once:
#   no-op       nothing changed: os.bin keeps its bytes, inode and time, and a file written into
#               the image beforehand (NOTE.TXT, as the file syscalls would write one) is still there
#   test only   a test program (heaptest) changes: its ELF is rebuilt, os.bin is not
#   shell       shell.c changes: shell.elf and blank.elf copied back out of os.bin (mcopy) are the
#               bytes of the freshly built ELF files, and shell.elf has the change
#   settled     nothing changed after that: the refreshed image is kept (as for no-op)
#   stdlib      the user stdlib changes: both shipped programs in os.bin are the new ones
#   settled     again
#   same bytes  shell.c is touched but not changed: shell.elf is relinked with the same bytes, and
#               os.bin is kept, because only the programs' bytes count
# Every change starts from an image written after the programs were built, the state a make with
# the change missing would leave behind.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check()

OUT=${RORTHOX_TEST_ROOT:-build/tests}/image-refresh
rm -rf "$OUT"
mkdir -p "$OUT"
TREE="$OUT/tree"
RESULTS="$OUT/results.txt"
: > "$RESULTS"
mkdir -p "$TREE"
git rev-parse --is-inside-work-tree > /dev/null 2>&1 || { echo "image-refresh: needs a git checkout" >&2; exit 1; }
# The working tree's tracked bytes, including uncommitted changes.
git ls-files -z | tar --null -T - -cf - | (cd "$TREE" && tar -xf -)
IMG="$TREE/bin/os.bin"
SHELL_ELF="$TREE/programs/shell/build/shell.elf"
BLANK_ELF="$TREE/programs/blank/build/blank.elf"
HEAPTEST_ELF="$TREE/programs/heaptest/build/heaptest.elf"

# run_make <log name>: one "make all" in the scratch tree, without a jobserver or flags of a make
# this test may run under.
step=0
run_make() {
    step=$((step + 1))
    if ! env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u MAKEOVERRIDES make -C "$TREE" all \
        > "$OUT/make-$step-$1.log" 2>&1; then
        echo "image-refresh: make failed, see $OUT/make-$step-$1.log" >&2
        tail -n 20 "$OUT/make-$step-$1.log" >&2
        exit 1
    fi
}
image_id() { stat -c '%i %y' "$IMG"; sha256sum < "$IMG"; }
sum() { sha256sum < "$1"; }
record() { echo "$1" >> "$RESULTS"; }   # check() greps these lines

# image_matches <tag> <name> <built ELF>: the program copied out of os.bin is the built ELF.
image_matches() {
    rm -f "$OUT/extracted"
    if mcopy -i "$IMG" "::$2" "$OUT/extracted" 2> /dev/null && cmp -s "$OUT/extracted" "$3"; then
        record "$1 $2 in image matches"
    fi
}

# preserved <tag>: os.bin is the same file with the same bytes and still holds NOTE.TXT.
preserved() {
    [ "$(image_id)" = "$before" ] && record "$1 image unchanged"
    mtype -i "$IMG" ::NOTE.TXT 2> /dev/null | grep -qxF "note $note" && record "$1 note kept"
    return 0
}

run_make initial
image_matches initial shell.elf "$SHELL_ELF"
image_matches initial blank.elf "$BLANK_ELF"
check "first build: shell.elf in os.bin is the built one" "$RESULTS" "initial shell.elf in image matches" line
check "first build: blank.elf in os.bin is the built one" "$RESULTS" "initial blank.elf in image matches" line

# A file written into the image, which a recreated image would lose.
note=$RANDOM$RANDOM
echo "note $note" > "$OUT/note.txt"
mcopy -i "$IMG" "$OUT/note.txt" ::NOTE.TXT
before=$(image_id)
run_make no-op
preserved no-op
check "no-op make: os.bin keeps its bytes, inode and time" "$RESULTS" "no-op image unchanged" line
check "no-op make: a file written into os.bin survives" "$RESULTS" "no-op note kept" line

old=$(sum "$HEAPTEST_ELF")
printf '\nconst char image_refresh_test[] = "heaptest %s";\n' "$note" >> "$TREE/programs/heaptest/heaptest.c"
run_make test-only
[ "$(sum "$HEAPTEST_ELF")" != "$old" ] && record "test-only heaptest rebuilt"
preserved test-only
check "test program change: heaptest.elf is rebuilt" "$RESULTS" "test-only heaptest rebuilt" line
check "test program change: os.bin keeps its bytes, inode and time" "$RESULTS" "test-only image unchanged" line
check "test program change: the file written into os.bin survives" "$RESULTS" "test-only note kept" line

printf '\nconst char image_refresh_test[] = "shell %s";\n' "$note" >> "$TREE/programs/shell/src/shell.c"
run_make shell
grep -aqF "shell $note" "$SHELL_ELF" && record "shell change built"
image_matches shell shell.elf "$SHELL_ELF"
image_matches shell blank.elf "$BLANK_ELF"
check "shell.c change: the new shell.elf is built" "$RESULTS" "shell change built" line
check "shell.c change, one make: shell.elf in os.bin is the new one" "$RESULTS" "shell shell.elf in image matches" line
check "shell.c change, one make: blank.elf in os.bin is the built one" "$RESULTS" "shell blank.elf in image matches" line

# settled <tag>: a file written into the image, then a make with nothing to do keeps both.
settled() {
    mcopy -o -i "$IMG" "$OUT/note.txt" ::NOTE.TXT
    before=$(image_id)
    run_make "$1"
    preserved "$1"
    check "$1: os.bin keeps its bytes, inode and time" "$RESULTS" "$1 image unchanged" line
    check "$1: the file written into os.bin survives" "$RESULTS" "$1 note kept" line
}
settled settled-shell

printf '\nconst char image_refresh_stdlib[] = "stdlib %s";\n' "$note" >> "$TREE/programs/stdlib/src/start.c"
run_make stdlib
grep -aqF "stdlib $note" "$SHELL_ELF" && grep -aqF "stdlib $note" "$BLANK_ELF" && record "stdlib change built"
image_matches stdlib shell.elf "$SHELL_ELF"
image_matches stdlib blank.elf "$BLANK_ELF"
check "stdlib change: both shipped programs are rebuilt with it" "$RESULTS" "stdlib change built" line
check "stdlib change, one make: shell.elf in os.bin is the new one" "$RESULTS" "stdlib shell.elf in image matches" line
check "stdlib change, one make: blank.elf in os.bin is the new one" "$RESULTS" "stdlib blank.elf in image matches" line
settled settled-stdlib

mcopy -o -i "$IMG" "$OUT/note.txt" ::NOTE.TXT
before=$(image_id)
old_time=$(stat -c %y "$SHELL_ELF")
old=$(sum "$SHELL_ELF")
touch "$TREE/programs/shell/src/shell.c"
run_make same-bytes
[ "$(stat -c %y "$SHELL_ELF")" != "$old_time" ] && [ "$(sum "$SHELL_ELF")" = "$old" ] &&
    record "same-bytes shell relinked"
preserved same-bytes
check "touched shell.c: shell.elf is relinked with the same bytes" "$RESULTS" "same-bytes shell relinked" line
check "touched shell.c: os.bin keeps its bytes, inode and time" "$RESULTS" "same-bytes image unchanged" line
check "touched shell.c: the file written into os.bin survives" "$RESULTS" "same-bytes note kept" line

if [ "$failures" -ne 0 ]; then
    echo "image-refresh: $failures check(s) failed; the scratch tree stays in $TREE" >&2
    exit 1
fi
rm -rf "$TREE"
echo "image-refresh: all checks passed"
