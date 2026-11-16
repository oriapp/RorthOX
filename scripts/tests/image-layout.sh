#!/usr/bin/env bash
#
# bin/os.bin has exactly the FAT16 layout of config.mk, fully backed, and fsck.fat finds nothing on it:
#   sectors   0        boot sector: BPB 512 bytes/sector, 4 sectors/cluster (2 KiB), 200 reserved,
#                      2 FATs, 512 root entries, TotSec16 32,968, TotSec32 0, media 0xF8, 32 sectors
#                      per FAT, CHS 32 sectors/64 heads, no hidden sectors; drive 0x80, signature
#                      0x29, volume ID 0xD105, label "RORTHOX", type "FAT16"
#             1-199    kernel.bin, then zeroes up to sector 200
#             200-231  FAT 1, 232-263 FAT 2: equal, entries 0 and 1 F8 FF FF FF, nothing past
#                      cluster 8,169
#             264-295  root directory: the label entry RORTHOX (attribute 0x08, no cluster, size 0)
#                      in slot 0
#             296-     data: 8,168 clusters (FAT16), up to the end of the file at 16,879,616 bytes
# The boot code around the BPB is pinned (BOOT_CODE_SHA256, bytes 0-2 and 62-511 of boot.bin): a
# change there changes how every machine boots, so it must be a deliberate one; update the value
# with it. The geometry must match an image that mkfs.fat formats with the same values and no
# alignment (only the CHS heads and the volume ID differ: the build keeps heads 64 and its own
# ID). On copies: a one-byte file takes one 2 KiB cluster; 511 files fit in the root next to the
# label and the 512th doesn't, through mtools and through the kernel (filetest.elf fullroot creates
# the 511th file and gets -ENOSPC for the next one), and fsck.fat still finds nothing afterwards.
# bin/os.bin must not change.
#
# Env: BOOT_TIMEOUT=60   RUN_TIMEOUT=120   FSCK_FAT=fsck.fat   MKFS_FAT=mkfs.fat

set -euo pipefail
cd "$(dirname "$0")/../.."
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-120}"
. ./scripts/qemu-lib.sh
. ./scripts/tests/lib/fsck-fat.sh

OUT=${RORTHOX_TEST_ROOT:-build/tests}/image-layout
PROGRAM=programs/filetest/build/filetest.elf
FATIMG="python3 -B scripts/tests/lib/fatimg.py"
BOOT_CODE_SHA256=81bfdaf83c3658699577ab6e7d249aab424339ef705a9ae384e2c5d004bfa307
CLUSTERS=8168
[ -f bin/os.bin ] && [ -f bin/boot.bin ] && [ -f bin/kernel.bin ] && [ -f "$PROGRAM" ] ||
    { echo "image-layout: run 'make all' first" >&2; exit 1; }
hash_before=$(sha256sum bin/os.bin | cut -d' ' -f1)
rm -rf "$OUT"
mkdir -p "$OUT"

echo "image-layout: bin/os.bin"
python3 - bin/os.bin bin/boot.bin bin/kernel.bin > "$OUT/layout.txt" 2>&1 << 'EOF' || true
import hashlib, struct, sys
image, boot, kernel = (open(p, 'rb').read() for p in sys.argv[1:4])
(oem, bps, spc, rsvd, fats, roots, tot16, media, spf, spt, heads, hidden, tot32, drive, res, sig, volid,
 label, fstype) = struct.unpack_from('<8sHBHBHHBHHHIIBBBI11s8s', image, 3)
print('oem=%r' % oem.decode())
print('bytes_per_sector=%d sectors_per_cluster=%d reserved=%d fats=%d root_entries=%d' % (bps, spc, rsvd, fats, roots))
print('total_sectors16=%d total_sectors32=%d media=%#x sectors_per_fat=%d' % (tot16, tot32, media, spf))
print('sectors_per_track=%d heads=%d hidden=%d' % (spt, heads, hidden))
print('drive=%#x reserved_byte=%d signature=%#x volume_id=%#x label=%r type=%r' % (drive, res, sig, volid, label.decode(), fstype.decode()))
print('boot_signature=%s' % image[510:512].hex())
print('image_bytes=%d' % len(image))
total = tot16 or tot32
if total * bps == len(image):
    print('image is exactly as long as the BPB says')
if image[:512] == boot:
    print('sector 0 is bin/boot.bin')
print('boot_code_sha256=%s' % hashlib.sha256(boot[0:3] + boot[62:512]).hexdigest())
if image[512:512 + len(kernel)] == kernel:
    print('kernel.bin from sector 1')
if not any(image[512 + len(kernel):rsvd * bps]):
    print('zeroes from the end of kernel.bin up to sector %d' % rsvd)
