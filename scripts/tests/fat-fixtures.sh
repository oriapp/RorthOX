#!/usr/bin/env bash
#
# The legacy disk fixtures and the fsck.fat helper the FAT tests use. No QEMU.
#
# scripts/tests/lib/legacy-fat-image.py builds the old layout (64 KiB clusters, 64 root entries,
# a 7,812,500-sector BPB on a 16 MiB data area) from bin/boot.bin and bin/kernel.bin:
#   history   exactly the BPB and length of that layout, the boot code and kernel of this build,
#             equal FATs, shell.elf and blank.elf read-only and byte for byte; no root label, so
#             fsck.fat gives the old label finding and exit 1 (pinned, not "clean")
#   labelled  the same with a root label entry RORTHOX: fsck.fat finds nothing
# scripts/tests/lib/fsck-fat.sh must call an image clean only on exit 0 with nothing but the
# version and summary lines and the expected cluster count, and must pin the exit status and
# every finding of a fixture damaged on purpose. Fixtures (copies of "labelled"): a long name
# whose checksum no longer matches (fsck.fat prints it but exits 0), an orphaned long name, FAT
# copies that differ, a lost cluster, and a BPB with fewer sectors (another cluster count).
# The helper must refuse: a missing fsck.fat, the exit-0 finding, a checker that exits nonzero,
# writes to stderr or prints another version line around normal output, wrong pinned findings
# or exit, and the other geometry. The builder must refuse a boot sector made for another
# reserved area (it would load the legacy FATs as kernel), and a TERM while fsck.fat runs must
# remove the helper's copy and then run the caller's own trap. bin/os.bin must not change.

