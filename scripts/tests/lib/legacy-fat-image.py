#!/usr/bin/env python3
"""The legacy RorthOX disk layout, made explicitly, for the tests that depend on it.

The FAT tests were written for the layout bin/os.bin had up to now: 512-byte sectors, 64 KiB
clusters (128 sectors), 200 reserved sectors (boot sector + kernel), 2 FATs of 256 sectors, a
root directory of 64 entries and a BPB of 7,812,500 sectors (61,029 clusters, FAT16), while the
file is only the boot sector, the kernel and 16 MiB. The image is built here from that recipe
(the same steps the Makefile used), whatever layout bin/os.bin has, so those tests keep checking
the same cases.

  legacy-fat-image.py history <out>     the image the old Makefile recipe gives: bin/boot.bin with
                                        the legacy BPB, bin/kernel.bin, 16 MiB of zeroes, FAT
                                        entries 0 and 1 set, shell.elf and blank.elf copied in by
                                        mtools and made read-only. The BPB has the label RORTHOX
                                        but the root has no label entry (fsck.fat reports that).
  legacy-fat-image.py labelled <out>    the same, plus the root label entry RORTHOX (mlabel), so
                                        fsck.fat finds nothing on the unchanged image: for tests
                                        that write and then expect a clean volume.
  legacy-fat-image.py check <image>     exit 1 unless the image has the legacy BPB and length.

Run from the repository root after 'make all'. Prints the geometry as key=value lines. The boot
sector comes from this build, so it must still load 199 kernel sectors and say 200 reserved
sectors: the builder refuses otherwise (with more, the legacy boot sector would read the legacy
FATs and root directory into memory after the kernel).
"""
import os
import struct
import subprocess
import sys

SECTOR = 512
RESERVED = 200
SECTORS_PER_FAT = 256
TOTAL_SECTORS = 7812500
CLUSTERS = 61029
DATA_BYTES = 16 * 1024 * 1024
LABEL = b'RORTHOX    '
SHIPPED = ('programs/shell/build/shell.elf', 'programs/blank/build/blank.elf')

# The legacy BPB and extended BPB, bytes 3-61 of the boot sector.
BPB = (b'RORTHOX ' + struct.pack('<HBHBHHBHHHII', SECTOR, 0x80, RESERVED, 2, 64, 0, 0xF8,
                                 SECTORS_PER_FAT, 0x20, 0x40, 0, TOTAL_SECTORS)
       + struct.pack('<BBBI', 0x80, 0, 0x29, 0xD105) + LABEL + b'FAT16   ')


def fail(msg):
    print('legacy-fat-image: ' + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError as e:
        fail('%s (run make all first)' % e)


def run(*cmd):
    if subprocess.run(cmd).returncode != 0:
        fail('%s failed' % ' '.join(cmd))


def check(path):
    data = read(path)
    kernel = len(read('bin/kernel.bin'))
    if len(data) < 512 or data[3:62] != BPB or data[510:512] != b'\x55\xaa':
        fail('%s: not the legacy BPB' % path)
    if len(data) != SECTOR + kernel + DATA_BYTES:
        fail('%s: %d bytes, the legacy recipe gives %d' % (path, len(data), SECTOR + kernel + DATA_BYTES))
    fat1 = data[RESERVED * SECTOR:(RESERVED + SECTORS_PER_FAT) * SECTOR]
    fat2 = data[(RESERVED + SECTORS_PER_FAT) * SECTOR:(RESERVED + 2 * SECTORS_PER_FAT) * SECTOR]
    if fat1 != fat2 or fat1[:4] != b'\xf8\xff\xff\xff':
        fail('%s: FAT copies differ or entries 0 and 1 are wrong' % path)
    print('clusters=%d' % CLUSTERS)
    print('total_bytes=%d' % (TOTAL_SECTORS * SECTOR))
    print('image_bytes=%d' % len(data))


def boot_kernel_sectors(boot):
    """The kernel sector count the boot code loads: mov eax, 1 (first sector), then mov ecx, n."""
    at = boot.find(b'\xb8\x01\x00\x00\x00\xb9')
    if at < 0 or boot.find(b'\xb8\x01\x00\x00\x00\xb9', at + 1) >= 0:
        return None
    return struct.unpack_from('<I', boot, at + 6)[0]


def check_boot(boot):
    if len(boot) != SECTOR or boot[510:512] != b'\x55\xaa':
        fail('bin/boot.bin is not a boot sector')
    if struct.unpack_from('<H', boot, 14)[0] != RESERVED or boot_kernel_sectors(boot) != RESERVED - 1:
        fail('bin/boot.bin says %s reserved sectors and loads %s kernel sectors, the legacy layout '
             'needs %d and %d' % (struct.unpack_from('<H', boot, 14)[0], boot_kernel_sectors(boot),
                                  RESERVED, RESERVED - 1))


def build(variant, out):
    boot, kernel = read('bin/boot.bin'), read('bin/kernel.bin')
    check_boot(boot)
    if len(kernel) > (RESERVED - 1) * SECTOR:
        fail('bin/kernel.bin does not fit in the %d legacy reserved sectors' % RESERVED)
    image = bytearray(boot[:3] + BPB + boot[62:] + kernel + bytes(DATA_BYTES))
    for fat in (RESERVED, RESERVED + SECTORS_PER_FAT):
        image[fat * SECTOR:fat * SECTOR + 4] = b'\xf8\xff\xff\xff'
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(image)
    os.replace(tmp, out)
    run('mcopy', '-i', out, *SHIPPED, '::')
    run('mattrib', '-i', out, '+r', *('::' + os.path.basename(p) for p in SHIPPED))
    if variant == 'labelled':
        run('mlabel', '-i', out, '::' + LABEL.decode().strip())
    check(out)


def main(argv):
    if len(argv) == 2 and argv[0] in ('history', 'labelled'):
        build(argv[0], argv[1])
    elif len(argv) == 2 and argv[0] == 'check':
        check(argv[1])
    else:
        fail('usage: see the top of this file')


if __name__ == '__main__':
    main(sys.argv[1:])