fat = [image[(rsvd + i * spf) * bps:(rsvd + (i + 1) * spf) * bps] for i in range(fats)]
root_sector = rsvd + fats * spf
root_sectors = (roots * 32 + bps - 1) // bps
data_sector = root_sector + root_sectors
clusters = (total - data_sector) // spc
print('fat_sectors=%s root_sector=%d data_sector=%d clusters=%d' % (','.join(str(rsvd + i * spf) for i in range(fats)), root_sector, data_sector, clusters))
print('fat_type=%s' % ('FAT12' if clusters < 4085 else 'FAT16' if clusters < 65525 else 'FAT32'))
if (total - data_sector) % spc == 0:
    print('the data area is whole clusters')
if spf * bps // 2 >= clusters + 2:
    print('each FAT holds an entry for every cluster')
if all(f == fat[0] for f in fat):
    print('FAT copies equal')
print('fat_start=%s' % ' '.join(f[:4].hex() for f in fat))
if not any(fat[0][2 * (clusters + 2):]):
    print('no FAT entry past cluster %d' % (clusters + 1))
root = image[root_sector * bps:data_sector * bps]
slot0 = root[:32]
print('slot0 name=%r attr=%#x cluster=%d size=%d' % (slot0[:11].decode('latin-1'), slot0[11], struct.unpack_from('<H', slot0, 26)[0], struct.unpack_from('<I', slot0, 28)[0]))
labels = sum(1 for i in range(0, len(root), 32) if root[i] not in (0, 0xE5) and root[i + 11] & 0x3F == 0x08)
print('label_entries=%d' % labels)
EOF
check "BPB: OEM name"                                 "$OUT/layout.txt" "oem='RORTHOX '" line
check "BPB: 512/sector, 4 sectors/cluster, 200 reserved, 2 FATs, 512 root entries" "$OUT/layout.txt" \
    "bytes_per_sector=512 sectors_per_cluster=4 reserved=200 fats=2 root_entries=512" line
check "BPB: TotSec16 32968, TotSec32 0, media 0xF8, 32 sectors/FAT" "$OUT/layout.txt" \
    "total_sectors16=32968 total_sectors32=0 media=0xf8 sectors_per_fat=32" line
check "BPB: 32 sectors/track, 64 heads, no hidden sectors" "$OUT/layout.txt" "sectors_per_track=32 heads=64 hidden=0" line
check "extended BPB: drive, signature, ID, label, type" "$OUT/layout.txt" \
    "drive=0x80 reserved_byte=0 signature=0x29 volume_id=0xd105 label='RORTHOX    ' type='FAT16   '" line
check "boot sector signature 55 AA"                   "$OUT/layout.txt" "boot_signature=55aa" line
check "image is 16,879,616 bytes"                     "$OUT/layout.txt" "image_bytes=16879616" line
check "image is as long as the BPB's volume"          "$OUT/layout.txt" "image is exactly as long as the BPB says" line
check "sector 0 is bin/boot.bin"                      "$OUT/layout.txt" "sector 0 is bin/boot.bin" line
check "boot code outside the BPB is the pinned code"  "$OUT/layout.txt" "boot_code_sha256=$BOOT_CODE_SHA256" line
check "kernel.bin from sector 1"                      "$OUT/layout.txt" "kernel.bin from sector 1" line
check "kernel area padded with zeroes to sector 200"  "$OUT/layout.txt" "zeroes from the end of kernel.bin up to sector 200" line
check "FATs at 200 and 232, root at 264, data at 296, 8168 clusters" "$OUT/layout.txt" \
    "fat_sectors=200,232 root_sector=264 data_sector=296 clusters=$CLUSTERS" line
check "FAT16 by its cluster count"                    "$OUT/layout.txt" "fat_type=FAT16" line
check "data area is whole clusters"                   "$OUT/layout.txt" "the data area is whole clusters" line
check "each FAT holds every cluster's entry"          "$OUT/layout.txt" "each FAT holds an entry for every cluster" line
check "FAT copies equal"                              "$OUT/layout.txt" "FAT copies equal" line
check "both FATs start F8 FF FF FF"                   "$OUT/layout.txt" "fat_start=f8ffffff f8ffffff" line
check "no FAT entry past the last cluster"            "$OUT/layout.txt" "no FAT entry past cluster 8169" line
check "root slot 0: the label RORTHOX, no cluster, size 0" "$OUT/layout.txt" \
    "slot0 name='RORTHOX    ' attr=0x8 cluster=0 size=0" line
check "one label entry in the root"                   "$OUT/layout.txt" "label_entries=1" line
fsck_clean "fsck.fat finds nothing on bin/os.bin as built" bin/os.bin $CLUSTERS "$OUT/fsck-image.log"