set -euo pipefail
cd "$(dirname "$0")/../.."
. ./scripts/qemu-lib.sh   # only for check() and finish()
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/fat-fixtures
LEGACY="python3 -B scripts/tests/lib/legacy-fat-image.py"
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
SHELL_ELF=programs/shell/build/shell.elf
BLANK_ELF=programs/blank/build/blank.elf
[ -f bin/os.bin ] && [ -f "$SHELL_ELF" ] && [ -f "$BLANK_ELF" ] || { echo "fat-fixtures: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -rf "$OUT"
mkdir -p "$OUT"
CLUSTERS=61029

echo "fat-fixtures: legacy images"
$LEGACY history "$OUT/history.img" > "$OUT/history-geometry.txt"
$LEGACY labelled "$OUT/labelled.img" > "$OUT/labelled-geometry.txt"
{
    cmp -s <(head -c 3 bin/boot.bin; tail -c +63 bin/boot.bin) <(head -c 3 "$OUT/history.img"; head -c 512 "$OUT/history.img" | tail -c +63) &&
        echo "boot code of bin/boot.bin"
    cmp -s bin/kernel.bin <(tail -c +513 "$OUT/history.img" | head -c "$(stat -c %s bin/kernel.bin)") && echo "kernel of bin/kernel.bin"
    python3 - "$OUT/history.img" << 'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read(64)
f = struct.unpack_from('<HBHBHHBHHHII', d, 11)
print('bpb=%d,%d,%d,%d,%d,%d,%#x,%d,%d,%d,%d,%d label=%s' % (f + (d[43:54].decode(),)))
EOF
    for f in "$SHELL_ELF" "$BLANK_ELF"; do
        n=$(basename "$f")
        mcopy -n -i "$OUT/history.img" "::$n" "$OUT/got-$n" && cmp -s "$OUT/got-$n" "$f" && echo "$n as built"
        mattrib -i "$OUT/history.img" "::$n" | grep -q '^ *A *R *::/'"$n"'$' && echo "$n read-only"
    done
    $FATIMG slots "$OUT/history.img" | sed 's/^/history /'
    $FATIMG slots "$OUT/labelled.img" | sed 's/^/labelled /'
} > "$OUT/layout.txt" 2>&1 || true
check "history: legacy BPB (128-sector clusters, 64 root entries, 7812500 sectors)" "$OUT/layout.txt" \
    "bpb=512,128,200,2,64,0,0xf8,256,32,64,0,7812500 label=RORTHOX    " line
check "history: 61029 clusters on a 16 MiB data area"   "$OUT/history-geometry.txt" "clusters=$CLUSTERS" line
check "history: boot sector + kernel + 16 MiB long"     "$OUT/history-geometry.txt" "image_bytes=$((512 + $(stat -c %s bin/kernel.bin) + 16777216))" line
check "history: boot code from bin/boot.bin"            "$OUT/layout.txt" "boot code of bin/boot.bin" line
check "history: kernel from bin/kernel.bin"             "$OUT/layout.txt" "kernel of bin/kernel.bin" line
check "history: shell.elf as built"                     "$OUT/layout.txt" "shell.elf as built" line
check "history: blank.elf as built"                     "$OUT/layout.txt" "blank.elf as built" line
check "history: shell.elf read-only"                    "$OUT/layout.txt" "shell.elf read-only" line
check "history: blank.elf read-only"                    "$OUT/layout.txt" "blank.elf read-only" line
check "history: no root label entry"                    "$OUT/layout.txt" "history used_slots=2" line
check "labelled: root label entry RORTHOX after them"   "$OUT/layout.txt" "labelled slot=2 label RORTHOX____" line
check "labelled: 61029 clusters"                        "$OUT/labelled-geometry.txt" "clusters=$CLUSTERS" line

echo "fat-fixtures: fsck.fat on the legacy images"
cat > "$OUT/label.diag" << 'EOF'
Label in boot sector is 'RORTHOX', but there is no volume label in root directory.
  Auto-removing label from boot sector.

Leaving filesystem unchanged.
EOF
fsck_expect "history: fsck.fat gives exit 1 and only the label finding" "$OUT/history.img" $CLUSTERS 1 "$OUT/history.log" "$OUT/label.diag"
fsck_clean "labelled: fsck.fat finds nothing" "$OUT/labelled.img" $CLUSTERS "$OUT/labelled.log"

# Damaged copies of the labelled image. Root slots: 0 shell.elf, 1 blank.elf, 2 the label,
# 3-4 the long name's entries, 5 its short entry LONGFI~1.TXT (cluster 4, after shell.elf/blank.elf).
printf 'hello\n' > "$OUT/hello.txt"
cp "$OUT/labelled.img" "$OUT/lfn.img"
mcopy -i "$OUT/lfn.img" "$OUT/hello.txt" "::Long File Name.txt"
damage() {
    python3 - "$@" << 'EOF'
import struct, sys
src, out, what = sys.argv[1:]
d = bytearray(open(src, 'rb').read())
bps, spc, rsvd, fats, roots, _t, _m, spf = struct.unpack_from('<HBHBHHBH', d, 11)
root = (rsvd + fats * spf) * bps
fat = lambda i, c: (rsvd + i * spf) * bps + 2 * c
if what == 'checksum':      # both long name entries: the short entry's checksum no longer matches
    for s in (3, 4):
        d[root + 32 * s + 13] ^= 0x5A
elif what == 'orphan':      # short entry deleted, long name entries and cluster left
    d[root + 32 * 5] = 0xE5
elif what == 'mirror':      # cluster 40 marked end of chain in FAT 2 only
    d[fat(1, 40):fat(1, 40) + 2] = b'\xff\xff'
elif what == 'lost':        # cluster 40 allocated in both FATs, owned by no file
    for i in (0, 1):
        d[fat(i, 40):fat(i, 40) + 2] = b'\xff\xff'
elif what == 'geometry':    # 7,800,000 sectors: a valid volume of 60931 clusters
    struct.pack_into('<I', d, 32, 7800000)
open(out, 'wb').write(d)
EOF
}
damage "$OUT/lfn.img" "$OUT/checksum.img" checksum
damage "$OUT/lfn.img" "$OUT/orphan.img" orphan
damage "$OUT/labelled.img" "$OUT/mirror.img" mirror
damage "$OUT/labelled.img" "$OUT/lost.img" lost
damage "$OUT/labelled.img" "$OUT/geometry.img" geometry
cat > "$OUT/checksum.diag" << 'EOF'
Wrong checksum for long file name "Long File Name.txt".
  (Short name LONGFI~1.TXT may have changed without updating the long name)
  Not auto-correcting this.
EOF
cat > "$OUT/orphan.diag" << 'EOF'
Orphaned long file name part "Long File Name.txt"
  Auto-deleting.
Reclaimed 1 unused cluster (65536 bytes).

Leaving filesystem unchanged.
EOF
cat > "$OUT/mirror.diag" << 'EOF'
FATs differ but appear to be intact.
  Using first FAT.

Leaving filesystem unchanged.
EOF
cat > "$OUT/lost.diag" << 'EOF'
Reclaimed 1 unused cluster (65536 bytes).

Leaving filesystem unchanged.
EOF
fsck_clean  "long name with a valid checksum: clean"                     "$OUT/lfn.img" $CLUSTERS "$OUT/lfn.log"
fsck_expect "wrong long name checksum: exit 0 with its finding"          "$OUT/checksum.img" $CLUSTERS 0 "$OUT/checksum.log" "$OUT/checksum.diag"
fsck_expect "orphaned long name: exit 1, orphan and lost cluster"        "$OUT/orphan.img" $CLUSTERS 1 "$OUT/orphan.log" "$OUT/orphan.diag"
fsck_expect "FAT copies differ: exit 1 with that finding"                "$OUT/mirror.img" $CLUSTERS 1 "$OUT/mirror.log" "$OUT/mirror.diag"
fsck_expect "lost cluster: exit 1 with that finding"                     "$OUT/lost.img" $CLUSTERS 1 "$OUT/lost.log" "$OUT/lost.diag"

echo "fat-fixtures: the helper refuses what isn't clean (each must FAIL)"
# A stand-in checker: the normal two lines (STUB_VERSION replaces the first), then STUB_EXIT and STUB_ERR.
cat > "$OUT/stub-fsck" << 'EOF'
#!/bin/sh
echo "${STUB_VERSION:-fsck.fat 4.2 (2021-01-31)}"
echo "$2: 3 files, 2/61029 clusters"
[ -z "${STUB_ERR:-}" ] || echo "$STUB_ERR" >&2
exit "${STUB_EXIT:-0}"
EOF
chmod +x "$OUT/stub-fsck"
# refused <tag> <reason> [VAR=value...] <helper call>: in a subshell with those variables, the
# call reports FAIL and its log's verdict starts with <reason>. The call logs to $OUT/refused-<tag>.log.
refused() {
    local tag="$1" reason="$2"
    shift 2
    (
        failures=0
        while [[ "$1" == *=* ]]; do export "${1?}"; shift; done
        "$@"
    ) > "$OUT/refused-$tag.txt" 2>&1 || true
    { grep -q '^  FAIL ' "$OUT/refused-$tag.txt" && grep -q "^verdict: $reason" "$OUT/refused-$tag.log" && echo refused; } \
        > "$OUT/refused-$tag.result" 2>&1 || true
    check "helper refuses $tag ($reason)" "$OUT/refused-$tag.result" "refused" line
}
refused missing-fsck "fsck.fat not found" FSCK_FAT="$OUT/no-fsck" \
    fsck_clean x "$OUT/labelled.img" $CLUSTERS "$OUT/refused-missing-fsck.log"
refused exit0-finding "findings differ" \
    fsck_clean x "$OUT/checksum.img" $CLUSTERS "$OUT/refused-exit0-finding.log"
refused checker-exit4 "exit status 4, expected 0" FSCK_FAT="$OUT/stub-fsck" STUB_EXIT=4 \
    fsck_clean x "$OUT/labelled.img" $CLUSTERS "$OUT/refused-checker-exit4.log"
refused checker-stderr "output on stderr" FSCK_FAT="$OUT/stub-fsck" STUB_ERR="Failed to read sector 7812499." \
    fsck_clean x "$OUT/labelled.img" $CLUSTERS "$OUT/refused-checker-stderr.log"
refused checker-version "no version line" FSCK_FAT="$OUT/stub-fsck" STUB_VERSION="dosfsck 2.11, 12 Mar 2005, FAT32, LFN" \
    fsck_clean x "$OUT/labelled.img" $CLUSTERS "$OUT/refused-checker-version.log"
refused other-findings "findings differ" \
    fsck_expect x "$OUT/lost.img" $CLUSTERS 1 "$OUT/refused-other-findings.log" "$OUT/label.diag"
refused other-exit "exit status 1, expected 0" \
    fsck_expect x "$OUT/lost.img" $CLUSTERS 0 "$OUT/refused-other-exit.log" "$OUT/lost.diag"
refused geometry "summary is not" \
    fsck_clean x "$OUT/geometry.img" $CLUSTERS "$OUT/refused-geometry.log"
# The same image is clean for its own cluster count, and legacy-fat-image.py refuses its BPB.
fsck_clean "the 7,800,000-sector copy is clean as a 60931-cluster volume" "$OUT/geometry.img" 60931 "$OUT/geometry.log"
{ $LEGACY check "$OUT/geometry.img" > /dev/null 2>&1 && echo accepted || echo refused; } > "$OUT/geometry-check.txt"
check "legacy-fat-image.py check refuses the other BPB" "$OUT/geometry-check.txt" "refused" line

echo "fat-fixtures: the builder refuses a boot sector made for another reserved area"
# build() with bin/boot.bin as built, with ReservedSectors 208, and loading 207 kernel sectors.
python3 -B - "$OUT/boot-build.img" > "$OUT/boot-check.txt" 2>&1 << 'EOF' || true
import contextlib, importlib.util, io, struct, sys
spec = importlib.util.spec_from_file_location('legacy', 'scripts/tests/lib/legacy-fat-image.py')
legacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legacy)
read = legacy.read
boot = read('bin/boot.bin')
n = boot.find(bytes([0xb8, 1, 0, 0, 0, 0xb9])) + 6
for tag, at, value in (('as built', None, None), ('reserved=208', 14, struct.pack('<H', 208)),
                       ('kernel=207', n, struct.pack('<I', 207))):
    b = bytearray(boot)
    if at is not None:
        b[at:at + len(value)] = value
    legacy.read = lambda path, b=bytes(b): b if path == 'bin/boot.bin' else read(path)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            legacy.build('history', sys.argv[1])
        print('boot %s accepted' % tag)
    except SystemExit:
        print('boot %s refused' % tag)
