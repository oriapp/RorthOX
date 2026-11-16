#!/usr/bin/env python3
"""Disk images for fat-vfat-read.sh: VFAT long names made by mtools, and copies damaged on purpose.

  vfat-fixtures.py <out dir>     (from the repository root, after 'make all')

Valid images (mtools writes every name; this script only reads them back to find the entries):
  legacy.img    legacy-fat-image.py labelled (64 KiB clusters, 64 root entries, root label
                RORTHOX) plus long names of 13, 26 and 255 characters (the 255 one crosses a
                root sector), case variants and a long-named directory tree.
  foreign.img   mkfs.fat with 512-byte clusters and the label RORTHOX: the same kinds of names,
                and 'Split directory', whose 3-cluster chain is not contiguous: the long name
                'Crosses a cluster edge.txt' starts in the last slot of its first cluster and ends
                in the next one, a 200-character name crosses into its third cluster (a
                255-character name fits in a 259-character path only in the root).

Damaged copies of foreign.img: each changes a few bytes of one long-name entry set (DAMAGED
below says which and why) or adds crafted records (a directory with a bad first cluster, two
records with one name, a long name whose short entry was deleted, odd short names, a long name
on a label). A damaged long name must never be found, not even a prefix of it, while its short
alias still opens the file.

Writes into <out dir>:
  *.img                  the images
  payload/<n>            the bytes each file holds (the host checks compare them)
  cases.txt              one lookup per line, tab-separated: <kind> <image> <path> <expected>
                         (kinds and expectations: see scripts/tests/host/fat-vfat-read.c)
  sets/<n>.bin, .name    raw long-name entry sets plus their alias entry, and the name mtools
                         was given (for the decoder checks in fat-vfat-codec.c); sets/damaged.txt
                         lists the damaged copies of the 255-character set
  listing-<n>.txt        the records a scan of a directory must give, in order
  fsck.txt               <image> <clusters> <exit> <diagnostics file or ->: what fsck.fat -n
                         must say about each image
"""
import os
import shutil
import struct
import subprocess
import sys

OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)   # the 13 UCS-2 units of an LFN entry
CHARS = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
N13 = 'Thirteen.text'
N26 = 'Twenty-six characters.text'
N255 = (CHARS * 5)[:251] + '.txt'
N200 = (CHARS * 4)[:196] + '.txt'
CROSS = 'Crosses a cluster edge.txt'                  # 26 characters
SPLIT = 'Split directory'
assert len(N13) == 13 and len(N26) == 26 and len(N255) == 255 and len(N200) == 200 and len(CROSS) == 26


def fail(msg):
    print('vfat-fixtures: ' + msg, file=sys.stderr)
    sys.exit(1)