echo "image-layout: against mkfs.fat"
REF="$OUT/mkfs.img"
if "${MKFS_FAT:-mkfs.fat}" -C -a -F 16 -S 512 -s 4 -R 200 -f 2 -r 512 -h 0 -M 0xF8 -n RORTHOX \
        --mbr=no --invariant "$REF" 16484 > "$OUT/mkfs.log" 2>&1; then
    python3 - "$REF" bin/os.bin > "$OUT/mkfs.txt" 2>&1 << 'EOF' || true
import sys
ref, image = (open(p, 'rb').read() for p in sys.argv[1:3])
print('mkfs_bytes=%d' % len(ref))
# Bytes 11-61: BPB and extended BPB. 24-27 are the CHS sectors/track and heads, 39-42 the volume ID.
diff = [i for i in range(11, 62) if ref[i] != image[i]]
print('bpb differs at: %s' % ' '.join(str(i) for i in diff))
if all(24 <= i <= 27 or 39 <= i <= 42 for i in diff):
    print('same BPB but for CHS and volume ID')
if all(ref[s * 512:s * 512 + 4] == image[s * 512:s * 512 + 4] for s in (200, 232)):
    print('same FAT starts at sectors 200 and 232')
if ref[264 * 512:264 * 512 + 12] == image[264 * 512:264 * 512 + 12]:
    print('same label entry at sector 264')
EOF
else
    echo "mkfs.fat failed, see $OUT/mkfs.log" > "$OUT/mkfs.txt"
fi
sed -n 's/^bpb differs/  mkfs.fat BPB differs/p' "$OUT/mkfs.txt"
check "mkfs.fat image as long as bin/os.bin"          "$OUT/mkfs.txt" "mkfs_bytes=16879616" line
check "mkfs.fat BPB the same but for CHS and volume ID" "$OUT/mkfs.txt" "same BPB but for CHS and volume ID" line
check "mkfs.fat FATs where bin/os.bin has them"       "$OUT/mkfs.txt" "same FAT starts at sectors 200 and 232" line
check "mkfs.fat label entry where bin/os.bin has it"  "$OUT/mkfs.txt" "same label entry at sector 264" line
fsck_clean "fsck.fat: the mkfs.fat image has the same 8168 clusters" "$REF" $CLUSTERS "$OUT/fsck-mkfs.log"
rm -f "$REF"