EOF
check "builder takes this build's boot sector"          "$OUT/boot-check.txt" "boot as built accepted" line
check "builder refuses 208 reserved sectors"           "$OUT/boot-check.txt" "boot reserved=208 refused" line
check "builder refuses a boot sector loading 207 sectors" "$OUT/boot-check.txt" "boot kernel=207 refused" line

echo "fat-fixtures: the fsck copy is removed on TERM, and the caller's traps stay"
# term-driver.sh sets its own TERM trap, makes one normal check (its traps must be the same
# afterwards), then one with a checker that never ends; it gets TERM (its whole process group)
# while that checker runs: the copy must be gone and the driver's own trap must have run.
mkdir -p "$OUT/tmp"
cat > "$OUT/hang-fsck" << 'EOF'
#!/bin/sh
: > "$HANG_STARTED"
exec sleep 600
EOF
chmod +x "$OUT/hang-fsck"
cat > "$OUT/term-driver.sh" << 'EOF'
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/fsck-fat.sh
failures=0
trap 'echo "caller trap ran" >> "$TRAPLOG"; exit 7' TERM
term=$(trap -p TERM)
int=$(trap -p INT)
fsck_clean x "$OUT/labelled.img" 61029 "$OUT/term-1.log" > /dev/null
[ "$(trap -p TERM)" = "$term" ] && [ "$(trap -p INT)" = "$int" ] && echo "traps restored" >> "$TRAPLOG"
FSCK_FAT="$OUT/hang-fsck" fsck_clean x "$OUT/labelled.img" 61029 "$OUT/term-2.log" > /dev/null
echo "not reached" >> "$TRAPLOG"
EOF
: > "$OUT/term-trap.txt"
rm -f "$OUT/hang-started"
OUT="$OUT" TRAPLOG="$OUT/term-trap.txt" HANG_STARTED="$OUT/hang-started" TMPDIR="$OUT/tmp" \
    setsid bash "$OUT/term-driver.sh" > "$OUT/term-driver.out" 2>&1 &
