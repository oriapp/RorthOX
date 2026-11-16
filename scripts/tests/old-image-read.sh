#!/usr/bin/env bash
#
# The kernel of this build still reads a disk made with the old layout: 64 KiB clusters, 64 root
# entries, a BPB of 7,812,500 sectors on a 16 MiB data area (scripts/tests/lib/legacy-fat-image.py
# history: the image the old Makefile made, without a root label entry), whatever layout bin/os.bin
# has. This must pass before and after a change of the default layout.
#
# The image gets FILETEST.ELF and, made on the host, what "filetest.elf write" leaves behind:
# DATA.BIN (70,000 pattern bytes and "tail\n", two clusters, read-only; its first cluster is a
# hole left by a deleted file and its second comes after GAP.BIN, a cluster of 0xDB bytes, so
# reading the clusters in disk order instead of the chain gives wrong bytes), OLDFILE.TXT,
# CHILD.TXT and SUB/NOTE.TXT. The kernel boots from that image and
# loads the shell from it; "filetest.elf oldread" then reads every file through the 8.3 file
# syscalls (open, read, seek, stat, close) and compares each byte with what was written; a WRITE
# open of DATA.BIN must give -ERDONLY. The image must not change. bin/os.bin must not change.
#
# Env: BOOT_TIMEOUT=60   RUN_TIMEOUT=120

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
. ./scripts/qemu-lib.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/old-image-read
PROGRAM=programs/filetest/build/filetest.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
[ -f bin/os.bin ] && [ -f "$PROGRAM" ] || { echo "old-image-read: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -rf "$OUT"
mkdir -p "$OUT"

IMG="$OUT/old-image.bin"
python3 -B scripts/tests/lib/legacy-fat-image.py history "$IMG" > "$OUT/legacy.txt"
python3 -c 'import sys
open(sys.argv[1], "wb").write(bytes(((i * 7 + i // 251) & 0xFF) for i in range(70000)) + b"tail\n")' "$OUT/data.bin"
printf 'new\n' > "$OUT/oldfile.txt"
printf 'child\n' > "$OUT/child.txt"
printf 'note\n' > "$OUT/note.txt"
head -c 65536 /dev/zero > "$OUT/hole.bin"
head -c 65536 /dev/zero | tr '\0' '\333' > "$OUT/gap.bin"
mcopy -i "$IMG" "$PROGRAM" ::FILETEST.ELF
mcopy -i "$IMG" "$OUT/hole.bin" ::HOLE.BIN
mcopy -i "$IMG" "$OUT/gap.bin" ::GAP.BIN
mdel -i "$IMG" ::HOLE.BIN
mcopy -i "$IMG" "$OUT/data.bin" ::DATA.BIN
mcopy -i "$IMG" "$OUT/oldfile.txt" ::OLDFILE.TXT
mcopy -i "$IMG" "$OUT/child.txt" ::CHILD.TXT
mmd -i "$IMG" ::SUB
mcopy -i "$IMG" "$OUT/note.txt" ::SUB/NOTE.TXT
mattrib -i "$IMG" +r ::DATA.BIN
{
    $FATIMG info "$IMG" | grep -E '^(cluster_bytes|root_entries)='
    $FATIMG entry "$IMG" DATA.BIN | grep '^chain='
    $FATIMG entry "$IMG" GAP.BIN | sed -n 's/^chain=/gap=/p'
    python3 -B scripts/tests/lib/legacy-fat-image.py check "$IMG" > /dev/null && echo "legacy BPB and length"
    mtype -i "$IMG" ::DATA.BIN | cmp -s - "$OUT/data.bin" && echo "DATA.BIN reads back on the host"
} > "$OUT/fixture.txt" 2>&1 || true
chain=$(sed -n 's/^chain=//p' "$OUT/fixture.txt")
gap=$(sed -n 's/^gap=//p' "$OUT/fixture.txt")
echo "  DATA.BIN chain: $chain, GAP.BIN: $gap"
awk -F, -v gap="$gap" 'NF == 2 && gap == $1 + 1 && $2 > gap { print "fragmented" }' <<< "$chain" >> "$OUT/fixture.txt"
check "fixture: 64 KiB clusters"                  "$OUT/fixture.txt" "cluster_bytes=65536" line
check "fixture: 64 root entries"                  "$OUT/fixture.txt" "root_entries=64" line
check "fixture: still the legacy BPB and length"  "$OUT/fixture.txt" "legacy BPB and length" line
check "fixture: DATA.BIN's 2 clusters on both sides of GAP.BIN" "$OUT/fixture.txt" "fragmented" line
check "fixture: DATA.BIN reads back on the host"  "$OUT/fixture.txt" "DATA.BIN reads back on the host" line
image_before=$(sha256sum "$IMG" | cut -d' ' -f1)

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

echo "old-image-read: boot from the old-layout image"
qemu_start "$IMG" "$OUT/boot"
if wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "filetest.elf oldread"
    wait_line "filetest: end oldread" "$RUN_TIMEOUT" || true
fi
qemu_stop || true
grep -F 'filetest:' "$OUT/boot/screen.txt" 2> /dev/null | sed 's/^/  /' || true

check "the shell loaded from the old image"       "$OUT/boot/screen.txt" "${RORTHOX_NAME} v${RORTHOX_VERSION}"
check "every file read back byte for byte"        "$OUT/boot/screen.txt" "filetest: oldread PASS"
check "no kernel panic"                           "$OUT/boot/debugcon.log" "PANIC" absent
[ "$(sha256sum "$IMG" | cut -d' ' -f1)" = "$image_before" ] && echo unchanged > "$OUT/old-image.txt" || echo changed > "$OUT/old-image.txt"
check "the old image not changed by the reads"    "$OUT/old-image.txt" "unchanged" line
[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"        "$OUT/image.txt" "unchanged" line
finish old-image-read
