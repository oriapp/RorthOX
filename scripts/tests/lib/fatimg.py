#!/usr/bin/env python3
"""FAT16 fixture helper for the tests in scripts/tests/ (works on copies of bin/os.bin only).

Every value comes from the image's own BPB. Only short (8.3) names in the root directory.

  fatimg.py info <image>                        geometry, one key=value per line
  fatimg.py entry <image> <name>                the root entry of <name>: offset, cluster, size, chain
  fatimg.py set-cluster <image> <name> <n>      set the first cluster of <name> to <n> (low word only)
  fatimg.py relocate <image> <name> <index>     move cluster number <index> of <name>'s chain to a
                                                free cluster after the chain that lies wholly inside
                                                the image file; both FATs are updated the same way,
                                                and the old cluster is freed and filled with 0xDB
  fatimg.py fill-cluster <image> <cluster> <byte>
                                                fill data cluster <cluster> with <byte> (e.g. a
                                                directory with no entries at all, not even . and ..)
  fatimg.py rename <image> <name> <new name>    give the root entry of <name> another 8.3 name; the
                                                new name may already be taken (duplicate fixtures)
  fatimg.py set-high <image> <name> <n>         set the high first-cluster word of <name> (always 0
                                                on FAT16; for reserved-field fixtures)
  fatimg.py set-size <image> <name> <n>         set the file size field of <name>
  fatimg.py slots <image> [<dir>]               the raw 32-byte slots of the root directory (or of the
                                                first cluster of root directory <dir>) up to the first
                                                0x00 slot: "slot=<i> <used|deleted|lfn|label> <name>",
                                                then "used_slots=<n>" and "end=<0x00 slot index|none>"

Exits 1 with a message if anything doesn't look the way it should (bad name, missing entry,
FAT copies that differ, a chain that doesn't match the file size).
"""
import struct
import sys

EOC_MIN = 0xFFF8