echo "image-layout: a one-byte file"
cp bin/os.bin "$OUT/one.img"
printf 'x' > "$OUT/one.txt"
{
    used() { python3 -c 'import struct, sys
d = open(sys.argv[1], "rb").read(232 * 512)
print(sum(1 for c in range(2, 8170) if struct.unpack_from("<H", d, 200 * 512 + 2 * c)[0]))' "$1"; }
    before=$(used "$OUT/one.img")
    mcopy -i "$OUT/one.img" "$OUT/one.txt" ::ONE.TXT
    $FATIMG entry "$OUT/one.img" ONE.TXT | grep -E '^(size|chain)='
    $FATIMG entry "$OUT/one.img" ONE.TXT | grep -qE '^chain=[0-9]+$' && echo "chain of one cluster"
    $FATIMG info "$OUT/one.img" | grep '^cluster_bytes='
    echo "clusters_taken=$(($(used "$OUT/one.img") - before))"
} > "$OUT/one.txt.log" 2>&1 || true
check "ONE.TXT is 1 byte"                             "$OUT/one.txt.log" "size=1" line
check "ONE.TXT has a one-cluster chain"               "$OUT/one.txt.log" "chain of one cluster" line
check "a cluster is 2048 bytes"                       "$OUT/one.txt.log" "cluster_bytes=2048" line
check "the image has one more cluster in use"         "$OUT/one.txt.log" "clusters_taken=1" line
fsck_clean "fsck.fat finds nothing with ONE.TXT"      "$OUT/one.img" $CLUSTERS "$OUT/fsck-one.log"
rm -f "$OUT/one.img"

# names <first> <count> <dir>: empty files G<first>.TXT ... in <dir> (8.3, upper case: no long names).
names() {
    mkdir -p "$3"
    for ((i = $1; i < $1 + $2; i++)); do : > "$3/$(printf 'G%03d.TXT' "$i")"; done
}

echo "image-layout: root capacity through mtools"
cp bin/os.bin "$OUT/mtools-root.img"
names 1 509 "$OUT/root509"
{
    mcopy -i "$OUT/mtools-root.img" "$OUT"/root509/* :: && echo "509 more files copied"
    echo "root_files=$($FATIMG slots "$OUT/mtools-root.img" | grep -c ' used ')"
    : > "$OUT/H512.TXT"
    if mcopy -i "$OUT/mtools-root.img" "$OUT/H512.TXT" :: > "$OUT/mcopy-512.log" 2>&1; then
        echo "the 512th file was copied"
    else
        echo "the 512th file was refused"
    fi
    mdir -i "$OUT/mtools-root.img" ::H512.TXT > /dev/null 2>&1 || echo "H512.TXT not in the root"
} > "$OUT/mtools-root.txt" 2>&1 || true
check "mtools: 509 files next to shell.elf, blank.elf and the label" "$OUT/mtools-root.txt" "509 more files copied" line
check "mtools: the root holds 511 files"              "$OUT/mtools-root.txt" "root_files=511" line
check "mtools: the 512th file is refused"             "$OUT/mtools-root.txt" "the 512th file was refused" line
check "mtools: the 512th file is not in the root"     "$OUT/mtools-root.txt" "H512.TXT not in the root" line
fsck_clean "fsck.fat finds nothing with a full root"  "$OUT/mtools-root.img" $CLUSTERS "$OUT/fsck-mtools-root.log"
rm -f "$OUT/mtools-root.img"

echo "image-layout: root capacity through the kernel"
# 510 files: shell.elf, blank.elf, FILETEST.ELF, SCRATCH.TXT and 506 more. fullroot creates F00.TXT
# (the 511th) and gets -ENOSPC for F01.TXT.
IMG="$OUT/kernel-root.img"
cp bin/os.bin "$IMG"
printf 'const' > "$OUT/scratch.txt"
names 1 506 "$OUT/root506"
{
    mcopy -i "$IMG" "$PROGRAM" ::FILETEST.ELF && mcopy -i "$IMG" "$OUT/scratch.txt" ::SCRATCH.TXT &&
        mcopy -i "$IMG" "$OUT"/root506/* :: && echo "root_files=$($FATIMG slots "$IMG" | grep -c ' used ')"
} > "$OUT/kernel-root-before.txt" 2>&1 || true
check "kernel: 510 files in the root before the boot" "$OUT/kernel-root-before.txt" "root_files=510" line

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

qemu_start "$IMG" "$OUT/boot"
if grep -qx "root_files=510" "$OUT/kernel-root-before.txt" &&
        wait_screen "${RORTHOX_NAME} v${RORTHOX_VERSION}" "$BOOT_TIMEOUT"; then
    type_line "filetest.elf fullroot"
    wait_line "filetest: end fullroot" "$RUN_TIMEOUT" || true
fi
qemu_stop || true
grep -F 'filetest:' "$OUT/boot/screen.txt" 2> /dev/null | sed 's/^/  /' || true
check "kernel: fullroot passed"                       "$OUT/boot/screen.txt" "filetest: fullroot PASS"
# The whole line (behind an optional prompt), so "created 10" can't pass for "created 1".
grep -Ex '(> )?filetest: fullroot created 1' "$OUT/boot/screen.txt" > "$OUT/fullroot-created.txt" 2> /dev/null || true
check "kernel: the 511th file created, the 512th refused" "$OUT/fullroot-created.txt" "filetest: fullroot created 1"
check "no kernel panic"                               "$OUT/boot/debugcon.log" "PANIC" absent
{
    echo "root_files=$($FATIMG slots "$IMG" | grep -c ' used ')"
    mdir -i "$IMG" ::F00.TXT > /dev/null 2>&1 && echo "F00.TXT in the root"
    mdir -i "$IMG" ::F01.TXT > /dev/null 2>&1 || echo "F01.TXT not in the root"
    [ "$(mtype -i "$IMG" ::SCRATCH.TXT)" = "const+root" ] && echo "SCRATCH.TXT as written"
} > "$OUT/kernel-root.txt" 2>&1 || true
check "kernel: the root holds 511 files"              "$OUT/kernel-root.txt" "root_files=511" line
check "kernel: F00.TXT created"                       "$OUT/kernel-root.txt" "F00.TXT in the root" line
check "kernel: F01.TXT not created"                   "$OUT/kernel-root.txt" "F01.TXT not in the root" line
check "kernel: SCRATCH.TXT written in a full root"    "$OUT/kernel-root.txt" "SCRATCH.TXT as written" line
fsck_clean "fsck.fat finds nothing after the kernel filled the root" "$IMG" $CLUSTERS "$OUT/fsck-kernel-root.log"
rm -f "$IMG"

[ "$(sha256sum bin/os.bin | cut -d' ' -f1)" = "$hash_before" ] && echo unchanged > "$OUT/image.txt" || echo changed > "$OUT/image.txt"
check "bin/os.bin not changed by the test"            "$OUT/image.txt" "unchanged" line
finish image-layout