driver=$!
for ((i = 0; i < 200; i++)); do [ -f "$OUT/hang-started" ] && break; sleep 0.1; done
{
    [ -f "$OUT/hang-started" ] && echo "checker started"
    echo "copies while it runs: $(find "$OUT/tmp" -name check.img | wc -l)"
    [ "$(ps -o pgid= -p "$driver" | tr -d ' ')" = "$driver" ] && echo "driver leads its group"
} > "$OUT/term.txt"
kill -TERM -- -"$driver" 2> /dev/null || kill -TERM "$driver" 2> /dev/null || true
if wait "$driver"; then s=0; else s=$?; fi
{
    echo "driver exit=$s"
    echo "left in TMPDIR: $(find "$OUT/tmp" -mindepth 1 | wc -l)"
} >> "$OUT/term.txt"
check "normal check: the caller's TERM and INT traps unchanged" "$OUT/term-trap.txt" "traps restored" line
check "hanging checker started with its copy in TMPDIR"      "$OUT/term.txt" "copies while it runs: 1" line
check "driver got TERM as a process group"                  "$OUT/term.txt" "driver leads its group" line
check "TERM: the copy is removed"                           "$OUT/term.txt" "left in TMPDIR: 0" line
check "TERM: then the caller's own trap ran"                "$OUT/term-trap.txt" "caller trap ran" line
check "TERM: with the caller's exit status"                 "$OUT/term.txt" "driver exit=7" line
check "TERM: the script did not go on"                      "$OUT/term-trap.txt" "not reached" absent

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test" "$OUT/image.txt" "unchanged" line
if [ "$failures" -ne 0 ]; then
    echo "fat-fixtures: $failures check(s) failed" >&2
    exit 1
fi
echo "fat-fixtures: all checks passed"