class Image:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = bytearray(f.read())
        (self.bytes_per_sector, self.sectors_per_cluster, self.reserved, self.fats, self.root_entries,
         _small, _media, self.sectors_per_fat) = struct.unpack_from('<HBHBHHBH', self.data, 11)
        if self.data[38] != 0x29:
            fail('%s: no FAT16 extended boot signature' % path)
        bps = self.bytes_per_sector
        self.fat_offsets = [(self.reserved + i * self.sectors_per_fat) * bps for i in range(self.fats)]
        self.root_sector = self.reserved + self.fats * self.sectors_per_fat
        self.root_sectors = (self.root_entries * 32 + bps - 1) // bps
        self.data_sector = self.root_sector + self.root_sectors
        self.cluster_bytes = self.sectors_per_cluster * bps
        self.fat_entries = self.sectors_per_fat * bps // 2
        fats = [bytes(self.data[o:o + self.sectors_per_fat * bps]) for o in self.fat_offsets]
        if any(f != fats[0] for f in fats):
            fail('%s: the FAT copies differ' % path)

    def save(self):
        with open(self.path, 'wb') as f:
            f.write(self.data)

    def cluster_offset(self, cluster):
        return (self.data_sector + (cluster - 2) * self.sectors_per_cluster) * self.bytes_per_sector

    def cluster_in_image(self, cluster):
        return 2 <= cluster and self.cluster_offset(cluster) + self.cluster_bytes <= len(self.data)

    def fat_get(self, cluster):
        return struct.unpack_from('<H', self.data, self.fat_offsets[0] + 2 * cluster)[0]

    def fat_set(self, cluster, value):
        for o in self.fat_offsets:
            struct.pack_into('<H', self.data, o + 2 * cluster, value)

    def find(self, name):
        short = short_name(name)
        base = self.root_sector * self.bytes_per_sector
        for i in range(self.root_entries):
            off = base + 32 * i
            first = self.data[off]
            if first == 0x00:
                break
            if first == 0xE5 or self.data[off + 11] & 0x08:
                continue
            if bytes(self.data[off:off + 11]) == short:
                return off
        fail('%s: no root entry for %s' % (self.path, name))

    def entry(self, name):
        off = self.find(name)
        high, low, size = struct.unpack_from('<H', self.data, off + 20)[0], \
            struct.unpack_from('<H', self.data, off + 26)[0], struct.unpack_from('<I', self.data, off + 28)[0]
        return off, high, low, size

    def chain(self, name):
        off, high, cluster, size = self.entry(name)
        if high != 0:
            fail('%s: high cluster word of %s is %#x' % (self.path, name, high))
        chain = []
        while True:
            if cluster < 2 or cluster >= min(self.fat_entries, 0xFFF0) or cluster in chain:
                fail('%s: bad cluster %#x in the chain of %s' % (self.path, cluster, name))
            chain.append(cluster)
            nxt = self.fat_get(cluster)
            if nxt >= EOC_MIN:
                break
            cluster = nxt
        want = max(1, (size + self.cluster_bytes - 1) // self.cluster_bytes)
        if len(chain) != want:
            fail('%s: %s has %d clusters, its size %d needs %d' % (self.path, name, len(chain), size, want))
        return chain


def short_name(name):
    base, _, ext = name.upper().partition('.')
    if not (1 <= len(base) <= 8 and len(ext) <= 3) or '.' in ext:
        fail('%s is not an 8.3 name' % name)
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def fail(msg):
    print('fatimg: ' + msg, file=sys.stderr)
    sys.exit(1)


def main(argv):
    if len(argv) < 2:
        fail('usage: see the top of this file')
    cmd, img = argv[0], Image(argv[1])
    if cmd == 'info':
        for k in ('bytes_per_sector', 'sectors_per_cluster', 'reserved', 'fats', 'root_entries',
                  'sectors_per_fat', 'root_sector', 'root_sectors', 'data_sector', 'cluster_bytes'):
            print('%s=%d' % (k, getattr(img, k)))
        print('image_bytes=%d' % len(img.data))
    elif cmd == 'entry' and len(argv) == 3:
        off, high, low, size = img.entry(argv[2])
        chain = img.chain(argv[2])
        print('entry_offset=%d' % off)
        print('first_cluster=%d' % low)
        print('size=%d' % size)
        print('chain=%s' % ','.join(str(c) for c in chain))
    elif cmd == 'set-cluster' and len(argv) == 4:
        off = img.find(argv[2])
        value = int(argv[3], 0)
        if not 0 <= value <= 0xFFFF:
            fail('cluster %s does not fit 16 bits' % argv[3])
        struct.pack_into('<H', img.data, off + 26, value)
        img.save()
        print('entry_offset=%d' % off)
        print('first_cluster=%d' % value)
        print('first_sector=%d' % (img.data_sector + (value - 2) * img.sectors_per_cluster))
    elif cmd == 'relocate' and len(argv) == 4:
        name, index = argv[2], int(argv[3])
        chain = img.chain(name)
        if not 0 < index < len(chain) - 1:
            fail('%s: cluster %d is not an interior cluster of a %d-cluster chain' % (name, index, len(chain)))
        old = chain[index]
        new = next((c for c in range(max(chain) + 2, img.fat_entries)
                    if img.fat_get(c) == 0 and img.cluster_in_image(c)), None)
        if new is None:
            fail('no free cluster after the chain inside the image file')
        o, n, cb = img.cluster_offset(old), img.cluster_offset(new), img.cluster_bytes
        img.data[n:n + cb] = img.data[o:o + cb]
        img.data[o:o + cb] = b'\xDB' * cb
        img.fat_set(chain[index - 1], new)
        img.fat_set(new, img.fat_get(old))
        img.fat_set(old, 0)
        img.save()
        chain[index] = new
        print('moved=%d->%d' % (old, new))
        print('chain=%s' % ','.join(str(c) for c in Image(argv[1]).chain(name)))
    elif cmd == 'fill-cluster' and len(argv) == 4:
        cluster, byte = int(argv[2], 0), int(argv[3], 0)
        if not img.cluster_in_image(cluster) or not 0 <= byte <= 0xFF:
            fail('cluster %s is not inside the image file, or %s is not a byte' % (argv[2], argv[3]))
        o = img.cluster_offset(cluster)
        img.data[o:o + img.cluster_bytes] = bytes([byte]) * img.cluster_bytes
        img.save()
        print('filled=%d' % cluster)
    elif cmd == 'rename' and len(argv) == 4:
        off, new = img.find(argv[2]), short_name(argv[3])
        img.data[off:off + 11] = new
        img.save()
        print('entry_offset=%d' % off)
    elif cmd in ('set-high', 'set-size') and len(argv) == 4:
        off, value = img.find(argv[2]), int(argv[3], 0)
        if cmd == 'set-high':
            if not 0 <= value <= 0xFFFF:
                fail('%s does not fit 16 bits' % argv[3])
            struct.pack_into('<H', img.data, off + 20, value)
        else:
            if not 0 <= value <= 0xFFFFFFFF:
                fail('%s does not fit 32 bits' % argv[3])
            struct.pack_into('<I', img.data, off + 28, value)
        img.save()
        print('entry_offset=%d' % off)
    elif cmd == 'slots' and len(argv) in (2, 3):
        if len(argv) == 3:
            off, _high, cluster, _size = img.entry(argv[2])
            if not img.data[off + 11] & 0x10 or not img.cluster_in_image(cluster):
                fail('%s is not a directory inside the image file' % argv[2])
            base, count = img.cluster_offset(cluster), img.cluster_bytes // 32
        else:
            base, count = img.root_sector * img.bytes_per_sector, img.root_entries
        used, end = 0, 'none'
        for i in range(count):
            e = img.data[base + 32 * i:base + 32 * i + 32]
            if e[0] == 0x00:
                end = str(i)
                break
            kind = 'deleted' if e[0] == 0xE5 else 'lfn' if e[11] == 0x0F else 'label' if e[11] & 0x08 else 'used'
            used += 1
            name = bytes(e[0:11]).decode('latin-1')
            if e[0] == 0xE5:
                name = '?' + name[1:]
            print('slot=%d %s %s' % (i, kind, name.replace(' ', '_')))
        print('used_slots=%d' % used)
        print('end=%s' % end)
    else:
        fail('usage: see the top of this file')


if __name__ == '__main__':
    main(sys.argv[1:])