def run(*cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        fail('%s failed: %s' % (' '.join(cmd), r.stderr.strip()))
    return r.stdout


class Image:
    """Reads the geometry and directories of a FAT16 image file (enough to find entries)."""

    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        self.bps = struct.unpack_from('<H', d, 11)[0]
        self.spc = d[13]
        res = struct.unpack_from('<H', d, 14)[0]
        self.fat = res * self.bps
        spf = struct.unpack_from('<H', d, 22)[0]
        self.root = (res + d[16] * spf) * self.bps
        self.root_entries = struct.unpack_from('<H', d, 17)[0]
        self.data_off = self.root + self.root_entries * 32
        total = struct.unpack_from('<H', d, 19)[0] or struct.unpack_from('<I', d, 32)[0]
        self.clusters = (total - (self.data_off // self.bps)) // self.spc
        self.cluster_bytes = self.bps * self.spc

    def chain(self, first):
        out = [first]
        while True:
            nxt = struct.unpack_from('<H', self.data, self.fat + out[-1] * 2)[0]
            if nxt >= 0xFFF8:
                return out
            out.append(nxt)

    def slots(self, cluster):
        """Byte offsets of every slot of a directory (cluster 0 = the root)."""
        if cluster == 0:
            return [self.root + 32 * i for i in range(self.root_entries)]
        out = []
        for c in self.chain(cluster):
            base = self.data_off + (c - 2) * self.cluster_bytes
            out += [base + 32 * i for i in range(self.cluster_bytes // 32)]
        return out

    def records(self, cluster):
        """(long name or None, alias offset, [LFN entry offsets]) for each live short entry."""
        out, lfn = [], []
        for off in self.slots(cluster):
            e = self.data[off:off + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
            elif e[11] == 0x0F:
                lfn.append(off)
            else:
                name = None
                if lfn:
                    units = []
                    for o in reversed(lfn):
                        units += [struct.unpack_from('<H', self.data, o + i)[0] for i in OFFSETS]
                    name = ''.join(chr(u) for u in units).split('\0')[0]
                out.append((name, off, lfn))
                lfn = []
        return out

    def find(self, cluster, name):
        hits = [r for r in self.records(cluster) if r[0] == name or self.alias(r[1]) == name.upper()]
        if len(hits) != 1:
            fail('%s: %r found %d times' % (self.path, name, len(hits)))
        return hits[0]

    def alias(self, off):
        base = self.data[off:off + 8].decode('latin-1').rstrip(' ')
        ext = self.data[off + 8:off + 11].decode('latin-1').rstrip(' ')
        return base + ('.' + ext if ext else '')

    def shown(self, off):
        """The alias of an entry mtools wrote (ASCII only), with its NTRes lowercase bits applied."""
        base = self.data[off:off + 8].decode('ascii').rstrip(' ')
        ext = self.data[off + 8:off + 11].decode('ascii').rstrip(' ')
        base = base.lower() if self.data[off + 12] & 0x08 else base
        ext = ext.lower() if self.data[off + 12] & 0x10 else ext
        return base + ('.' + ext if ext else '')

    def first_cluster(self, off):
        return struct.unpack_from('<H', self.data, off + 26)[0]

    def free_root_slot(self):
        for off in self.slots(0):
            if self.data[off] == 0:
                return off
        fail('%s: root is full' % self.path)

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(self.data)


def checksum(alias11):
    s = 0
    for c in alias11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, alias11):
    """A long-name entry set for <name>, written here only for the crafted AMBIGUOUS records."""
    units = [ord(c) for c in name]
    if len(units) % 13:
        units += [0] + [0xFFFF] * (12 - len(units) % 13)
    n = len(units) // 13
    out = b''
    for k in range(n, 0, -1):
        e = bytearray(32)
        e[0] = k | (0x40 if k == n else 0)
        e[11], e[13] = 0x0F, checksum(alias11)
        for i, o in enumerate(OFFSETS):
            struct.pack_into('<H', e, o, units[(k - 1) * 13 + i])
        out += bytes(e)
    return out


def short_entry(alias11, attr=0x20, ntres=0, cluster=0, size=0):
    e = bytearray(32)
    e[0:11], e[11], e[12] = alias11, attr, ntres
    struct.pack_into('<HI', e, 26, cluster, size)
    return bytes(e)


class Builder:
    def __init__(self, out):
        self.out = out
        self.cases = []
        self.fsck = []
        self.payloads = 0
        for d in ('payload', 'sets'):
            os.makedirs(os.path.join(out, d), exist_ok=True)

    def path(self, name):
        return os.path.join(self.out, name)

    def payload(self, size, seed):
        """A new payload file whose bytes depend on its number and position."""
        self.payloads += 1
        data = bytes((seed * 31 + i * 7 + (i >> 9)) & 0xFF for i in range(size))
        p = self.path('payload/%d' % self.payloads)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def case(self, kind, image, path, expect):
        self.cases.append('\t'.join((kind, image, path, expect)))

    def copy_in(self, image, size, seed, dest):
        p = self.payload(size, seed)
        run('mcopy', '-i', self.path(image), p, '::' + dest)
        return p


def build_legacy(b):
    img = b.path('legacy.img')
    run(sys.executable, '-B', 'scripts/tests/lib/legacy-fat-image.py', 'labelled', img)
    run('mmd', '-i', img, '::Long directory name', '::Long directory name/Nested Dir')
    files = {}
    for i, name in enumerate((N13, N26, N255, 'Mixed Case.Txt', 'lower.txt', 'MiX.TXT',
                              'a+b [1];x=y,z.txt', 'archive.tar.gz',
                              'Long directory name/Inner file with a long name.data',
                              'Long directory name/Nested Dir/deep.txt')):
        files[name] = b.copy_in('legacy.img', 100 + 37 * i, i, name)
    return Image(img), files


def build_foreign(b):
    img = b.path('foreign.img')
    with open(img, 'wb') as f:
        f.truncate(16 * 1024 * 1024)
    run('mkfs.fat', '-F', '16', '-s', '1', '-S', '512', '-r', '512', '-n', 'RORTHOX', '--invariant', img)
    files = {}
    run('mmd', '-i', img, '::' + SPLIT)                                # its first cluster: 2
    for i, name in enumerate((N13, N26, N255, 'SHORT.TXT', 'low.TXT', 'Mixed Case.Txt')):
        files[name] = b.copy_in('foreign.img', 700 + 211 * i, 40 + i, name)
    empty = b.payload(0, 0)
    for i in range(13):                                                # slots 2-14 of cluster 2
        run('mcopy', '-i', img, empty, '::%s/F%02d.TXT' % (SPLIT, i))
    # Slot 15 of cluster 2 takes the LAST entry, the directory grows into a new cluster for the
    # rest; the file's own 3 clusters come after it, so the next growth is not adjacent either.
    files[SPLIT + '/' + CROSS] = b.copy_in('foreign.img', 1500, 60, SPLIT + '/' + CROSS)
    files[SPLIT + '/' + N200] = b.copy_in('foreign.img', 600, 61, SPLIT + '/' + N200)
    files[SPLIT + '/F00.TXT'] = empty
    image = Image(img)
    split = image.first_cluster(image.find(0, SPLIT)[1])
    chain = image.chain(split)
    slots = image.slots(split)
    per = image.cluster_bytes // 32
    name, alias, lfn = image.find(split, CROSS)
    if not (len(chain) == 3 and chain[1] != chain[0] + 1 and chain[2] != chain[1] + 1 and
            lfn == [slots[per - 1], slots[per]] and alias == slots[per + 1]):
        fail('foreign.img: %s is not split across the fragmented chain %s' % (CROSS, chain))
    name, alias, lfn = image.find(split, N200)
    if not (lfn[0] in slots[per:2 * per] and alias in slots[2 * per:]):
        fail('foreign.img: the 200-character set does not cross into the third cluster')
    return image, files, split


# Damage to the 255-character set in the root of foreign.img. L[0] is the first entry on disk
# (ordinal 20 | LAST: characters 247-254, the 0x0000 terminator, then 0xFFFF padding), L[19] the
# last (ordinal 1: characters 0-12). Each entry: (name, what is wrong, [(entry, byte, value)]).
DAMAGED_255 = [
    ('checksum-one', 'one entry has another checksum', [(0, 13, 'xor1')]),
    ('checksum-all', 'all entries agree, but not with the alias', [(i, 13, 'xor1') for i in range(20)]),
    ('gap', 'ordinals 20, 17, 18, ...', [(1, 0, 17)]),
    ('order', 'ordinals 20, 18, 19, 17, ...', [(1, 0, 18), (2, 0, 19)]),
    ('type', 'type byte 1 in the LAST entry', [(0, 12, 1)]),
    ('type-middle', 'type byte 1 in the 11th entry', [(10, 12, 1)]),
    ('cluster-low', 'first-cluster word 0x0001', [(0, 26, 1)]),
    ('cluster-high', 'first-cluster word 0x0100 in the 11th entry', [(10, 27, 1)]),
    ('padding', 'a 0xFF78 unit after the terminator', [(0, 24, ord('x'))]),
    ('padding-zero', 'a 0x0000 unit after the terminator', [(0, 28, 0), (0, 29, 0)]),
    ('ordinal21', 'LAST entry has ordinal 21', [(0, 0, 0x55)]),
    ('ordinal63', 'LAST entry has ordinal 63', [(0, 0, 0x7F)]),
    ('ordinal-zero', 'LAST entry has ordinal 0', [(0, 0, 0x40)]),
    ('no-last', 'no LAST flag', [(0, 0, 20)]),
    ('reserved-flag', 'bit 0x80 set in the ordinal', [(0, 0, 0xD4)]),
    ('second-last', 'ordinal 1 also has LAST (a 13-character name on its own)', [(19, 0, 0x41)]),
    ('length256', '256 characters', [(0, 20, ord('x')), (0, 21, 0), (0, 22, 0), (0, 23, 0)]),
    ('length260', '260 characters', [(0, o, ord('x')) for o in (20, 22, 24, 28, 30)] +
     [(0, o + 1, 0) for o in (20, 22, 24, 28, 30)]),
    ('nonascii', 'character 0 is U+0080', [(19, 1, 0x80)]),
    ('high-byte', 'character 0 is U+0130', [(19, 2, 1)]),
    ('control', 'character 0 is 0x1F', [(19, 1, 0x1F)]),
    ('del', 'character 0 is 0x7F', [(19, 1, 0x7F)]),
    ('early-nul', 'a terminator in the ordinal-1 entry (character 1)', [(19, 3, 0), (19, 4, 0)]),
    ('early-end', 'the ordinal-1 entry ends after character 0 (terminator, then padding)',
     [(19, 3, 0), (19, 4, 0)] + [(19, o + i, 0xFF) for o in OFFSETS[2:] for i in (0, 1)]),
    ('slash', 'character 0 is /', [(19, 1, ord('/'))]),
    ('backslash', 'character 0 is a backslash', [(19, 1, 0x5C)]),
    ('colon', 'character 0 is :', [(19, 1, ord(':'))]),
    ('question', 'character 0 is ?', [(19, 1, ord('?'))]),
    ('leading-space', 'character 0 is a space', [(19, 1, ord(' '))]),
    ('trailing-space', 'character 254 is a space', [(0, 18, ord(' '))]),
    ('trailing-dot', 'character 254 is a dot', [(0, 18, ord('.'))]),
]

# Damage to sets that cross the fragmented chain of 'Split directory' (and to its own name).
DAMAGED_SPLIT = [
    ('cross-checksum', CROSS, 'the entry in the second cluster has another checksum', [(1, 13, 'xor1')]),
    ('cross-ordinal', CROSS, 'the entry in the second cluster has ordinal 2', [(1, 0, 2)]),
    ('cross-deleted', CROSS, 'the LAST entry (end of the first cluster) is deleted', [(0, 0, 0xE5)]),
    ('cross200-type', N200, 'type byte 1 in an entry in the third cluster', [(-1, 12, 1)]),
    ('dir-checksum', None, "the directory's own long name has another checksum", [(0, 13, 'xor1')]),
]


def damage(image, entries, edits):
    for idx, byte, value in edits:
        off = entries[idx] + byte
        image.data[off] = image.data[off] ^ 1 if value == 'xor1' else value


# What fsck.fat -n (dosfstools 4.2) says about each damaged image, measured: (exit status, the
# lines between its version line and its summary line). Many kinds of damage are invisible to
# it (exit 0, no lines): those images are pinned that way too, never called clean.
NA = '  Not auto-correcting this.'
RESERVED_SLOT = ['Reserved field in VFAT long filename slot is not 0 (but 0x01).', 'Auto-setting to 0.', '',
                 'Leaving filesystem unchanged.']


def frag(k, name=N255):
    return name[(k - 1) * 13:k * 13]


def outside(ks, name=N255):
    out = []
    for k in ks:
        out += ['Long filename fragment "%s" found outside a LFN sequence.' % frag(k, name),
                '  (Maybe the start bit is missing on the last fragment)', NA]
    return out


def sequence(got, want, here, before):
    return ['Unexpected long filename sequence number (%d vs. expected %d).' % (got, want),
            '  It could be that just the number is wrong',
            '  if "%s" seems to match "%s".' % (here, before), NA]


def part_checksum(stored, first):
    return ['Checksum in long filename part wrong (%02x vs. expected %02x).' % (stored, first), NA]


def name_checksum(name, alias):
    return ['Wrong checksum for long file name "%s".' % name,
            '  (Short name %s may have changed without updating the long name)' % alias, NA]


def reclaimed(line):
    return ['/' + SPLIT, line, 'Reclaimed 8 unused clusters (4096 bytes).', '', 'Leaving filesystem unchanged.']


def fsck_expected(c255, alias255, csplit, ccross, clusters):
    silent = (0, [])
    exp = {tag: silent for tag in (
        'padding', 'padding-zero', 'length256', 'length260', 'nonascii', 'high-byte', 'control', 'del',
        'early-nul', 'early-end', 'slash', 'backslash', 'colon', 'question', 'leading-space', 'trailing-space',
        'trailing-dot', 'reserved-flag', 'dot', 'dotdot', 'ambiguous-long', 'ambiguous-alias')}
    exp.update({
        'checksum-one': (0, part_checksum(c255, c255 ^ 1) * 19 + name_checksum(N255, alias255)),
        'checksum-all': (0, name_checksum(N255, alias255)),
        'gap': (0, sequence(17, 19, frag(19), frag(20)) + outside(range(18, 0, -1))),
        'order': (0, sequence(18, 19, frag(19), frag(20)) + outside(range(18, 0, -1))),
        'type': (1, RESERVED_SLOT),
        'type-middle': (1, RESERVED_SLOT),
        'cluster-low': (1, ['Start cluster field in VFAT long filename slot is not 0 (but 0x0001).'] + RESERVED_SLOT[1:]),
        'cluster-high': (1, ['Start cluster field in VFAT long filename slot is not 0 (but 0x0100).'] + RESERVED_SLOT[1:]),
        'ordinal21': (0, sequence(19, 20, frag(19), frag(20)) + outside(range(18, 0, -1))),
        'ordinal63': (0, sequence(19, 30, frag(19), frag(20)) + outside(range(18, 0, -1))),
        'ordinal-zero': (0, ['Unexpected long filename sequence number (0 vs. expected -1).', NA] +
                         outside(range(19, 0, -1))),
        'no-last': (0, outside(range(20, 0, -1))),
        'second-last': (0, ['A new long file name starts within an old one.',
                            '  It could be that the LFN start bit is wrong here',
                            '  if "%s" seems to match "%s".' % (frag(1), N255[13:]), NA]),
        'cross-checksum': (0, part_checksum(ccross ^ 1, ccross)),
        'cross-ordinal': (0, sequence(2, 1, frag(1, CROSS), frag(2, CROSS))),
        'cross-deleted': (0, outside([1], CROSS)),
        'cross200-type': (1, RESERVED_SLOT),
        'dir-checksum': (0, part_checksum(csplit, csplit ^ 1) + name_checksum(SPLIT, 'SPLITD~1')),
        'dir-cluster0': (1, reclaimed(' Start does point to root directory. Deleting dir. ')),
        'dir-cluster1': (1, reclaimed('  Bad start cluster 1. Truncating file.')),
        'dir-cluster-max': (1, reclaimed('  Start cluster beyond limit (%d > %d). Truncating file.' %
                                         (clusters + 2, clusters + 1))),
        'dir-cluster-ffff': (1, reclaimed('  Start cluster beyond limit (65535 > %d). Truncating file.' %
                                          (clusters + 1))),
        'orphan': (1, ['Orphaned long file name part "Orphan name.txt"', '  Auto-deleting.', '',
                       'Leaving filesystem unchanged.']),
        'odd': (1, name_checksum('Label name', 'SECOND') +
                ['/A\x01B.TXT', '  Bad short file name (A\x01B.TXT).', '  Auto-renaming it.',
                    '  Renamed to FSCK0000.000', '/NUL', '  Bad short file name (NUL).', '  Auto-renaming it.',
                    '  Renamed to FSCK0000.001', '', 'Leaving filesystem unchanged.']),
    })
    return exp


def main(argv):
    if len(argv) != 1:
        fail('usage: vfat-fixtures.py <out dir>')
    out = argv[0]
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    b = Builder(out)

    # --- legacy.img: valid lookups by long name, alias and case variants ---
    legacy, lfiles = build_legacy(b)
    b.fsck.append(('legacy.img', 61029, 0, '-'))
    for name, payload in lfiles.items():
        cluster, parts = 0, name.split('/')
        aliases = []
        for part in parts:
            rec = legacy.find(cluster, part)
            aliases.append(legacy.alias(rec[1]))
            cluster = legacy.first_cluster(rec[1])
        b.case('ext', 'legacy.img', '0:/' + name, '=' + payload)
        b.case('ext', 'legacy.img', '0:/' + name.swapcase(), '=' + payload)
        b.case('ext', 'legacy.img', '0:/' + '/'.join(aliases), '=' + payload)
        b.case('ext', 'legacy.img', '0:/' + '/'.join(aliases).lower(), '=' + payload)
        # The old entry points find the alias (8.3 path), never the long name.
        b.case('fopen', 'legacy.img', '0:/' + '/'.join(aliases), '=' + payload)
        if parts[-1].upper() != aliases[-1].upper() and len(parts) == 1:
            b.case('fopen', 'legacy.img', '0:/' + name, 'error')
            b.case('open', 'legacy.img', '0:/' + name, 'error')
        b.case('prefixes', 'legacy.img', '0:/' + name, 'none')
    b.case('ext', 'legacy.img', '0:/SHELL.ELF', '=programs/shell/build/shell.elf')
    b.case('ext', 'legacy.img', '0:/RORTHOX', '-ENOENT')                      # the root label
    b.case('fopen', 'legacy.img', '0:/RORTHOX', '-ENOENT')
    b.case('open', 'legacy.img', '0:/RORTHOX', '-ENOENT')
    b.case('ext', 'legacy.img', '0:/rorthox', '-ENOENT')
    b.case('ext', 'legacy.img', '0:/Long directory name', '-EINVARG')         # a directory
    b.case('ext', 'legacy.img', '0:/Long directory name/missing', '-ENOENT')
    b.case('ext', 'legacy.img', '0:/' + N13 + '/x', '-EIO')                   # a file as parent
    b.case('oom', 'legacy.img', '0:/Long directory name/Nested Dir/deep.txt', '=' + lfiles['Long directory name/Nested Dir/deep.txt'])
    b.case('readfail', 'legacy.img', '0:/' + N255, '=' + lfiles[N255])

    # --- foreign.img: 512-byte clusters, a fragmented directory ---
    foreign, ffiles, split = build_foreign(b)
    clusters = foreign.clusters
    b.fsck.append(('foreign.img', clusters, 0, '-'))
    for name, payload in ffiles.items():
        b.case('ext', 'foreign.img', '0:/' + name, '=' + payload)
        b.case('ext', 'foreign.img', '0:/' + name.upper(), '=' + payload)
        b.case('prefixes', 'foreign.img', '0:/' + name, 'none')
    split_alias = foreign.alias(foreign.find(0, SPLIT)[1])
    cross_alias = foreign.alias(foreign.find(split, CROSS)[1])
    b.case('ext', 'foreign.img', '0:/%s/%s' % (split_alias, cross_alias), '=' + ffiles[SPLIT + '/' + CROSS])
    b.case('fopen', 'foreign.img', '0:/%s/%s' % (split_alias, cross_alias), '=' + ffiles[SPLIT + '/' + CROSS])
    b.case('ext', 'foreign.img', '0:/RORTHOX', '-ENOENT')
    # '.' and '..' are never targets: the extended path refuses them before any lookup.
    for path in ('0:/%s/.' % SPLIT, '0:/%s/..' % SPLIT, '0:/%s/../%s' % (SPLIT, N13)):
        b.case('ext', 'foreign.img', path, '-EINVARG')
    b.case('fopen', 'foreign.img', '0:/' + CROSS, 'error')
    b.case('readfail', 'foreign.img', '0:/%s/%s' % (SPLIT, CROSS), '=' + ffiles[SPLIT + '/' + CROSS])
    b.case('oom', 'foreign.img', '0:/%s/%s' % (SPLIT, N200), '=' + ffiles[SPLIT + '/' + N200])

    # Decoder inputs: the sets exactly as mtools wrote them.
    for tag, cluster, name in (('13', 0, N13), ('26', 0, N26), ('255', 0, N255),
                               ('cross', split, CROSS), ('cross200', split, N200)):
        rec = foreign.find(cluster, name)
        with open(b.path('sets/%s.bin' % tag), 'wb') as f:
            for off in rec[2] + [rec[1]]:
                f.write(foreign.data[off:off + 32])
        with open(b.path('sets/%s.name' % tag), 'w') as f:
            f.write(name)

    # --- damaged copies: the long name falls back to the alias ---
    root255 = foreign.find(0, N255)
    alias255 = foreign.alias(root255[1])
    sets_written = {}
    for tag, what, edits in DAMAGED_255:
        image = Image(foreign.path)
        damage(image, root255[2], edits)
        image.save(b.path(tag + '.img'))
        b.case('ext', tag + '.img', '0:/' + N255, '-ENOENT')
        b.case('prefixes', tag + '.img', '0:/' + N255, 'none')
        b.case('ext', tag + '.img', '0:/' + alias255, '=' + ffiles[N255])
        b.case('fopen', tag + '.img', '0:/' + alias255, '=' + ffiles[N255])
        with open(b.path('sets/%s.bin' % tag), 'wb') as f:
            for off in root255[2] + [root255[1]]:
                f.write(image.data[off:off + 32])
        sets_written[tag] = what
    with open(b.path('sets/damaged.txt'), 'w') as f:
        f.write(''.join('%s\t%s\n' % kv for kv in sets_written.items()))
    for tag, target, what, edits in DAMAGED_SPLIT:
        image = Image(foreign.path)
        if target is None:
            damage(image, foreign.find(0, SPLIT)[2], edits)
            long_path = '0:/%s/%s' % (SPLIT, CROSS)
            alias_path, payload = '0:/%s/%s' % (split_alias, cross_alias), ffiles[SPLIT + '/' + CROSS]
        else:
            rec = foreign.find(split, target)
            damage(image, rec[2], edits)
            long_path = '0:/%s/%s' % (SPLIT, target)
            alias_path = '0:/%s/%s' % (SPLIT, foreign.alias(rec[1]))
            payload = ffiles[SPLIT + '/' + target]
        image.save(b.path(tag + '.img'))
        b.case('ext', tag + '.img', long_path, '-ENOENT')
        b.case('prefixes', tag + '.img', long_path, 'none')
        b.case('ext', tag + '.img', alias_path, '=' + payload)

    # Two names of the dot form, on the 13-character set (one full entry, so no terminator).
    rec13 = foreign.find(0, N13)
    for tag, text in (('dot', '.'), ('dotdot', '..')):
        image = Image(foreign.path)
        units = [ord(c) for c in text] + [0] + [0xFFFF] * (12 - len(text))
        for i, o in enumerate(OFFSETS):
            struct.pack_into('<H', image.data, rec13[2][0] + o, units[i])
        image.save(b.path(tag + '.img'))
        b.case('ext', tag + '.img', '0:/' + foreign.alias(rec13[1]), '=' + ffiles[N13])
        b.case('ext', tag + '.img', '0:/' + N13, '-ENOENT')

    # --- 'Split directory' with a first cluster outside 2..max ---
    dir_off = foreign.find(0, SPLIT)[1]
    for tag, value in (('dir-cluster0', 0), ('dir-cluster1', 1), ('dir-cluster-max', clusters + 2),
                       ('dir-cluster-ffff', 0xFFFF)):
        image = Image(foreign.path)
        struct.pack_into('<H', image.data, dir_off + 26, value)
        image.save(b.path(tag + '.img'))
        b.case('ext', tag + '.img', '0:/%s/%s' % (SPLIT, CROSS), '-EIO')
        b.case('ext', tag + '.img', '0:/%s/F00.TXT' % split_alias, '-EIO')
        b.case('fopen', tag + '.img', '0:/%s/F00.TXT' % split_alias, '-EIO')

    # --- ambiguous: two distinct records answer to the same name ---
    image = Image(foreign.path)
    slot = image.free_root_slot()
    rec = lfn_entries(N13, b'DUP     TXT') + short_entry(b'DUP     TXT')
    image.data[slot:slot + len(rec)] = rec
    image.save(b.path('ambiguous-long.img'))
    b.case('ext', 'ambiguous-long.img', '0:/' + N13, '-EIO')
    b.case('ext', 'ambiguous-long.img', '0:/DUP.TXT', '=' + empty_payload(b))
    b.case('fopen', 'ambiguous-long.img', '0:/' + foreign.alias(rec13[1]), '=' + ffiles[N13])
    image = Image(foreign.path)
    rec = lfn_entries('short.txt', b'OTHER   TXT') + short_entry(b'OTHER   TXT')
    image.data[slot:slot + len(rec)] = rec
    image.save(b.path('ambiguous-alias.img'))
    b.case('ext', 'ambiguous-alias.img', '0:/SHORT.TXT', '-EIO')       # SHORT.TXT's alias + this LFN
    b.case('ext', 'ambiguous-alias.img', '0:/OTHER.TXT', '=' + empty_payload(b))
    b.case('fopen', 'ambiguous-alias.img', '0:/SHORT.TXT', '=' + ffiles['SHORT.TXT'])

    # --- odd.img: short names with odd bytes and a long name on a label ---
    image = Image(foreign.path)
    slot = image.free_root_slot()
    odd = [short_entry(b'\x05OO     TXT'),                  # 0x05 = OEM 0xE5, not deleted
           short_entry(b'A\x01B     TXT'),
           short_entry(b'\x80BC     TXT', ntres=0x08),
           short_entry(b'NUL\x00    TXT'),
           short_entry(b'LWR     TXT', ntres=0x18),
           short_entry(b'MXD     TXT', ntres=0x10),
           # A long name, then the label, then AFTER.TXT: the long name's checksum is AFTER.TXT's,
           # so only the label ending the set keeps it off that file.
           lfn_entries('Label name', b'AFTER   TXT') + short_entry(b'SECOND     ', attr=0x08),
           short_entry(b'AFTER   TXT')]
    raw = b''.join(odd)
    image.data[slot:slot + len(raw)] = raw
    image.save(b.path('odd.img'))
    for path in ('AFTER.TXT', 'after.txt', 'lwr.txt', 'LWR.TXT', 'mxd.TXT'):
        b.case('ext', 'odd.img', '0:/' + path, '=' + empty_payload(b))
    for path in ('?OO.TXT', 'A?B.TXT', '?bc.TXT', 'NUL?.TXT'):
        b.case('ext', 'odd.img', '0:/' + path, '-EINVARG')       # '?' is not a name character
    b.case('ext', 'odd.img', '0:/Label name', '-ENOENT')
    b.case('ext', 'odd.img', '0:/SECOND', '-ENOENT')
    b.case('ext', 'odd.img', '0:/' + N255, '=' + ffiles[N255])
    b.case('fopen', 'odd.img', '0:/AFTER.TXT', '=' + empty_payload(b))
    b.case('fopen', 'odd.img', '0:/SECOND', '-ENOENT')
    b.case('fopen', 'ambiguous-long.img', '0:/DUP.TXT', '=' + empty_payload(b))

    # --- orphan.img: a long name whose short entry was deleted, then a live entry its checksum fits ---
    image = Image(foreign.path)
    slot = image.free_root_slot()
    rec = (lfn_entries('Orphan name.txt', b'TARGET  TXT') + short_entry(b'\xe5ARGET  TXT') +
           short_entry(b'TARGET  TXT'))
    image.data[slot:slot + len(rec)] = rec
    image.save(b.path('orphan.img'))
    b.case('ext', 'orphan.img', '0:/Orphan name.txt', '-ENOENT')
    b.case('ext', 'orphan.img', '0:/TARGET.TXT', '=' + empty_payload(b))

    # Listings: what a scan of a directory gives, in order (long name or alias, the alias as shown).
    def listing(tag, image, cluster, extra=()):
        with open(b.path('listing-%s.txt' % tag), 'w') as f:
            for name, off, lfn in image.records(cluster):
                if image.data[off + 11] & 0x08 or image.data[off] == 0x2E:
                    continue
                f.write('%s|%s|%d\n' % (name or image.shown(off), image.shown(off), 1 if name else 0))
            f.write(''.join(extra))
    listing('foreign-split', foreign, split)
    with open(b.path('listing-before-split.txt'), 'w') as f:      # root records before it
        for name, off, lfn in foreign.records(0):
            if off == foreign.find(0, SPLIT)[1]:
                break
            if not foreign.data[off + 11] & 0x08:
                f.write('%s|%s|%d\n' % (name or foreign.shown(off), foreign.shown(off), 1 if name else 0))
    with open(b.path('listing-odd.txt'), 'w') as f:
        for name, off, lfn in foreign.records(0):
            if not foreign.data[off + 11] & 0x08:
                f.write('%s|%s|%d\n' % (name or foreign.shown(off), foreign.shown(off), 1 if name else 0))
        # Every byte outside 0x20-0x7E shows as '?'; the NTRes bits lower ASCII letters only.
        f.write('?OO.TXT|?OO.TXT|0\nA?B.TXT|A?B.TXT|0\n?bc.TXT|?bc.TXT|0\nNUL?.TXT|NUL?.TXT|0\n'
                'lwr.txt|lwr.txt|0\nMXD.txt|MXD.txt|0\nAFTER.TXT|AFTER.TXT|0\n')

    with open(b.path('cases.txt'), 'w') as f:
        f.write('\n'.join(b.cases) + '\n')
    expected = fsck_expected(checksum(foreign.data[root255[1]:root255[1] + 11]), alias255,
                             checksum(foreign.data[dir_off:dir_off + 11]),
                             checksum(foreign.data[foreign.find(split, CROSS)[1]:][:11]), clusters)
    images = sorted(f[:-4] for f in os.listdir(out) if f.endswith('.img'))
    if sorted(expected) != [t for t in images if t not in ('legacy', 'foreign')]:
        fail('fsck expectations and images differ')
    os.makedirs(b.path('fsck'), exist_ok=True)
    for tag in sorted(expected):
        status, lines = expected[tag]
        diag = b.path('fsck/%s.txt' % tag)
        with open(diag, 'wb') as f:
            f.write(''.join(l + '\n' for l in lines).encode('latin-1'))
        b.fsck.append((tag + '.img', clusters, status, diag))
    with open(b.path('fsck.txt'), 'w') as f:
        for image_name, n, status, diag in b.fsck:
            f.write('%s\t%d\t%d\t%s\n' % (image_name, n, status, diag))
    with open(b.path('geometry.txt'), 'w') as f:
        f.write('foreign clusters=%d split chain=%s\n' % (clusters, foreign.chain(split)))
    print('cases=%d' % len(b.cases))


_empty = []


def empty_payload(b):
    if not _empty:
        _empty.append(b.payload(0, 0))
    return _empty[0]


if __name__ == '__main__':
    main(sys.argv[1:])
