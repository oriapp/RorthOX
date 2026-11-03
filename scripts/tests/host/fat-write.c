/*
 * Host check for scripts/tests/fat-write.sh: creating, writing, appending and truncating files
 * through the real kernel/fs/file.c, pparser.c, fat/fat16.c and disk/streamer.c, built with the
 * host C compiler, on a copy of the RorthOX image held in memory. Each case runs in its own
 * process on a fresh copy.
 *
 * The disk model keeps two layers: written sectors stay pending until disk_flush() makes them
 * durable, and every write and flush goes into a ledger. disk_write_capacity() reports the image
 * size rounded up to whole sectors (what QEMU's IDENTIFY gives), or a smaller count or an error
 * for some cases, and writes past it are refused like the real driver does. Free data clusters
 * are filled with POISON before a case, so a published file byte that was never written shows.
 *
 * The checks read the disk with their own FAT16 code (not the kernel's): every directory entry's
 * chain must run through allocated clusters in both FAT copies to an end marker and cover the
 * size, no cluster may belong to two chains, and files the case knows hold their pattern for
 * every byte inside their size. After a whole operation: both FATs are identical, every chain is
 * exactly as long as its size needs, every allocated cluster belongs to a file or directory, and
 * the bytes after the end of each file in its last cluster are zero.
 *
 * Crash cuts: for an operation's ledger, the disk as it would be after a crash after each flush,
 * with none, all, each single one, and all but each single one of the sectors written since that
 * flush, must pass the same checks. That proves the order (data, then the new chain in FAT 1 and
 * FAT 2, then the old tail, then the directory entry; a truncated entry before any freed
 * cluster) under the model, not on real hardware.
 *
 * Cases: see the table at the end. Usage: fat-write <image> <outdir>. The "save" case writes
 * <outdir>/fixture.img (before) and <outdir>/written.img (after) and the expected file contents
 * as <outdir>/expect-<name>, for the script's mtools and fsck.fat checks.
 *
 * Prints one line per failing case and "fat-write host check: <failed> of <cases> cases failed",
 * plus "fat-write: <n> crash states checked" and the counts of the failure sweeps.
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "string/string.h"
#include "fs/file.h"
#include "disk/disk.h"
#include "disk/streamer.h"

void *malloc(unsigned long size);
void free(void *ptr);
void *memset(void *ptr, int c, unsigned long size);
void *memcpy(void *dest, const void *src, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
// kernel/string/string.c brings its own snprintf, so format with the C library's checked one.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define fmt(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)
long lseek(int fd, long offset, int whence);
long write(int fd, const void *buf, unsigned long count);
unsigned int alarm(unsigned int seconds);
void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
extern struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define SECTOR 512
#define POISON 0xEE
#define INT_MAX_VALUE 0x7FFFFFFF

// ---------------------------------------------------------------------------------------------
// The disk

static unsigned char *img;       // durable contents, whole sectors
static long img_sectors;
static long file_bytes;
static unsigned char **pend;     // pending contents of a sector, NULL if none
static int *pend_list;
static int pend_count;
static struct disk the_disk;
static unsigned int capacity;
static int capacity_error;
static int n_reads, n_writes, n_flushes;
static int fail_read_n, fail_write_n, fail_flush_n;

struct record
{
    long lba;                    // -1: a flush
    unsigned char *data;
};
static struct record *ledger;
static int ledger_count, ledger_size;

// Counts the parent reads back from a counting run.
static struct shared
{
    int writes, flushes, reads, value;
    long states;
} *shared;

static void ledger_add(long lba, const void *data)
{
    if (ledger_count == ledger_size)
    {
        ledger_size = ledger_size ? 2 * ledger_size : 4096;
        struct record *bigger = malloc(ledger_size * sizeof(*bigger));
        if (ledger_count)
        {
            memcpy(bigger, ledger, ledger_count * sizeof(*bigger));
        }
        ledger = bigger;
    }
    ledger[ledger_count].lba = lba;
    ledger[ledger_count].data = 0;
    if (lba >= 0)
    {
        ledger[ledger_count].data = malloc(SECTOR);
        memcpy(ledger[ledger_count].data, data, SECTOR);
    }
    ledger_count++;
}

static int ledger_writes(void)
{
    int n = 0;
    for (int i = 0; i < ledger_count; i++)
    {
        n += ledger[i].lba >= 0;
    }
    return n;
}

struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

static const unsigned char *live_sector(long lba)
{
    return pend[lba] ? pend[lba] : img + lba * SECTOR;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    if (idisk != &the_disk || total < 1 || total > 256 || !buf || (long)lba + total > img_sectors)
    {
        return -EIO;
    }
    n_reads++;
    if (fail_read_n && n_reads == fail_read_n)
    {
        return -EIO;
    }
    for (int i = 0; i < total; i++)
    {
        memcpy((unsigned char *)buf + i * SECTOR, live_sector(lba + i), SECTOR);
    }
    return 0;
}

int disk_write_capacity(struct disk *idisk, unsigned int *sectors)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (!sectors)
    {
        return -EINVARG;
    }
    if (capacity_error)
    {
        return capacity_error;
    }
    *sectors = capacity;
    return 0;
}

int disk_write_block(struct disk *idisk, unsigned int lba, int total, const void *buf)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (total < 1 || total > 256 || !buf)
    {
        return -EINVARG;
    }
    if (capacity_error)
    {
        return capacity_error;
    }
    if (lba >= capacity || (unsigned int)total > capacity - lba)
    {
        return -EINVARG;
    }
    if ((long)lba + total > img_sectors)
    {
        // Inside the reported capacity but past what the model holds (c_int_max's drive is
        // bigger than its memory): a write the kernel must never make there.
        printf("    write of sector %u past the model's %ld sectors\n", lba, img_sectors);
        return -EIO;
    }
    n_writes++;
    if (fail_write_n && n_writes == fail_write_n)
    {
        return -EIO;
    }
    for (int i = 0; i < total; i++)
    {
        long s = (long)lba + i;
        ledger_add(s, (const unsigned char *)buf + i * SECTOR);
        if (!pend[s])
        {
            pend[s] = malloc(SECTOR);
            pend_list[pend_count++] = s;
        }
        memcpy(pend[s], (const unsigned char *)buf + i * SECTOR, SECTOR);
    }
    return 0;
}

int disk_flush(struct disk *idisk)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (capacity_error)
    {
        return capacity_error;
    }
    n_flushes++;
    if (fail_flush_n && n_flushes == fail_flush_n)
    {
        return -EIO;
    }
    for (int i = 0; i < pend_count; i++)
    {
        long s = pend_list[i];
        memcpy(img + s * SECTOR, pend[s], SECTOR);
        free(pend[s]);
        pend[s] = 0;
    }
    pend_count = 0;
    ledger_add(-1, 0);
    return 0;
}

// A crash: pending sectors are lost (keep = 0) or all reach the disk (keep = 1).
static void crash(int keep)
{
    for (int i = 0; i < pend_count; i++)
    {
        long s = pend_list[i];
        if (keep)
        {
            memcpy(img + s * SECTOR, pend[s], SECTOR);
        }
        free(pend[s]);
        pend[s] = 0;
    }
    pend_count = 0;
}

void print(const char *str) {}

// ---------------------------------------------------------------------------------------------
// Geometry, from the image's own BPB

static struct
{
    long spc, reserved, fats, root_entries, spf, root_sector, data_sector, cluster_bytes;
    long bpb_max;      // last cluster number the BPB allows (count + 1)
    long max_writable; // last cluster that lies wholly inside the reported capacity
} g;

static unsigned int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned int le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24); }

static void geometry(void)
{
    g.spc = img[13];
    g.reserved = le16(img + 14);
    g.fats = img[16];
    g.root_entries = le16(img + 17);
    g.spf = le16(img + 22);
    long total = le16(img + 19) ? le16(img + 19) : le32(img + 32);
    g.root_sector = g.reserved + g.fats * g.spf;
    g.data_sector = g.root_sector + (g.root_entries * 32 + SECTOR - 1) / SECTOR;
    g.cluster_bytes = g.spc * SECTOR;
    g.bpb_max = (total - g.data_sector) / g.spc + 1;
}

static long max_writable_for(unsigned int sectors)
{
    long complete = sectors > (unsigned long)g.data_sector ? (sectors - g.data_sector) / g.spc : 0;
    return complete + 1 < g.bpb_max ? complete + 1 : g.bpb_max;
}

static long cluster_byte(long cl) { return (g.data_sector + (cl - 2) * g.spc) * SECTOR; }
static long root_slot(int i) { return g.root_sector * SECTOR + 32L * i; }
static long dir_slot(long cl, int i) { return cluster_byte(cl) + 32L * i; }

// ---------------------------------------------------------------------------------------------
// Fixtures (written straight into the durable image before the volume is mounted)

static unsigned char pattern(int seed, long i)
{
    return (unsigned char)((i * 131 + (i >> 9) * 7 + seed * 29 + 1) ^ (i >> 16));
}

static void fx_fat(int copies, long cl, unsigned int value)
{
    for (int f = 0; f < 2; f++)
    {
        if (copies & (1 << f))
        {
            long at = (g.reserved + f * g.spf) * SECTOR + 2 * cl;
            img[at] = value & 0xFF;
            img[at + 1] = value >> 8;
        }
    }
}

static unsigned int fat_img(int copy, long cl)
{
    return le16(img + (g.reserved + copy * g.spf) * SECTOR + 2 * cl);
}

static void fx_entry(long pos, const char *name83, int attr, long cl, unsigned int size)
{
    unsigned char *e = img + pos;
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = attr;
    e[16] = 0x21; // 1980-01-01
    e[18] = 0x21;
    e[24] = 0x21;
    e[26] = cl & 0xFF;
    e[27] = (cl >> 8) & 0xFF;
    e[28] = size & 0xFF;
    e[29] = (size >> 8) & 0xFF;
    e[30] = (size >> 16) & 0xFF;
    e[31] = size >> 24;
}

// A file with its data (pattern seed), a zero tail, and its chain in both FATs.
static void fx_file(long pos, const char *name83, int seed, unsigned int size, const long *chain, int n, int attr)
{
    for (int i = 0; i < n; i++)
    {
        for (long b = 0; b < g.cluster_bytes; b++)
        {
            long at = i * g.cluster_bytes + b;
            img[cluster_byte(chain[i]) + b] = at < size ? pattern(seed, at) : 0;
        }
        fx_fat(3, chain[i], i + 1 < n ? chain[i + 1] : 0xFFFF);
    }
    fx_entry(pos, name83, attr, n ? chain[0] : 0, size);
}

// A directory: its cluster(s) zeroed, "." and "..", chain in both FATs.
static void fx_dir(long pos, const char *name83, const long *chain, int n)
{
    for (int i = 0; i < n; i++)
    {
        memset(img + cluster_byte(chain[i]), 0, g.cluster_bytes);
        fx_fat(3, chain[i], i + 1 < n ? chain[i + 1] : 0xFFFF);
    }
    fx_entry(dir_slot(chain[0], 0), ".          ", 0x10, chain[0], 0);
    fx_entry(dir_slot(chain[0], 1), "..         ", 0x10, 0, 0);
    fx_entry(pos, name83, 0x10, chain[0], 0);
}

static long freelist[70000];
static int free_count;

// The free clusters (both FATs 0) up to max_writable, in order; fills them with POISON.
static void fx_scan_free(void)
{
    free_count = 0;
    for (long cl = 2; cl <= g.max_writable + 1 && cl <= g.bpb_max; cl++)
    {
        if (!fat_img(0, cl) && !fat_img(1, cl))
        {
            if (cl <= g.max_writable)
            {
                freelist[free_count++] = cl;
            }
            long start = cluster_byte(cl);
            for (long b = 0; b < g.cluster_bytes && start + b < img_sectors * SECTOR; b++)
            {
                img[start + b] = POISON;
            }
        }
    }
}

static int root_first_free;   // first 0x00 root slot of the shipped image

// The standard fixture, with these root entries after the shipped files (F = free list):
//   OLD.BIN   70000 bytes, seed 1, chain F1 F3        TRUNC.BIN 140000 bytes, seed 2, F4 F6 F8
//   DIR       directory at F9                          RO.TXT    100 bytes, read-only, seed 3, F10
//   EMPTY.TXT 0 bytes, no cluster                      LONGNAME.TXT 50 bytes, seed 4, F11
// F0, F2, F5, F7 and F12 onwards stay free.
#define SEED_OLD 1
#define SEED_TRUNC 2
#define SEED_RO 3
#define SEED_LONG 4
#define SEED_NEW 5
static long F[32];

static void fixture(void)
{
    fx_scan_free();
    for (int i = 0; i < 32; i++)
    {
        F[i] = freelist[i];
    }
    int r = root_first_free;
    long c_old[] = { F[1], F[3] }, c_trunc[] = { F[4], F[6], F[8] }, c_dir[] = { F[9] };
    long c_ro[] = { F[10] }, c_long[] = { F[11] };
    fx_file(root_slot(r++), "OLD     BIN", SEED_OLD, 70000, c_old, 2, 0x20);
    fx_file(root_slot(r++), "TRUNC   BIN", SEED_TRUNC, 140000, c_trunc, 3, 0x20);
    fx_dir(root_slot(r++), "DIR        ", c_dir, 1);
    fx_file(root_slot(r++), "RO      TXT", SEED_RO, 100, c_ro, 1, 0x21);
    fx_file(root_slot(r++), "EMPTY   TXT", 0, 0, 0, 0, 0x20);
    fx_file(root_slot(r++), "LONGNAMETXT", SEED_LONG, 50, c_long, 1, 0x20);
    fx_scan_free();
}

// Mount the volume as the kernel would (fresh heap, fresh descriptor table).
static void mount(void)
{
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.type = RORTHOX_DISK_TYPE_REAL;
    the_disk.sector_size = SECTOR;
    fs_init();
    the_disk.filesystem = fs_resolve(&the_disk);
    if (!the_disk.filesystem)
    {
        printf("    the image did not mount as FAT16\n");
        fflush(0);
        _exit(2);
    }
}

// ---------------------------------------------------------------------------------------------
// The independent checker

static const unsigned char **view; // sector -> contents for the checker
static char why[300];

static void view_live(void)
{
    for (long s = 0; s < img_sectors; s++)
    {
        view[s] = live_sector(s);
    }
}

static int vbyte(long pos) { return view[pos / SECTOR][pos % SECTOR]; }
static unsigned int vfat(int copy, long cl)
{
    long at = (g.reserved + copy * g.spf) * SECTOR + 2 * cl;
    return vbyte(at) | (vbyte(at + 1) << 8);
}

// What a case knows about a file: its pattern seed and the sizes it may have.
struct expect
{
    const char *name83;
    int seed;          // -1: content not checked
    long sizes[4];     // allowed sizes, -1 ends the list; sizes[0] == -2: the file must not exist
    int absent_ok;     // 1: the file may also not exist (a crash before it was created)
};
static struct expect *expects;
static int expect_count;
static int found[32];

static unsigned char used[70000];

static int chain_walk(int copy, long first, long *out, int max, char *what)
{
    int n = 0;
    long cl = first;
    while (1)
    {
        if (cl < 2 || cl > g.bpb_max || n == max)
        {
            fmt(what, 120, "FAT %d: chain from %ld reaches cluster %ld (after %d)", copy + 1, first, cl, n);
            return -1;
        }
        unsigned int next = vfat(copy, cl);
        if (next == 0 || next == 1 || (next >= 0xFFF0 && next <= 0xFFF7))
        {
            fmt(what, 120, "FAT %d: cluster %ld of the chain from %ld is %#x", copy + 1, cl, first, next);
            return -1;
        }
        if (out)
        {
            out[n] = cl;
        }
        n++;
        if (next >= 0xFFF8)
        {
            return n;
        }
        cl = next;
    }
}

static long chain_buf[70000];

static int check_entry(const unsigned char *e, int final, int depth);

static int check_directory(long first, int final, int depth)
{
    char what[120];
    int n = chain_walk(0, first, chain_buf, 4096, what);
    if (n < 0)
    {
        fmt(why, sizeof(why), "directory: %s", what);
        return 0;
    }
    long chain[64];
    if (n > 64)
    {
        fmt(why, sizeof(why), "directory chain of %d clusters", n);
        return 0;
    }
    memcpy(chain, chain_buf, n * sizeof(long));
    for (int i = 0; i < n; i++)
    {
        if (used[chain[i]]++)
        {
            fmt(why, sizeof(why), "cluster %ld in two chains", chain[i]);
            return 0;
        }
    }
    for (int i = 0; i < n; i++)
    {
        for (long s = 0; s < g.cluster_bytes / 32; s++)
        {
            unsigned char e[32];
            for (int b = 0; b < 32; b++)
            {
                e[b] = vbyte(cluster_byte(chain[i]) + 32 * s + b);
            }
            if (!e[0])
            {
                return 1;
            }
            if (e[0] == '.' )
            {
                continue;
            }
            if (!check_entry(e, final, depth + 1))
            {
                return 0;
            }
        }
    }
    return 1;
}

static int check_entry(const unsigned char *e, int final, int depth)
{
    if (e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || ((e[11] & 0x08) && !(e[11] & 0x10)))
    {
        return 1;
    }
    char name[12];
    memcpy(name, e, 11);
    name[11] = 0;
    long first = le16(e + 26);
    unsigned int size = le32(e + 28);
    if (le16(e + 20))
    {
        fmt(why, sizeof(why), "%s: high cluster word %#x", name, le16(e + 20));
        return 0;
    }
    if (e[11] & 0x10)
    {
        return depth > 2 ? 1 : check_directory(first, final, depth);
    }
    long need = (size + g.cluster_bytes - 1) / g.cluster_bytes;
    int n = 0;
    if (first || size)
    {
        char what[120];
        int n2;
        n = chain_walk(0, first, chain_buf, 70000, what);
        if (n < 0)
        {
            fmt(why, sizeof(why), "%s (size %u): %s", name, size, what);
            return 0;
        }
        n2 = chain_walk(1, first, 0, 70000, what);
        if (n2 < 0)
        {
            fmt(why, sizeof(why), "%s (size %u): %s", name, size, what);
            return 0;
        }
        if (n < need || n2 < need)
        {
            fmt(why, sizeof(why), "%s: size %u needs %ld clusters, FAT 1 chain %d, FAT 2 chain %d", name, size, need, n, n2);
            return 0;
        }
        if (final && (n != need || n2 != need))
        {
            fmt(why, sizeof(why), "%s: size %u needs exactly %ld clusters, chains %d and %d", name, size, need, n, n2);
            return 0;
        }
        for (int i = 0; i < n; i++)
        {
            if (used[chain_buf[i]]++)
            {
                fmt(why, sizeof(why), "%s: cluster %ld in two chains", name, chain_buf[i]);
                return 0;
            }
        }
    }
    for (int x = 0; x < expect_count; x++)
    {
        if (memcmp(expects[x].name83, e, 11) != 0)
        {
            continue;
        }
        found[x]++;
        if (expects[x].sizes[0] == -2)
        {
            fmt(why, sizeof(why), "%s exists", name);
            return 0;
        }
        int ok = 0;
        for (int k = 0; k < 4 && expects[x].sizes[k] >= 0; k++)
        {
            ok |= expects[x].sizes[k] == size;
        }
        if (!ok)
        {
            fmt(why, sizeof(why), "%s: size %u is not one the case allows", name, size);
            return 0;
        }
        if (expects[x].seed < 0)
        {
            break;
        }
        for (long i = 0; i < (long)n * g.cluster_bytes; i++)
        {
            int b = vbyte(cluster_byte(chain_buf[i / g.cluster_bytes]) + i % g.cluster_bytes);
            int want = i < size ? pattern(expects[x].seed, i) : 0;
            if (i >= size && !final)
            {
                break;
            }
            if (b != want)
            {
                fmt(why, sizeof(why), "%s (size %u): byte %ld is %#x, expected %#x%s", name, size, i, b, want,
                         i >= size ? " (tail after the end of the file)" : "");
                return 0;
            }
        }
        break;
    }
    return 1;
}

// 1 if the volume in view passes; why says what is wrong otherwise.
static int check_volume(int final)
{
    memset(used, 0, sizeof(used));
    memset(found, 0, sizeof(found));
    why[0] = 0;
    for (int i = 0; i < g.root_entries; i++)
    {
        unsigned char e[32];
        for (int b = 0; b < 32; b++)
        {
            e[b] = vbyte(root_slot(i) + b);
        }
        if (!e[0])
        {
            break;
        }
        if (!check_entry(e, final, 0))
        {
            return 0;
        }
    }
    for (int x = 0; x < expect_count; x++)
    {
        if (expects[x].sizes[0] != -2 && found[x] != 1 && !(expects[x].absent_ok && !found[x]))
        {
            fmt(why, sizeof(why), "%.11s found %d times", expects[x].name83, found[x]);
            return 0;
        }
    }
    if (final)
    {
        for (long s = 0; s < g.spf * SECTOR; s++)
        {
            if (vbyte(g.reserved * SECTOR + s) != vbyte((g.reserved + g.spf) * SECTOR + s))
            {
                fmt(why, sizeof(why), "the FAT copies differ at byte %ld (entry %ld)", s, s / 2);
                return 0;
            }
        }
        for (long cl = 2; cl <= g.bpb_max; cl++)
        {
            if (vfat(0, cl) && !used[cl])
            {
                fmt(why, sizeof(why), "cluster %ld is allocated (%#x) but in no chain", cl, vfat(0, cl));
                return 0;
            }
        }
    }
    return 1;
}

static int live_ok(const char *what, int final)
{
    view_live();
    if (!check_volume(final))
    {
        printf("    %s: %s\n", what, why);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Crash cuts over one operation

static unsigned char *snap;

static void oracle_start(void)
{
    if (pend_count)
    {
        printf("    pending writes before the operation\n");
        fflush(0);
        _exit(2);
    }
    if (!snap)
    {
        snap = malloc(img_sectors * SECTOR);
    }
    memcpy(snap, img, img_sectors * SECTOR);
    ledger_count = 0;
}

static long *plba;
static unsigned char **pdata;

// Every crash state of the ledger since oracle_start() must pass check_volume(0).
static int oracle_check(const char *what)
{
    int phases = 0;
    for (int i = 0; i < ledger_count; i++)
    {
        phases += ledger[i].lba < 0;
    }
    if (!plba)
    {
        plba = malloc(img_sectors * sizeof(long));
        pdata = malloc(img_sectors * sizeof(unsigned char *));
    }
    for (int cut = 0; cut <= phases; cut++)
    {
        // The sectors written after flush number <cut> (latest contents).
        int np = 0, phase = 0;
        for (int i = 0; i < ledger_count; i++)
        {
            if (ledger[i].lba < 0)
            {
                phase++;
                continue;
            }
            if (phase != cut)
            {
                continue;
            }
            int k = 0;
            while (k < np && plba[k] != ledger[i].lba)
            {
                k++;
            }
            plba[k] = ledger[i].lba;
            pdata[k] = ledger[i].data;
            if (k == np)
            {
                np++;
            }
        }
        int variants = np ? 2 + 2 * np : 1;
        if (np == 1)
        {
            variants = 2;
        }
        for (int v = 0; v < variants; v++)
        {
            for (long s = 0; s < img_sectors; s++)
            {
                view[s] = snap + s * SECTOR;
            }
            phase = 0;
            for (int i = 0; i < ledger_count && phase < cut; i++)
            {
                if (ledger[i].lba < 0)
                {
                    phase++;
                }
                else
                {
                    view[ledger[i].lba] = ledger[i].data;
                }
            }
            for (int k = 0; k < np; k++)
            {
                int keep = v == 1 || (v >= 2 && v < 2 + np && k == v - 2) || (v >= 2 + np && k != v - 2 - np);
                if (keep)
                {
                    view[plba[k]] = pdata[k];
                }
            }
            shared->states++;
            if (!check_volume(0))
            {
                printf("    %s: crash after flush %d of %d, %s of the %d sector(s) written since: %s\n", what, cut, phases,
                       v == 0 ? "none" : v == 1 ? "all" : v < 2 + np ? "only one" : "all but one", np, why);
                return 0;
            }
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Helpers for the cases

static unsigned char *buf;       // pattern source / read buffer, 1 MiB

static void fill(int seed, long from, long n)
{
    for (long i = 0; i < n; i++)
    {
        buf[i] = pattern(seed, from + i);
    }
}

#define EXPECT(...) do { static struct expect e_[] = { __VA_ARGS__ }; expects = e_; \
    expect_count = sizeof(e_) / sizeof(e_[0]); } while (0)
#define FAIL(...) do { printf("    " __VA_ARGS__); printf("\n"); return 1; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) FAIL(__VA_ARGS__); } while (0)

static int file_size(int fd)
{
    struct file_stat st;
    return fstat(fd, &st) < 0 ? -1 : (int)st.filesize;
}

// Read the whole file through the byte interface and compare it with pattern(seed).
static int content_ok(const char *path, int seed, long size)
{
    int fd = file_open(path, FILE_MODE_READ);
    if (fd <= 0)
    {
        printf("    %s: read open returned %d\n", path, fd);
        return 0;
    }
    long got = 0;
    while (1)
    {
        int n = file_read(fd, buf, 70000);
        if (n < 0)
        {
            printf("    %s: read returned %d at %ld\n", path, n, got);
            return 0;
        }
        if (!n)
        {
            break;
        }
        for (int i = 0; i < n; i++)
        {
            if (buf[i] != pattern(seed, got + i))
            {
                printf("    %s: byte %ld reads %#x, expected %#x\n", path, got + i, buf[i], pattern(seed, got + i));
                return 0;
            }
        }
        got += n;
    }
    fclose(fd);
    if (got != size)
    {
        printf("    %s: read %ld bytes, expected %ld\n", path, got, size);
        return 0;
    }
    return 1;
}

static int heap_ok(const char *what, int live_before)
{
    if (kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf("    %s: bad free or write after free\n", what);
        return 0;
    }
    if (kalloc_live() != live_before)
    {
        printf("    %s: %d allocation(s) left behind\n", what, kalloc_live() - live_before);
        return 0;
    }
    return 1;
}

static int no_descriptors(void)
{
    for (int i = 0; i < RORTHOX_MAX_FILE_DESCRIPTORS; i++)
    {
        if (file_descriptors[i])
        {
            return 0;
        }
    }
    return 1;
}

static long entry_pos_of(long dirpos_start, int slots, const char *name83)
{
    for (int i = 0; i < slots; i++)
    {
        const unsigned char *e = live_sector((dirpos_start + 32L * i) / SECTOR) + (dirpos_start + 32L * i) % SECTOR;
        if (!e[0])
        {
            return -1;
        }
        if (!memcmp(e, name83, 11))
        {
            return dirpos_start + 32L * i;
        }
    }
    return -1;
}

static long chain_of(const char *name83, long *out)
{
    view_live();
    long pos = entry_pos_of(root_slot(0), g.root_entries, name83);
    if (pos < 0)
    {
        return -1;
    }
    char what[120];
    long first = vbyte(pos + 26) | (vbyte(pos + 27) << 8);
    return first ? chain_walk(0, first, out, 70000, what) : 0;
}

// ---------------------------------------------------------------------------------------------
// Cases. Each starts with the fixture written and the volume mounted.

// A new root file of 140000 bytes (3 clusters) in one write: it takes the free holes F0 F2 F5,
// zeroes the tail of its last cluster, every crash state is safe, and it reads back after a
// remount from the durable layer.
static int c_create_grow(void)
{
    EXPECT({ "NEW     BIN", SEED_NEW, { 0, 140000, -1 }, 1 }, { "OLD     BIN", SEED_OLD, { 70000, -1 } },
           { "TRUNC   BIN", SEED_TRUNC, { 140000, -1 } });
    oracle_start();
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "create returned %d", fd);
    fill(SEED_NEW, 0, 140000);
    int n = file_write(fd, buf, 140000);
    CHECK(n == 140000, "write returned %d", n);
    CHECK(file_size(fd) == 140000, "stat size %d", file_size(fd));
    CHECK(file_seek(fd, 0, SEEK_CUR) == 140000, "position %d", file_seek(fd, 0, SEEK_CUR));
    fclose(fd);
    CHECK(pend_count == 0, "%d sectors still pending after the write returned", pend_count);
    CHECK(oracle_check("create and grow"), "");
    CHECK(live_ok("create and grow", 1), "");
    long chain[8];
    CHECK(chain_of("NEW     BIN", chain) == 3 && chain[0] == F[0] && chain[1] == F[2] && chain[2] == F[5],
          "chain is not the free holes %ld %ld %ld", F[0], F[2], F[5]);
    mount();
    CHECK(content_ok("0:/NEW.BIN", SEED_NEW, 140000), "");
    CHECK(content_ok("0:/OLD.BIN", SEED_OLD, 70000), "");
    return 0;
}

// Append 200000 bytes to OLD.BIN (70000 bytes, chain F1 F3): three new clusters F0 F2 F5 are
// built detached, then linked after F3. Every crash state keeps OLD.BIN valid.
static int c_append_grow(void)
{
    EXPECT({ "OLD     BIN", SEED_OLD, { 70000, 270000, -1 } }, { "TRUNC   BIN", SEED_TRUNC, { 140000, -1 } });
    oracle_start();
    int fd = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    CHECK(fd > 0, "append open returned %d", fd);
    CHECK(file_seek(fd, 0, SEEK_CUR) == 70000, "append opens at %d, not the end", file_seek(fd, 0, SEEK_CUR));
    fill(SEED_OLD, 70000, 200000);
    int n = file_write(fd, buf, 200000);
    CHECK(n == 200000, "write returned %d", n);
    fclose(fd);
    CHECK(oracle_check("append and grow"), "");
    CHECK(live_ok("append and grow", 1), "");
    long chain[8];
    CHECK(chain_of("OLD     BIN", chain) == 5 && chain[0] == F[1] && chain[1] == F[3] && chain[2] == F[0] &&
          chain[3] == F[2] && chain[4] == F[5], "chain is not F1 F3 F0 F2 F5");
    mount();
    CHECK(content_ok("0:/OLD.BIN", SEED_OLD, 270000), "");
    return 0;
}

// WRITE-open of TRUNC.BIN (3 fragmented clusters): the entry becomes size 0 / cluster 0 before any
// cluster is freed, all three clusters are free in both FATs afterwards, and they are used again.
static int c_truncate(void)
{
    EXPECT({ "TRUNC   BIN", SEED_TRUNC, { 140000, 0, -1 } }, { "OLD     BIN", SEED_OLD, { 70000, -1 } });
    oracle_start();
    int fd = file_open("0:/TRUNC.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "write open returned %d", fd);
    CHECK(file_size(fd) == 0, "size after truncate %d", file_size(fd));
    fclose(fd);
    CHECK(oracle_check("truncate"), "");
    CHECK(live_ok("truncate", 1), "");
    long pos = entry_pos_of(root_slot(0), g.root_entries, "TRUNC   BIN");
    CHECK(pos >= 0 && le16(img + pos + 26) == 0 && le32(img + pos + 28) == 0, "entry not size 0, cluster 0");
    CHECK((img[pos + 11] & 0x20) && le16(img + pos + 24) == 0x21, "archive bit or date not set");
    for (int i = 4; i <= 8; i += 2)
    {
        CHECK(!fat_img(0, F[i]) && !fat_img(1, F[i]), "cluster %ld not free in both FATs", F[i]);
    }
    // The freed clusters are allocated again (lowest first: F0 F2 F4 F5 F6).
    fd = file_open("0:/TRUNC.BIN", FILE_MODE_APPEND);
    fill(SEED_TRUNC, 0, 5 * 65536);
    CHECK(file_write(fd, buf, 5 * 65536) == 5 * 65536, "rewrite failed");
    fclose(fd);
    long chain[8];
    CHECK(chain_of("TRUNC   BIN", chain) == 5 && chain[2] == F[4] && chain[4] == F[6], "freed clusters not reused");
    EXPECT({ "TRUNC   BIN", SEED_TRUNC, { 5 * 65536, -1 } });
    CHECK(live_ok("rewrite after truncate", 1), "");
    return 0;
}

// Writes inside the file: only the given bytes change (sector and cluster edges, both sides).
static int c_overwrite(void)
{
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "create returned %d", fd);
    fill(SEED_NEW, 0, 140000);
    CHECK(file_write(fd, buf, 140000) == 140000, "write failed");
    static const long at[] = { 511, 1023, 65535, 0, 139999 };
    static const int len[] = { 3, 2, 2, 1, 1 };
    for (int i = 0; i < 5; i++)
    {
        CHECK(file_seek(fd, at[i], SEEK_SET) == at[i], "seek to %ld", at[i]);
        for (int k = 0; k < len[i]; k++)
        {
            buf[k] = pattern(SEED_NEW, at[i] + k) ^ 0xFF;
        }
        CHECK(file_write(fd, buf, len[i]) == len[i], "overwrite at %ld", at[i]);
    }
    CHECK(file_size(fd) == 140000, "size changed to %d", file_size(fd));
    fclose(fd);
    fd = file_open("0:/NEW.BIN", FILE_MODE_READ);
    CHECK(file_read(fd, buf, 200000) == 140000, "read back");
    for (long i = 0; i < 140000; i++)
    {
        int changed = 0;
        for (int k = 0; k < 5; k++)
        {
            changed |= i >= at[k] && i < at[k] + len[k];
        }
        unsigned char want = pattern(SEED_NEW, i) ^ (changed ? 0xFF : 0);
        CHECK(buf[i] == want, "byte %ld is %#x, expected %#x", i, buf[i], want);
    }
    fclose(fd);
    EXPECT({ "NEW     BIN", -1, { 140000, -1 } });
    CHECK(live_ok("overwrite", 1), "");
    return 0;
}

// Two APPEND descriptors and a reader on OLD.BIN: each append lands at the end the other one
// made, also after a seek to 0; the reader sees the new size.
static int c_two_appenders(void)
{
    int a = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    int b = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    int r = file_open("0:/OLD.BIN", FILE_MODE_READ);
    CHECK(a > 0 && b > 0 && r > 0, "opens %d %d %d", a, b, r);
    fill(SEED_OLD, 70000, 30);
    CHECK(file_write(a, buf, 10) == 10, "first append");
    CHECK(file_write(b, buf + 10, 10) == 10, "second append");
    CHECK(file_seek(a, 0, SEEK_SET) == 0, "seek");
    CHECK(file_write(a, buf + 20, 10) == 10, "append after a seek");
    CHECK(file_size(r) == 70030 && file_size(b) == 70030, "sizes %d %d", file_size(r), file_size(b));
    fclose(a);
    fclose(b);
    fclose(r);
    CHECK(content_ok("0:/OLD.BIN", SEED_OLD, 70030), "");
    return 0;
}

// Truncate while a reader and a writer are open: the reader sees size 0 and EOF, keeps its
// position; a WRITE descriptor past the new end can't write (no hole).
static int c_truncate_others(void)
{
    int r = file_open("0:/TRUNC.BIN", FILE_MODE_READ);
    CHECK(file_read(r, buf, 100) == 100, "read 100");
    int w = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    fill(SEED_NEW, 0, 500);
    CHECK(file_write(w, buf, 500) == 500, "write 500");
    int w2 = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(w2 > 0, "second write open %d", w2);
    CHECK(file_size(w) == 0, "first writer sees size %d", file_size(w));
    CHECK(file_write(w, buf, 1) == -EINVARG, "write past the new end not refused");
    int t = file_open("0:/TRUNC.BIN", FILE_MODE_WRITE);
    CHECK(t > 0, "truncate open %d", t);
    CHECK(file_size(r) == 0, "reader sees size %d", file_size(r));
    CHECK(file_read(r, buf, 100) == 0, "reader not at EOF");
    // The reader keeps its position 100: once the file is that long again, it reads on from there.
    fill(SEED_TRUNC, 0, 300);
    CHECK(file_write(t, buf, 300) == 300, "rewrite after truncate");
    CHECK(file_read(r, buf + 1000, 300) == 200, "reader did not read on from its position");
    for (int i = 0; i < 200; i++)
    {
        CHECK(buf[1000 + i] == pattern(SEED_TRUNC, 100 + i), "reader byte %d", 100 + i);
    }
    fclose(r);
    fclose(w);
    fclose(w2);
    fclose(t);
    EXPECT({ "NEW     BIN", -1, { 0, -1 } }, { "TRUNC   BIN", SEED_TRUNC, { 300, -1 } });
    CHECK(live_ok("truncate under open descriptors", 1), "");
    return 0;
}

// A new root file is found again without a remount (the root cache), by the byte interface and
// by the loader's fopen/fstat/fread; a file in DIR too.
static int c_root_cache(void)
{
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    fill(SEED_NEW, 0, 1000);
    CHECK(file_write(fd, buf, 1000) == 1000, "write");
    fclose(fd);
    fd = fopen_status("0:/new.bin", "r");
    CHECK(fd > 0, "fopen of the new file returned %d", fd);
    CHECK(file_size(fd) == 1000, "fstat size %d", file_size(fd));
    CHECK(fread(buf, 1000, 1, fd) == 1, "fread");
    for (int i = 0; i < 1000; i++)
    {
        CHECK(buf[i] == pattern(SEED_NEW, i), "fread byte %d", i);
    }
    fclose(fd);
    fd = file_open("0:/DIR/NOTE.TXT", FILE_MODE_APPEND);
    CHECK(fd > 0, "create in DIR returned %d", fd);
    fill(SEED_NEW, 0, 7);
    CHECK(file_write(fd, buf, 7) == 7, "write in DIR");
    fclose(fd);
    CHECK(content_ok("0:/DIR/NOTE.TXT", SEED_NEW, 7), "");
    long pos = entry_pos_of(dir_slot(F[9], 0), 2048, "NOTE    TXT");
    CHECK(pos == dir_slot(F[9], 2), "NOTE.TXT is not in DIR's first free slot");
    EXPECT({ "NEW     BIN", SEED_NEW, { 1000, -1 } }, { "NOTE    TXT", SEED_NEW, { 7, -1 } });
    CHECK(live_ok("root cache", 1), "");
    return 0;
}

// Names: lowercase is stored uppercase and space padded; bad names, paths and targets are refused
// before anything is written.
static int c_names(void)
{
    static const char *bad[] = {
        "0:/ABCDEFGHI.TXT", "0:/ABC.TXTX", "0:/LONGNAMEX.TXT", "0:/LONGNAME.TXTX", "0:/A..B", "0:/.A", "0:/A.",
        "0:/A B", "0:/A*B", "0:/A?B", "0:/A+B", "0:/A,B", "0:/A;B", "0:/A=B", "0:/A[B", "0:/A]B", "0:/A|B",
        "0:/A\"B", "0:/A<B", "0:/A>B", "0:/A:B", "0:/A\\B", "0:/", "0:/DIR//X", "0:/X/", "1:/X", "0:X", "/X",
        "0:/.", "0:/..", "0:/DIR/../X", "0:/A.B.C", "0:/\x01", "0:/A\x7f", "0:/\x80", "0:/\xe5X", "",
    };
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        int fd = file_open(bad[i], FILE_MODE_WRITE);
        CHECK(fd == -EINVARG, "\"%s\": open returned %d, expected %d", bad[i], fd, -EINVARG);
        CHECK(fopen_status(bad[i], "w") < 0, "\"%s\": fopen w accepted", bad[i]);
    }
    char longpath[200];
    memset(longpath, 0, sizeof(longpath));
    memcpy(longpath, "0:/DIR", 6);
    for (int i = 6; i + 2 < RORTHOX_MAX_PATH + 2; i += 2)
    {
        memcpy(longpath + i, "/X", 2);
    }
    CHECK(file_open(longpath, FILE_MODE_WRITE) == -EINVARG, "a %d-byte path accepted", (int)strlen(longpath));
    CHECK(file_open(0, FILE_MODE_WRITE) == -EINVARG, "NULL path");
    CHECK(file_open("0:/NEW.BIN", 3) == -EINVARG, "mode 3");
    CHECK(ledger_writes() == 0, "%d sector writes for refused names", ledger_writes());
    static const struct { const char *path, *name83; } good[] = {
        { "0:/abcdefgh.txt", "ABCDEFGHTXT" }, { "0:/x", "X          " }, { "0:/a_b-1.c$", "A_B-1   C$ " },
        { "0:/DIR/z.y", "Z       Y  " },
    };
    for (int i = 0; i < 4; i++)
    {
        int fd = file_open(good[i].path, FILE_MODE_WRITE);
        CHECK(fd > 0, "\"%s\": open returned %d", good[i].path, fd);
        fclose(fd);
    }
    CHECK(entry_pos_of(root_slot(0), g.root_entries, good[0].name83) >= 0 &&
          entry_pos_of(root_slot(0), g.root_entries, good[1].name83) >= 0 &&
          entry_pos_of(root_slot(0), g.root_entries, good[2].name83) >= 0 &&
          entry_pos_of(dir_slot(F[9], 0), 2048, good[3].name83) >= 0, "a created name is not stored as 8.3 uppercase");
    EXPECT({ "LONGNAMETXT", SEED_LONG, { 50, -1 } }, { "ABCDEFGHTXT", -1, { 0, -1 } });
    CHECK(live_ok("names", 1), "");
    return 0;
}

// Targets: read-only file, directory, missing parent, file as parent, missing file for READ.
static int c_targets(void)
{
    CHECK(file_open("0:/RO.TXT", FILE_MODE_WRITE) == -ERDONLY, "WRITE on a read-only file");
    CHECK(file_open("0:/RO.TXT", FILE_MODE_APPEND) == -ERDONLY, "APPEND on a read-only file");
    CHECK(file_open("0:/DIR", FILE_MODE_WRITE) == -EINVARG, "WRITE on a directory");
    CHECK(file_open("0:/DIR", FILE_MODE_READ) == -EINVARG, "READ on a directory");
    CHECK(file_open("0:/NODIR/X", FILE_MODE_WRITE) == -ENOENT, "missing parent");
    CHECK(file_open("0:/OLD.BIN/X", FILE_MODE_WRITE) < 0, "a file as parent");
    CHECK(file_open("0:/NOPE.TXT", FILE_MODE_READ) == -ENOENT, "READ of a missing file");
    CHECK(ledger_writes() == 0, "%d sector writes for refused targets", ledger_writes());
    CHECK(content_ok("0:/RO.TXT", SEED_RO, 100), "");
    CHECK(no_descriptors(), "descriptors left open");
    return 0;
}

// Byte reads/writes and the signed seek: modes, zero counts, bounds.
static int c_modes_seek(void)
{
    int r = file_open("0:/LONGNAME.TXT", FILE_MODE_READ);
    int w = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(r > 0 && w > 0, "opens");
    int writes = ledger_writes();
    CHECK(file_write(r, buf, 0) == -EBADF && file_write(r, buf, 1) == -EBADF, "write on a READ descriptor");
    CHECK(file_read(w, buf, 0) == -EBADF && file_read(w, buf, 1) == -EBADF, "read on a WRITE descriptor");
    CHECK(file_write(w, 0, 0) == 0 && file_read(r, 0, 0) == 0, "zero counts");
    CHECK(file_write(w, buf, 0x80000000u) == -EINVARG && file_read(r, buf, 0x80000000u) == -EINVARG, "count > INT_MAX");
    CHECK(file_write(w, 0, 1) == -EINVARG && file_read(r, 0, 1) == -EINVARG, "NULL buffer");
    CHECK(file_read(0, buf, 1) == -EBADF && file_write(513, buf, 1) == -EBADF && file_seek(-1, 0, SEEK_SET) == -EBADF, "bad fd");
    CHECK(ledger_writes() == writes, "a refused or empty write wrote");
    struct { int off, whence, want; } s[] = {
        { 0, SEEK_SET, 0 }, { 50, SEEK_SET, 50 }, { 51, SEEK_SET, -EINVARG }, { -1, SEEK_SET, -EINVARG },
        { 0, SEEK_END, 50 }, { -50, SEEK_END, 0 }, { -51, SEEK_END, -EINVARG }, { 1, SEEK_END, -EINVARG },
        { 10, SEEK_SET, 10 }, { 5, SEEK_CUR, 15 }, { -15, SEEK_CUR, 0 }, { -1, SEEK_CUR, -EINVARG },
        { INT_MAX_VALUE, SEEK_CUR, -EINVARG }, { -INT_MAX_VALUE - 1, SEEK_CUR, -EINVARG },
        { -INT_MAX_VALUE - 1, SEEK_END, -EINVARG }, { INT_MAX_VALUE, SEEK_SET, -EINVARG }, { 0, 3, -EINVARG },
        { 0, -1, -EINVARG },
    };
    int pos = 0;
    for (unsigned int i = 0; i < sizeof(s) / sizeof(s[0]); i++)
    {
        int res = file_seek(r, s[i].off, s[i].whence);
        CHECK(res == s[i].want, "seek(%d, %d) returned %d, expected %d", s[i].off, s[i].whence, res, s[i].want);
        pos = res >= 0 ? res : pos;
        CHECK(file_seek(r, 0, SEEK_CUR) == pos, "position %d after seek(%d, %d), expected %d",
              file_seek(r, 0, SEEK_CUR), s[i].off, s[i].whence, pos);
    }
    // The loader's interface on the same descriptor keeps its own rules.
    CHECK(fseek(r, 51, SEEK_SET) == -EIO && fseek(r, 50, SEEK_SET) == 0 && fseek(r, 0, SEEK_END) == -EUNIMP, "legacy fseek");
    CHECK(fseek(r, 10, SEEK_SET) == 0 && fseek(r, 41, SEEK_CUR) == -EIO && fseek(r, 40, SEEK_CUR) == 0, "legacy fseek CUR");
    CHECK(fseek(r, 45, SEEK_SET) == 0 && fread(buf, 4, 3, r) == 1, "legacy fread count with a partial item");
    CHECK(file_seek(r, 45, SEEK_SET) == 45 && file_read(r, buf, 100) == 5 && file_read(r, buf, 100) == 0, "short read at EOF");
    fclose(r);
    fclose(w);
    return 0;
}

// Full root: the last slot (no end marker after it) is used, then -ENOSPC with nothing written;
// an existing file in the full root can still be written.
static int c_full_root(void)
{
    int writes;
    char name[12];
    for (int i = root_first_free + 6; i < g.root_entries - 1; i++)
    {
        fmt(name, sizeof(name), "F%02d     TXT", i);
        fx_entry(root_slot(i), name, 0x20, 0, 0);
    }
    mount();
    int fd = file_open("0:/LAST.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0, "the last root slot was not used (%d)", fd);
    fclose(fd);
    CHECK(entry_pos_of(root_slot(0), g.root_entries, "LAST    TXT") == root_slot(g.root_entries - 1), "not in the last slot");
    writes = ledger_writes();
    CHECK(file_open("0:/MORE.TXT", FILE_MODE_WRITE) == -ENOSPC, "full root not -ENOSPC");
    CHECK(file_open("0:/MORE.TXT", FILE_MODE_APPEND) == -ENOSPC, "full root not -ENOSPC (APPEND)");
    CHECK(ledger_writes() == writes, "the refused creates wrote");
    fd = file_open("0:/EMPTY.TXT", FILE_MODE_APPEND);
    fill(SEED_NEW, 0, 10);
    CHECK(fd > 0 && file_write(fd, buf, 10) == 10, "write to an existing file in a full root");
    fclose(fd);
    CHECK(file_open("0:/LAST.TXT", FILE_MODE_READ) > 0, "LAST.TXT not found again");
    mount();
    CHECK(content_ok("0:/EMPTY.TXT", SEED_NEW, 10), "");
    EXPECT({ "MORE    TXT", 0, { -2 } });
    CHECK(live_ok("full root", 1), "");
    return 0;
}

// The first byte of DIR's two clusters (cl[0], cl[1]) outside the 32-byte slot at skip that
// differs from before (the durable image), or -1.
static long dir_changed(const unsigned char *before, const long *cl, long skip)
{
    for (int c = 0; c < 2; c++)
    {
        for (long b = 0; b < g.cluster_bytes; b++)
        {
            long at = cluster_byte(cl[c]) + b;
            if ((at < skip || at >= skip + 32) && img[at] != before[c * g.cluster_bytes + b])
            {
                return at;
            }
        }
    }
    return -1;
}

// One write to an existing file in DIR: its new size and first cluster land in its own slot,
// every other byte of DIR's clusters stays, and it reads back after a remount.
static int dir_write(const char *path, int mode, int seed, long size, long slot, const long *cl, unsigned char *before)
{
    mount();
    CHECK(pend_count == 0, "pending writes before %s", path);
    memcpy(before, img + cluster_byte(cl[0]), g.cluster_bytes);
    memcpy(before + g.cluster_bytes, img + cluster_byte(cl[1]), g.cluster_bytes);
    int fd = file_open(path, mode);
    CHECK(fd > 0, "%s: %s open returned %d", path, mode == FILE_MODE_WRITE ? "WRITE" : "APPEND", fd);
    fill(seed, 0, size);
    int n = file_write(fd, buf, size);
    CHECK(n == size, "%s: write returned %d", path, n);
    fclose(fd);
    mount();
    long at = dir_changed(before, cl, slot);
    CHECK(at < 0, "%s: byte %ld of DIR changed (slot %ld of cluster %ld), not the file's slot at %ld", path, at,
          at < 0 ? 0 : (at - cluster_byte(at < cluster_byte(cl[1]) ? cl[0] : cl[1])) / 32,
          at < cluster_byte(cl[1]) ? cl[0] : cl[1], slot);
    CHECK(le32(img + slot + 28) == (unsigned int)size && le16(img + slot + 26) != 0,
          "%s: its slot holds size %u, cluster %u, expected size %ld", path, le32(img + slot + 28), le16(img + slot + 26), size);
    CHECK(content_ok(path, seed, size), "");
    return 0;
}

// DIR with two clusters (its first one full): a create lands in the second cluster, and writes
// to the files there change only their own slots; DIR with one full cluster gives -ENOSPC (no
// growth).
static int c_dir_chain(void)
{
    long dir2 = F[13];
    fx_fat(3, F[9], dir2);
    fx_fat(3, dir2, 0xFFFF);
    memset(img + cluster_byte(dir2), 0, g.cluster_bytes);
    char name[12];
    long slots = g.cluster_bytes / 32;
    for (long i = 2; i < slots; i++)
    {
        fmt(name, sizeof(name), "D%06ld TXT", i);
        fx_entry(dir_slot(F[9], i), name, 0x20, 0, 0);
    }
    fx_scan_free();
    mount();
    int fd = file_open("0:/DIR/NOTE.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0, "create in a two-cluster directory returned %d", fd);
    fclose(fd);
    CHECK(entry_pos_of(dir_slot(dir2, 0), slots, "NOTE    TXT") == dir_slot(dir2, 0), "not in the second cluster's first slot");
    CHECK(file_open("0:/DIR/NOTE.TXT", FILE_MODE_READ) > 0, "not found again");
    CHECK(live_ok("two-cluster directory", 1), "");
    // Writes to existing files whose entries are in the second cluster (slots 0 and 1 there):
    // "." and ".." in the first cluster's slots 0 and 1, the D entries and the other file's slot
    // stay as they were.
    fd = file_open("0:/DIR/NOTE2.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0, "second create in the two-cluster directory returned %d", fd);
    fclose(fd);
    CHECK(entry_pos_of(dir_slot(dir2, 0), slots, "NOTE2   TXT") == dir_slot(dir2, 1),
          "NOTE2.TXT not in the second cluster's second slot");
    long cl[2] = { F[9], dir2 };
    unsigned char *before = malloc(2 * g.cluster_bytes);
    if (dir_write("0:/DIR/NOTE.TXT", FILE_MODE_APPEND, SEED_NEW, 10, dir_slot(dir2, 0), cl, before) ||
        dir_write("0:/DIR/NOTE.TXT", FILE_MODE_WRITE, SEED_RO, 70000, dir_slot(dir2, 0), cl, before) ||
        dir_write("0:/DIR/NOTE2.TXT", FILE_MODE_APPEND, SEED_LONG, 20, dir_slot(dir2, 1), cl, before))
    {
        return 1;
    }
    free(before);
    EXPECT({ "NOTE    TXT", SEED_RO, { 70000, -1 } }, { "NOTE2   TXT", SEED_LONG, { 20, -1 } });
    CHECK(live_ok("writes in the second directory cluster", 1), "");
    // Now one cluster only, full.
    fx_fat(3, F[9], 0xFFFF);
    fx_fat(3, dir2, 0);
    fx_scan_free();
    mount();
    int writes = ledger_writes();
    CHECK(file_open("0:/DIR/MORE.TXT", FILE_MODE_WRITE) == -ENOSPC, "full directory not -ENOSPC");
    CHECK(ledger_writes() == writes && !fat_img(0, dir2), "the full directory grew or something was written");
    return 0;
}

// Slots: deleted ones are reused, but not a deleted LFN slot, not a slot right after an LFN entry,
// and a 0x00 slot whose next slot isn't 0x00 is refused (-EIO, nothing written). A volume label
// or an LFN entry that reads like the name doesn't match it.
static int c_slots(void)
{
    int r = root_first_free + 6;
    fx_entry(root_slot(r), "\xe5" "ELETED TXT", 0x0F, 0, 0);      // deleted LFN slot: never reused
    fx_entry(root_slot(r + 1), "LFNNAME TXT", 0x0F, 0, 0);        // active LFN entry
    fx_entry(root_slot(r + 2), "\xe5" "FTERLFNTXT", 0x20, 0, 0);  // deleted, right after the LFN
    fx_entry(root_slot(r + 3), "SHORT   TXT", 0x20, 0, 0);        // the LFN's short entry
    fx_entry(root_slot(r + 4), "VOLNAME TXT", 0x08, 0, 0);        // a volume label
    fx_entry(root_slot(r + 5), "\xe5" "SED     TXT", 0x20, 0, 0); // deleted: the slot to reuse
    mount();
    CHECK(file_open("0:/LFNNAME.TXT", FILE_MODE_READ) == -ENOENT, "an LFN entry matched");
    CHECK(file_open("0:/VOLNAME.TXT", FILE_MODE_READ) == -ENOENT, "a volume label matched");
    int fd = file_open("0:/VOLNAME.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0, "create next to a label returned %d", fd);
    fclose(fd);
    CHECK(entry_pos_of(root_slot(0), g.root_entries, "VOLNAME TXT") == root_slot(r + 4) &&
          img[root_slot(r + 4) + 11] == 0x08, "the label was changed");
    long pos = -1;
    for (int i = 0; i < g.root_entries && pos < 0; i++)
    {
        if (!memcmp(img + root_slot(i), "VOLNAME TXT", 11) && img[root_slot(i) + 11] == 0x20)
        {
            pos = i;
        }
    }
    CHECK(pos == r + 5, "the new file is in slot %ld, expected the deleted slot %d", pos, r + 5);
    fd = file_open("0:/SECOND.TXT", FILE_MODE_WRITE);
    fclose(fd);
    CHECK(img[root_slot(r + 6)] == 'S', "the next create did not take the first 0x00 slot");
    CHECK(img[root_slot(r)] == 0xE5 && img[root_slot(r + 2)] == 0xE5, "a protected deleted slot was used");
    // A 0x00 slot followed by an entry: refused, nothing written.
    fx_entry(root_slot(r + 8), "JUNK    TXT", 0x20, 0, 0);
    mount();
    int writes = ledger_writes();
    CHECK(file_open("0:/THIRD.TXT", FILE_MODE_WRITE) == -EIO, "a 0x00 slot before an entry was used");
    CHECK(ledger_writes() == writes, "the refused create wrote");
    return 0;
}

// A long-name entry right before the end marker with no free slot before it: a broken directory,
// so create gives -EIO (not -ENOSPC) and writes nothing; writing an existing file still works.
// With a deleted slot before the long-name entry, create uses that slot.
static int c_lfn_at_end(void)
{
    int r = root_first_free + 6;
    fx_entry(root_slot(r), "ORPHAN  TXT", 0x0F, 0, 0);
    mount();
    int writes = ledger_writes();
    int res = file_open("0:/NEW.TXT", FILE_MODE_WRITE);
    CHECK(res == -EIO, "create after an orphan long-name entry returned %d, expected %d", res, -EIO);
    res = file_open("0:/NEW.TXT", FILE_MODE_APPEND);
    CHECK(res == -EIO, "create (APPEND) after an orphan long-name entry returned %d, expected %d", res, -EIO);
    CHECK(ledger_writes() == writes && pend_count == 0, "the refused create wrote");
    CHECK(no_descriptors(), "descriptors left open");
    int fd = file_open("0:/EMPTY.TXT", FILE_MODE_APPEND);
    fill(SEED_NEW, 0, 10);
    CHECK(fd > 0 && file_write(fd, buf, 10) == 10, "write to an existing file after the refused create");
    fclose(fd);
    mount();
    CHECK(content_ok("0:/EMPTY.TXT", SEED_NEW, 10), "");
    // A deleted slot before the long-name entry: that slot is used.
    fx_entry(root_slot(r), "\xe5" "ELETED TXT", 0x20, 0, 0);
    fx_entry(root_slot(r + 1), "ORPHAN  TXT", 0x0F, 0, 0);
    mount();
    fd = file_open("0:/NEW.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0, "create with a deleted slot before the long-name entry returned %d", fd);
    fclose(fd);
    CHECK(entry_pos_of(root_slot(0), g.root_entries, "NEW     TXT") == root_slot(r), "not in the deleted slot");
    CHECK(img[root_slot(r + 1) + 11] == 0x0F && img[root_slot(r + 2)] == 0, "the long-name entry or the end marker changed");
    return 0;
}

// Capacity from IDENTIFY: only clusters wholly inside the reported sectors are used.
// QEMU-style capacity (the image rounded up): the last complete cluster is used, the partial one
// after it never. A smaller capacity: allocation stops at its last complete cluster.
static int c_capacity(void)
{
    // Every free cluster but max_writable taken by FILL.BIN.
    long n = 0;
    static long chain[70000];
    for (int i = 0; i < free_count; i++)
    {
        if (freelist[i] != g.max_writable)
        {
            chain[n++] = freelist[i];
        }
    }
    for (long i = 0; i < n; i++)
    {
        fx_fat(3, chain[i], i + 1 < n ? chain[i + 1] : 0xFFFF);
    }
    fx_entry(root_slot(root_first_free + 6), "FILL    BIN", 0x20, chain[0], (unsigned int)(n * g.cluster_bytes));
    CHECK(!fat_img(0, g.max_writable + 1) && g.max_writable + 1 <= g.bpb_max,
          "fixture: cluster %ld after the last complete one is not free in the BPB range", g.max_writable + 1);
    fx_scan_free();
    mount();
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    fill(SEED_NEW, 0, 65536);
    CHECK(file_write(fd, buf, 65536) == 65536, "the last complete cluster not usable");
    long got[4];
    CHECK(chain_of("NEW     BIN", got) == 1 && got[0] == g.max_writable, "NEW.BIN is not in cluster %ld", g.max_writable);
    int writes = ledger_writes();
    CHECK(file_write(fd, buf, 1) == -ENOSPC, "the partial cluster %ld was offered", g.max_writable + 1);
    CHECK(ledger_writes() == writes && !fat_img(0, g.max_writable + 1), "the refused write wrote");
    CHECK(file_seek(fd, 0, SEEK_CUR) == 65536 && file_size(fd) == 65536, "position or size moved");
    fclose(fd);
    EXPECT({ "NEW     BIN", SEED_NEW, { 65536, -1 } });
    CHECK(live_ok("capacity", 1), "");
    return 0;
}

static int c_small_capacity(void)
{
    // The reported drive ends inside the cluster after F12: F0..F12 usable, F13 onwards not.
    capacity = (unsigned int)(g.data_sector + (F[12] - 1) * g.spc + 5);
    long limit = max_writable_for(capacity);
    CHECK(limit == F[12], "fixture: limit %ld", limit);
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    // Free up to F12: F0 F2 F5 F7 F12 = 5 clusters.
    int writes = ledger_writes();
    fill(SEED_NEW, 0, 6 * 65536);
    CHECK(file_write(fd, buf, 6 * 65536) == -ENOSPC, "6 clusters fit below a smaller capacity");
    CHECK(ledger_writes() == writes, "the refused write wrote");
    CHECK(file_write(fd, buf, 5 * 65536) == 5 * 65536, "5 clusters do not fit");
    long got[8];
    CHECK(chain_of("NEW     BIN", got) == 5 && got[4] == F[12], "chain does not end at %ld", F[12]);
    fclose(fd);
    EXPECT({ "NEW     BIN", SEED_NEW, { 5 * 65536, -1 } });
    CHECK(live_ok("smaller capacity", 1), "");
    return 0;
}

// A directory whose cluster lies past the reported capacity: the drive refuses the slot write
// before any port, so create gives -EIO with nothing written, but writing stays on for the rest
// of the disk (a real write or flush failure turns it off: see the failure sweeps).
static int c_slot_past_capacity(void)
{
    // The reported drive ends where DIR's cluster F9 starts: F0 F2 F5 F7 are usable.
    capacity = (unsigned int)(g.data_sector + (F[9] - 2) * g.spc);
    CHECK(max_writable_for(capacity) == F[9] - 1, "fixture: limit %ld", max_writable_for(capacity));
    int writes = ledger_writes();
    int res = file_open("0:/DIR/NOTE.TXT", FILE_MODE_WRITE);
    CHECK(res == -EIO, "create in a directory past the capacity returned %d, expected %d", res, -EIO);
    CHECK(ledger_writes() == writes && pend_count == 0, "the refused create wrote");
    CHECK(no_descriptors(), "descriptors left open");
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "create in the root after the refused create returned %d", fd);
    fill(SEED_NEW, 0, 2 * 65536);
    res = file_write(fd, buf, 2 * 65536);
    CHECK(res == 2 * 65536, "write after the refused create returned %d", res);
    fclose(fd);
    fd = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    fill(SEED_OLD, 70000, 10);
    CHECK(fd > 0 && file_write(fd, buf, 10) == 10, "append after the refused create");
    fclose(fd);
    CHECK(file_open("0:/DIR/NOTE.TXT", FILE_MODE_WRITE) == -EIO, "second create past the capacity");
    mount();
    CHECK(content_ok("0:/NEW.BIN", SEED_NEW, 2 * 65536) && content_ok("0:/OLD.BIN", SEED_OLD, 70010), "");
    long got[4];
    CHECK(chain_of("NEW     BIN", got) == 2 && got[0] == F[0] && got[1] == F[2], "NEW.BIN not in F0 F2");
    EXPECT({ "NEW     BIN", SEED_NEW, { 2 * 65536, -1 } }, { "OLD     BIN", SEED_OLD, { 70010, -1 } },
           { "NOTE    TXT", 0, { -2 } });
    CHECK(live_ok("directory past the capacity", 1), "");
    return 0;
}

// A drive bigger than 2 GiB: IDENTIFY reports the BPB's whole size (7,812,500 sectors, like the
// README's padded fsck copy booted in QEMU), the BPB allows clusters up to 61030, but byte
// positions are ints, so only clusters that end inside INT_MAX / 512 sectors are written: up to
// 32763 for the shipped layout. The model's disk grows to those 2 GiB (sparse) for this case.
// FILL.BIN takes every free cluster below the last three allowed ones.
static int c_int_max(void)
{
    long sectors = INT_MAX_VALUE / SECTOR;
    long last = (sectors - g.data_sector) / g.spc + 1;
    unsigned int bpb_sectors = le16(img + 19) ? le16(img + 19) : le32(img + 32);
    CHECK(bpb_sectors > sectors && last + 2 <= g.bpb_max,
          "fixture: the BPB (%u sectors, clusters up to %ld) does not reach past cluster %ld", bpb_sectors, g.bpb_max, last);
    CHECK(pend_count == 0, "fixture: pending writes");
    long big = sectors + 1;
    unsigned char *m = mmap(0, big * SECTOR, 3, 0x4022, -1, 0); // PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE
    CHECK(m != (void *)-1, "no memory for a %ld-sector disk", big);
    memcpy(m, img, img_sectors * SECTOR);
    img = m;
    img_sectors = big;
    pend = malloc(big * sizeof(*pend));
    memset(pend, 0, big * sizeof(*pend));
    pend_list = malloc(big * sizeof(int));
    view = malloc(big * sizeof(*view));
    static long chain[70000];
    long n = 0;
    for (long cl = 2; cl <= last - 3; cl++)
    {
        if (!fat_img(0, cl) && !fat_img(1, cl))
        {
            chain[n++] = cl;
        }
    }
    for (long i = 0; i < n; i++)
    {
        fx_fat(3, chain[i], i + 1 < n ? chain[i + 1] : 0xFFFF);
    }
    fx_entry(root_slot(root_first_free + 6), "FILL    BIN", 0x20, chain[0], (unsigned int)(n * g.cluster_bytes));
    capacity = bpb_sectors;
    g.max_writable = last;
    fx_scan_free();
    CHECK(free_count == 3 && freelist[0] == last - 2 && freelist[2] == last && !fat_img(0, last + 1) && !fat_img(0, last + 2),
          "fixture: clusters %ld-%ld are not the only free ones up to %ld", last - 2, last + 2, last + 2);
    mount();
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "create returned %d", fd);
    int writes = ledger_writes();
    fill(SEED_NEW, 0, 4 * 65536);
    int res = file_write(fd, buf, 4 * 65536);
    CHECK(res == -ENOSPC, "a write needing clusters %ld-%ld (past INT_MAX) returned %d", last - 2, last + 1, res);
    CHECK(ledger_writes() == writes && pend_count == 0, "the refused write wrote");
    CHECK(file_seek(fd, 0, SEEK_CUR) == 0 && file_size(fd) == 0, "position or size moved");
    res = file_write(fd, buf, 3 * 65536);
    CHECK(res == 3 * 65536, "a write into the last three clusters below INT_MAX returned %d", res);
    long got[8];
    CHECK(chain_of("NEW     BIN", got) == 3 && got[0] == last - 2 && got[2] == last,
          "NEW.BIN is not in clusters %ld-%ld", last - 2, last);
    writes = ledger_writes();
    CHECK(file_write(fd, buf, 1) == -ENOSPC, "cluster %ld (past INT_MAX) was offered", last + 1);
    CHECK(ledger_writes() == writes && !fat_img(0, last + 1), "the refused write wrote");
    fclose(fd);
    mount();
    CHECK(content_ok("0:/NEW.BIN", SEED_NEW, 3 * 65536), "");
    EXPECT({ "NEW     BIN", SEED_NEW, { 3 * 65536, -1 } }, { "FILL    BIN", -1, { -1 } });
    expects[1].sizes[0] = n * g.cluster_bytes;
    expects[1].sizes[1] = -1;
    CHECK(live_ok("drive past INT_MAX", 1), "");
    shared->value = last;
    return 0;
}

// Geometry the writer refuses (reading still works): four FAT copies (128 sectors each, so the
// root and data stay where they are and the first two copies still match), or hidden sectors.
static int refused_geometry(const char *what)
{
    mount();
    CHECK(content_ok("0:/OLD.BIN", SEED_OLD, 70000), "");
    int res = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    CHECK(res == -EIO, "%s: create returned %d, expected %d", what, res, -EIO);
    res = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    CHECK(res == -EIO, "%s: append open returned %d, expected %d", what, res, -EIO);
    CHECK(ledger_writes() == 0 && pend_count == 0, "%s: something was written", what);
    CHECK(no_descriptors(), "%s: descriptors left open", what);
    return 0;
}

static int c_fat_copies(void)
{
    CHECK(g.fats == 2 && g.spf == 256 && !le16(img + 19), "fixture: not 2 FATs of 256 sectors");
    long fat = g.reserved * SECTOR, copy = 128 * SECTOR;
    for (int i = 1; i < 4; i++)
    {
        memcpy(img + fat + i * copy, img + fat, copy);
    }
    img[16] = 4;
    img[22] = 128;
    img[23] = 0;
    unsigned int total = (unsigned int)(g.data_sector + 32000 * g.spc); // 32000 clusters fit 128-sector FATs
    img[32] = total & 0xFF;
    img[33] = (total >> 8) & 0xFF;
    img[34] = (total >> 16) & 0xFF;
    img[35] = total >> 24;
    return refused_geometry("4 FAT copies");
}

static int c_hidden_sectors(void)
{
    img[28] = 1;
    return refused_geometry("1 hidden sector");
}

// No space: a write that needs more clusters than are free is refused before anything is written,
// keeps the position, and doesn't stop later writes.
static int c_enospc(void)
{
    // Take every free cluster but two.
    static long chain[70000];
    long n = free_count - 2;
    for (long i = 0; i < n; i++)
    {
        chain[i] = freelist[i + 2];
        fx_fat(3, chain[i], i + 1 < n ? freelist[i + 3] : 0xFFFF);
    }
    fx_entry(root_slot(root_first_free + 6), "FILL    BIN", 0x20, chain[0], (unsigned int)(n * g.cluster_bytes));
    fx_scan_free();
    mount();
    int fd = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    int writes = ledger_writes();
    fill(SEED_OLD, 70000, 4 * 65536);
    CHECK(file_write(fd, buf, 4 * 65536) == -ENOSPC, "a write needing 4 more clusters with 2 free");
    CHECK(ledger_writes() == writes, "the refused write wrote");
    CHECK(file_seek(fd, 0, SEEK_CUR) == 70000 && file_size(fd) == 70000, "position or size moved");
    CHECK(file_write(fd, buf, 2 * 65536) == 2 * 65536, "a write that fits failed after -ENOSPC");
    fclose(fd);
    EXPECT({ "OLD     BIN", SEED_OLD, { 70000 + 2 * 65536, -1 } });
    CHECK(live_ok("no space", 1), "");
    return 0;
}

// FATs 0 and 1 entries free (0): never allocated; data starts at cluster 2.
static int c_reserved_entries(void)
{
    fx_fat(3, 0, 0);
    fx_fat(3, 1, 0);
    mount();
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    fill(SEED_NEW, 0, 3 * 65536);
    CHECK(file_write(fd, buf, 3 * 65536) == 3 * 65536, "write");
    fclose(fd);
    long got[8];
    CHECK(chain_of("NEW     BIN", got) == 3 && got[0] == F[0] && got[0] >= 2 && got[1] >= 2 && got[2] >= 2,
          "cluster 0 or 1 allocated");
    CHECK(!fat_img(0, 0) && !fat_img(0, 1), "entries 0 and 1 written");
    return 0;
}

// The FAT copies differ: no mutation at all (-EIO), reads still work.
static int c_mirror(void)
{
    fx_fat(2, F[20], 0xFFFF);
    mount();
    CHECK(file_open("0:/NEW.BIN", FILE_MODE_WRITE) == -EIO, "create with different FATs");
    CHECK(file_open("0:/OLD.BIN", FILE_MODE_APPEND) == -EIO, "append with different FATs");
    CHECK(file_open("0:/TRUNC.BIN", FILE_MODE_WRITE) == -EIO, "truncate with different FATs");
    CHECK(ledger_writes() == 0, "%d sector writes", ledger_writes());
    CHECK(content_ok("0:/TRUNC.BIN", SEED_TRUNC, 140000), "");
    return 0;
}

// Broken chains: refused before anything is written. An overlong chain is refused for APPEND but
// freed whole by WRITE.
static int broken(const char *what, long size, const long *links, int nlinks, int truncate_ok)
{
    long first = links[0];
    for (int i = 0; i < nlinks - 1; i++)
    {
        fx_fat(3, links[i], links[i + 1]);
    }
    fx_entry(root_slot(root_first_free + 6), "BROKEN  BIN", 0x20, first, size);
    mount();
    CHECK(file_open("0:/BROKEN.BIN", FILE_MODE_APPEND) == -EIO, "%s: APPEND not -EIO", what);
    int fd = file_open("0:/BROKEN.BIN", FILE_MODE_WRITE);
    if (truncate_ok)
    {
        CHECK(fd > 0, "%s: WRITE returned %d", what, fd);
        for (int i = 0; i < nlinks - 1; i++)
        {
            CHECK(!fat_img(0, links[i]) && !fat_img(1, links[i]), "%s: cluster %ld not freed", what, links[i]);
        }
        return 0;
    }
    CHECK(fd == -EIO, "%s: WRITE returned %d, expected -EIO", what, fd);
    CHECK(ledger_writes() == 0, "%s: %d sector writes", what, ledger_writes());
    return 0;
}

static int c_loop(void) { long l[] = { F[20], F[21], F[20] }; return broken("loop", 3 * 65536, l, 3, 0); }
static int c_short(void) { long l[] = { F[20], 0xFFFF }; return broken("chain shorter than the size", 3 * 65536, l, 2, 0); }
static int c_to_free(void) { long l[] = { F[20], 0 }; return broken("link to a free cluster", 70000, l, 2, 0); }
static int c_to_bad(void) { long l[] = { F[20], 0xFFF7 }; return broken("link to a bad cluster", 70000, l, 2, 0); }
static int c_past(void) { long l[] = { F[20], g.max_writable + 1, 0xFFFF }; fx_fat(3, g.max_writable + 1, 0xFFFF); return broken("cluster past the device", 70000, l, 3, 0); }
static int c_first0(void) { long l[] = { 0 }; return broken("size without a cluster", 100, l, 1, 0); }
static int c_first1(void) { long l[] = { 1 }; return broken("first cluster 1", 100, l, 1, 0); }
static int c_overlong(void) { long l[] = { F[20], F[21], F[22], 0xFFFF }; return broken("overlong chain", 100, l, 4, 1); }

// Zero-size file that still owns a cluster: APPEND refused, WRITE frees the cluster.
static int c_empty_owner(void) { long l[] = { F[20], 0xFFFF }; return broken("size 0 with a cluster", 0, l, 2, 1); }

// Every allocation of a WRITE open failing in turn (existing file, then a new one): a failed open
// writes nothing (the file stays whole, the name stays absent), leaves no allocation and no
// descriptor behind. Also with all 512 descriptors in use.
static int c_open_oom(void)
{
    int tries = 0;
    for (int which = 0; which < 2; which++)
    {
        const char *path = which ? "0:/DIR/NEW.BIN" : "0:/TRUNC.BIN";
        for (int n = 1; n < 200; n++)
        {
            mount();
            int live = kalloc_live();
            ledger_count = 0;
            kalloc_fail_at(kalloc_calls() + n);
            int fd = file_open(path, FILE_MODE_WRITE);
            int failed = kalloc_failed();
            kalloc_fail_at(0);
            if (!failed)
            {
                CHECK(fd > 0, "%s: open without a failed allocation returned %d", path, fd);
                fclose(fd);
                break;
            }
            tries++;
            CHECK(fd < 0, "%s: allocation %d failed but the open succeeded", path, n);
            CHECK(ledger_writes() == 0, "%s: allocation %d failed after %d sector writes", path, n, ledger_writes());
            CHECK(no_descriptors() && heap_ok(path, live), "%s: allocation %d", path, n);
        }
    }
    // All descriptors taken: the open fails before truncating.
    mount();
    int count = 0;
    while (fopen_status("0:/LONGNAME.TXT", "r") > 0)
    {
        count++;
    }
    ledger_count = 0;
    CHECK(count == RORTHOX_MAX_FILE_DESCRIPTORS, "%d descriptors", count);
    CHECK(file_open("0:/OLD.BIN", FILE_MODE_WRITE) < 0, "open with no descriptor left");
    CHECK(file_open("0:/OTHER.BIN", FILE_MODE_WRITE) < 0, "create with no descriptor left");
    CHECK(ledger_writes() == 0, "truncated or created with no descriptor left");
    shared->value = tries;
    EXPECT({ "OLD     BIN", SEED_OLD, { 70000, -1 } }, { "TRUNC   BIN", -1, { 0, -1 } },
           { "OTHER   BIN", 0, { -2 } });
    CHECK(live_ok("open allocation failures", 1), "");
    return 0;
}

// The append of c_append_grow with every write, flush and read failing in turn. A failed write or
// flush returns -EIO and stops all later changes (also creates and truncates, which then write
// nothing); reads still work. Both "pending lost" and "pending kept" pass the crash checks.
static int sweep_run(int kind, int n)
{
    EXPECT({ "OLD     BIN", SEED_OLD, { 70000, 270000, -1 } }, { "TRUNC   BIN", SEED_TRUNC, { 140000, -1 } });
    int fd = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    CHECK(fd > 0, "open");
    n_writes = n_flushes = n_reads = 0;
    fail_write_n = kind == 0 ? n : 0;
    fail_flush_n = kind == 1 ? n : 0;
    fail_read_n = kind == 2 ? n : 0;
    fill(SEED_OLD, 70000, 200000);
    int res = file_write(fd, buf, 200000);
    int w = n_writes, f = n_flushes, r = n_reads;
    fail_write_n = fail_flush_n = fail_read_n = 0;
    if (kind < 0)
    {
        shared->writes = w;
        shared->flushes = f;
        shared->reads = r;
        return res == 200000 ? 0 : 1;
    }
    const char *names[] = { "write", "flush", "read" };
    CHECK(res < 0, "%s %d failed but the write returned %d", names[kind], n, res);
    CHECK(file_seek(fd, 0, SEEK_CUR) == 70000, "%s %d: position moved", names[kind], n);
    if (kind < 2 || ledger_writes())
    {
        CHECK(res == -EIO, "%s %d: returned %d, expected -EIO", names[kind], n, res);
        int before = ledger_count;
        CHECK(file_write(fd, buf, 1) == -EIO, "%s %d: a later write was not refused", names[kind], n);
        CHECK(file_open("0:/NEW.BIN", FILE_MODE_WRITE) == -EIO, "%s %d: a later create was not refused", names[kind], n);
        CHECK(file_open("0:/TRUNC.BIN", FILE_MODE_WRITE) == -EIO, "%s %d: a later truncate was not refused", names[kind], n);
        CHECK(ledger_count == before, "%s %d: something was written after the failure", names[kind], n);
    }
    fclose(fd);
    CHECK(content_ok("0:/TRUNC.BIN", SEED_TRUNC, 140000), "");
    // A crash now: the pending sectors lost, or all of them kept.
    for (int keep = 0; keep < 2; keep++)
    {
        for (long s = 0; s < img_sectors; s++)
        {
            view[s] = keep ? live_sector(s) : img + s * SECTOR;
        }
        if (!check_volume(0))
        {
            FAIL("%s %d failed, crash with pending %s: %s", names[kind], n, keep ? "kept" : "lost", why);
        }
    }
    return 0;
}

static int c_count_run(void) { return sweep_run(-1, 0); }

// One process for the whole save scenario: the script checks its images with mtools/fsck.fat.
static const char *out_dir;

static int save_file(const char *name, const unsigned char *data, long size)
{
    char path[512];
    fmt(path, sizeof(path), "%s/%s", out_dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long done = 0;
    while (fd >= 0 && done < size)
    {
        long n = write(fd, data + done, size - done > (1L << 20) ? (1L << 20) : size - done);
        if (n <= 0)
        {
            break;
        }
        done += n;
    }
    if (fd >= 0)
    {
        close(fd);
    }
    return done == size;
}

static int c_save(void)
{
    CHECK(save_file("fixture.img", img, file_bytes), "cannot save fixture.img");
    int fd = file_open("0:/NEW.BIN", FILE_MODE_WRITE);
    fill(SEED_NEW, 0, 140000);
    CHECK(file_write(fd, buf, 140000) == 140000, "write NEW.BIN");
    fclose(fd);
    fd = file_open("0:/OLD.BIN", FILE_MODE_APPEND);
    fill(SEED_OLD, 70000, 200000);
    CHECK(file_write(fd, buf, 200000) == 200000, "append OLD.BIN");
    fclose(fd);
    fd = file_open("0:/TRUNC.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "truncate");
    fclose(fd);
    fd = file_open("0:/DIR/note.txt", FILE_MODE_APPEND);
    fill(SEED_NEW, 0, 7);
    CHECK(file_write(fd, buf, 7) == 7, "write DIR/NOTE.TXT");
    fclose(fd);
    CHECK(pend_count == 0, "pending writes left");
    CHECK(save_file("written.img", img, file_bytes), "cannot save written.img");
    fill(SEED_NEW, 0, 140000);
    CHECK(save_file("expect-NEW.BIN", buf, 140000), "save");
    fill(SEED_OLD, 0, 270000);
    CHECK(save_file("expect-OLD.BIN", buf, 270000), "save");
    fill(SEED_NEW, 0, 7);
    CHECK(save_file("expect-NOTE.TXT", buf, 7), "save");
    fill(SEED_LONG, 0, 50);
    CHECK(save_file("expect-LONGNAME.TXT", buf, 50), "save");
    EXPECT({ "NEW     BIN", SEED_NEW, { 140000, -1 } }, { "OLD     BIN", SEED_OLD, { 270000, -1 } },
           { "TRUNC   BIN", -1, { 0, -1 } }, { "NOTE    TXT", SEED_NEW, { 7, -1 } });
    CHECK(live_ok("save", 1), "");
    return 0;
}

// ---------------------------------------------------------------------------------------------

struct test_case
{
    const char *name;
    int (*run)(void);
};

static const struct test_case cases[] = {
    { "create and grow (crash cuts)", c_create_grow },
    { "append and grow (crash cuts)", c_append_grow },
    { "truncate and reuse (crash cuts)", c_truncate },
    { "overwrite inside the file", c_overwrite },
    { "two appenders and a reader", c_two_appenders },
    { "truncate under open descriptors", c_truncate_others },
    { "root cache and subdirectory", c_root_cache },
    { "8.3 names", c_names },
    { "targets", c_targets },
    { "modes, zero counts, seek", c_modes_seek },
    { "full root", c_full_root },
    { "directory chain, full directory", c_dir_chain },
    { "slots, LFN, label", c_slots },
    { "long-name entry before the end marker", c_lfn_at_end },
    { "capacity: last complete cluster", c_capacity },
    { "capacity: smaller drive", c_small_capacity },
    { "capacity: directory past the drive", c_slot_past_capacity },
    { "capacity: drive past INT_MAX bytes", c_int_max },
    { "geometry: four FAT copies", c_fat_copies },
    { "geometry: hidden sectors", c_hidden_sectors },
    { "no space", c_enospc },
    { "FAT entries 0 and 1", c_reserved_entries },
    { "FAT copies differ", c_mirror },
    { "chain loop", c_loop },
    { "chain too short", c_short },
    { "link to a free cluster", c_to_free },
    { "link to a bad cluster", c_to_bad },
    { "cluster past the device", c_past },
    { "size without a cluster", c_first0 },
    { "first cluster 1", c_first1 },
    { "overlong chain", c_overlong },
    { "size 0 with a cluster", c_empty_owner },
    { "allocation failures in WRITE opens", c_open_oom },
    { "save images for mtools", c_save },
};

static int run_case(int (*run)(void), int kind, int n)
{
    fflush(0);
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(120);
        kalloc_init();
        capacity = (unsigned int)img_sectors;
        g.max_writable = max_writable_for(capacity);
        mount();
        ledger_count = 0;
        int res = run ? run() : sweep_run(kind, n);
        if (!res && (kalloc_bad_frees() || kalloc_poison_errors()))
        {
            printf("    bad free or write after free\n");
            res = 1;
        }
        fflush(0);
        _exit(res ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : (WIFSIGNALED(status) ? 2 : 1);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: fat-write <image> <outdir>\n");
        return 2;
    }
    out_dir = argv[2];
    int fd = open(argv[1], O_RDONLY);
    file_bytes = fd < 0 ? -1 : lseek(fd, 0, 2);
    img_sectors = (file_bytes + SECTOR - 1) / SECTOR;
    img = malloc(img_sectors * SECTOR);
    memset(img, 0, img_sectors * SECTOR);
    if (fd < 0 || pread(fd, img, file_bytes, 0) != file_bytes || img[38] != 0x29)
    {
        printf("fat-write: %s is not a FAT16 image\n", argv[1]);
        return 2;
    }
    close(fd);
    pend = malloc(img_sectors * sizeof(*pend));
    memset(pend, 0, img_sectors * sizeof(*pend));
    pend_list = malloc(img_sectors * sizeof(int));
    view = malloc(img_sectors * sizeof(*view));
    buf = malloc(1 << 20);
    shared = mmap(0, 4096, 3, 0x21, -1, 0); // PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS
    geometry();
    for (root_first_free = 0; root_first_free < g.root_entries && img[root_slot(root_first_free)]; root_first_free++)
    {
    }
    capacity = (unsigned int)img_sectors;
    g.max_writable = max_writable_for(capacity);
    // The fixture is written once here; every case gets its own copy through fork().
    fixture();
    printf("fat-write: %ld sectors, data from sector %ld, %ld-byte clusters, BPB clusters up to %ld, "
           "writable up to %ld\n", img_sectors, g.data_sector, g.cluster_bytes, g.bpb_max, g.max_writable);

    int total = sizeof(cases) / sizeof(cases[0]);
    int failed = 0;
    for (int i = 0; i < total; i++)
    {
        int res = run_case(cases[i].run, 0, 0);
        if (res)
        {
            printf("  FAIL %s%s\n", cases[i].name, res == 2 ? " (killed)" : "");
            failed++;
        }
        if (cases[i].run == c_open_oom)
        {
            printf("fat-write: %d allocation failures in WRITE opens\n", shared->value);
        }
        if (cases[i].run == c_int_max && !res)
        {
            printf("fat-write: a drive past INT_MAX bytes is written up to cluster %d\n", shared->value);
        }
    }
    // Failure sweeps over the append of c_append_grow.
    total++;
    if (run_case(c_count_run, 0, 0))
    {
        printf("  FAIL counting run of the append\n");
        failed++;
    }
    int counts[3] = { shared->writes, shared->flushes, shared->reads };
    const char *names[3] = { "write", "flush", "read" };
    const char *plurals[3] = { "writes", "flushes", "reads" };
    printf("fat-write: the append makes %d disk writes, %d flushes, %d reads\n", counts[0], counts[1], counts[2]);
    for (int kind = 0; kind < 3; kind++)
    {
        int bad = 0;
        for (int n = 1; n <= counts[kind]; n++)
        {
            if (run_case(0, kind, n))
            {
                bad++;
            }
        }
        total++;
        if (bad || !counts[kind])
        {
            printf("  FAIL %s failures: %d of %d failed\n", names[kind], bad, counts[kind]);
            failed++;
        }
        else
        {
            printf("fat-write: each of the %d %s failed in turn\n", counts[kind], plurals[kind]);
        }
    }
    printf("fat-write: %ld crash states checked\n", shared->states);
    printf("fat-write host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
