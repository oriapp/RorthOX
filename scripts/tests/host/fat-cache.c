/*
 * Host check for scripts/tests/fat-cache.sh: the FAT sector cache of kernel/fs/fat/fat16.c,
 * run on the image disk model (fat-image.c), so every physical sector read is counted
 * (fimg_read_count). The check includes fat16.c to call its static FAT entry functions
 * (fat16_get_fat_entry, fat16_set_entry, fat16_prepare_write, fat16_write_at, fat16_flush); the
 * files are written and read through the real file.c.
 *
 * Only physical reads are observed, never the cache's fields: "the cache is valid" means a
 * lookup is served without a read, "the cache was dropped" means the next lookup reads its
 * sector again. Clusters A and B (= A + 256) have their entries at the same offset of two
 * different sectors of the first FAT, with different values in both copies.
 *
 *   one read per sector   256 entries of one FAT sector looked up 4 times: one read of that
 *                         sector and nothing else; a 520-cluster file read with one file_read
 *                         reads each FAT sector at most once per run of links in it
 *   eviction              A, B, A: three reads with the right values; the file's chain relinked
 *                         so that every link goes to the other half (another FAT sector) reads
 *                         back in that order, with at most one read per link
 *   failed fill           a lookup whose sector read fails returns the read's error; the next
 *                         lookup reads its sector (the old one or the failed one) and gives its
 *                         value, also when the cache was cold
 *   both copies           after fat16_set_entry on the first copy, the next lookup reads the
 *                         sector and gives the new value; after both copies, it still does
 *                         (pending, then flushed: both durable copies hold it); a file
 *                         rewritten after a warm read reads back right, and so does a file
 *                         written with two calls on one handle (the second call follows the
 *                         chain the first one linked)
 *   uncertain write       a failed write (fat16_write_at) and a failed flush drop the cache
 *   preparation           write preparation drops the cache, and a warm first-FAT sector never
 *                         hides a difference between the copies on the disk (either copy changed
 *                         behind the cache): -EIO
 *
 * Usage: fat-cache <image>. Prints one line per failed check and
 * "fat-cache host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "fat-image.h"
#include "fs/fat/fat16.c"

void *malloc(unsigned long size);
unsigned int alarm(unsigned int seconds);

#define S FIMG_SECTOR
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static int checks, bad;
static struct disk *d;
static long reserved, spf, spc, root_lba, root_entries;
static int A, B;

#define BIG_CLUSTERS 520

static unsigned int le16(const unsigned char *p)
{
    return p[0] | (p[1] << 8);
}

static long fat_lba(int copy, int cluster)
{
    return reserved + copy * spf + (cluster * 2L) / S;
}

// The entry of <cluster> in copy <copy> as the disk has it now (pending over durable).
static unsigned int live_entry(int copy, int cluster)
{
    return le16(fimg_view_live()[fat_lba(copy, cluster)] + (cluster * 2) % S);
}

static unsigned int durable_entry(int copy, int cluster)
{
    return le16(fimg_durable() + fat_lba(copy, cluster) * S + (cluster * 2) % S);
}

// Change an entry on the media behind the kernel's back (no ledger record).
static void poke(int copy, int cluster, unsigned int value)
{
    unsigned char *p = fimg_durable() + fat_lba(copy, cluster) * S + (cluster * 2) % S;
    p[0] = value & 0xFF;
    p[1] = value >> 8;
}

static void poke_both(int cluster, unsigned int value)
{
    poke(0, cluster, value);
    poke(1, cluster, value);
}

// Physical reads of the first / second FAT since the last fimg_reset_counts().
static int fat_reads(int copy)
{
    int n = 0;
    for (long s = 0; s < spf; s++)
    {
        n += fimg_read_count(reserved + copy * spf + s);
    }
    return n;
}

static int get(int cluster)
{
    return fat16_get_fat_entry(d, cluster);
}

// A mount with A = 0x1111 and B = 0x2222 in both copies, counters at zero.
static void mount_ab(void)
{
    poke_both(A, 0x1111);
    poke_both(B, 0x2222);
    CHECK(fimg_mount() == 0, "mount");
    d = disk_get(0);
    fimg_reset_counts();
}

// ---------------------------------------------------------------------------------------------
// Files

static unsigned char *big_data, *big_back;
static const int big_bytes = BIG_CLUSTERS * 4 * S;  // spc is checked to be 4 in main

static void fill_big(int seed)
{
    for (int i = 0; i < big_bytes; i++)
    {
        big_data[i] = (unsigned char)(i * 13 + (i >> 11) * 7 + seed);
    }
}

static int write_file(const char *path, const unsigned char *data, int bytes)
{
    int fd = file_open(path, FILE_MODE_WRITE);
    int res = fd > 0 ? file_write(fd, data, bytes) : fd;
    if (fd > 0)
    {
        fclose(fd);
    }
    return res;
}

static int read_file(const char *path, unsigned char *out, int bytes)
{
    int fd = file_open(path, FILE_MODE_READ);
    if (fd <= 0)
    {
        return fd;
    }
    fimg_reset_counts();
    int res = file_read(fd, out, bytes);
    fclose(fd);
    return res;
}

// The chain of root directory entry <name83> on the durable media, in out (at most max).
static int durable_chain(const char *name83, int *out, int max)
{
    const unsigned char *root = fimg_durable() + root_lba * S;
    for (long i = 0; i < root_entries; i++)
    {
        const unsigned char *e = root + i * 32;
        if (memcmp((void *)e, (void *)name83, 11) == 0)
        {
            int n = 0;
            for (unsigned int c = le16(e + 26); c >= 2 && c < 0xFFF0 && n < max; c = durable_entry(0, c))
            {
                out[n++] = c;
            }
            return n;
        }
    }
    return 0;
}

// Reads one-sector-cache lookups need along a chain: one per run of links in the same sector.
static int runs(const int *chain, int n)
{
    int r = 0;
    for (int i = 0; i + 1 < n; i++)
    {
        r += i == 0 || fat_lba(0, chain[i]) != fat_lba(0, chain[i - 1]);
    }
    return r;
}

static int big_chain[BIG_CLUSTERS + 1];

// BIG.BIN written and flushed, the media remounted (cold cache).
static int write_big(void)
{
    CHECK(fimg_mount() == 0, "mount");
    CHECK(write_file("0:/BIG.BIN", big_data, big_bytes) == big_bytes, "write BIG.BIN");
    int n = durable_chain("BIG     BIN", big_chain, BIG_CLUSTERS + 1);
    CHECK(n == BIG_CLUSTERS, "BIG.BIN has %d clusters on the media", n);
    CHECK(fimg_mount() == 0, "mount again");
    d = disk_get(0);
    return n;
}

// ---------------------------------------------------------------------------------------------

static void c_one_read(void)
{
    // A's sector: every entry of it, four times over.
    int first = A - (A % 256);
    static unsigned int want[256];
    mount_ab();
    for (int i = 0; i < 256; i++)
    {
        want[i] = durable_entry(0, first + i);
    }
    int wrong = 0;
    for (int round = 0; round < 4; round++)
    {
        for (int i = 0; i < 256; i++)
        {
            wrong += get(first + i) != (int)want[i];
        }
    }
    CHECK(wrong == 0, "%d of 1024 lookups gave a wrong value", wrong);
    CHECK(fimg_read_count(fat_lba(0, A)) == 1 && fimg_counts.reads == 1,
          "1024 lookups in one FAT sector: %d reads of it, %d reads in all (want 1 and 1)",
          fimg_read_count(fat_lba(0, A)), fimg_counts.reads);

    // A whole file in one read: each FAT sector at most once per run of its links.
    fill_big(1);
    int n = write_big();
    int r = runs(big_chain, n);
    CHECK(read_file("0:/BIG.BIN", big_back, big_bytes) == big_bytes && memcmp(big_back, big_data, big_bytes) == 0,
          "BIG.BIN reads back");
    CHECK(fat_reads(0) <= r && fat_reads(1) == 0,
          "%d-cluster file read: %d first-FAT and %d second-FAT sector reads (want at most %d and 0)",
          n, fat_reads(0), fat_reads(1), r);
    printf("    %d-cluster file: %d first-FAT sector reads for %d links\n", n, fat_reads(0), n - 1);
}

static void c_eviction(void)
{
    mount_ab();
    CHECK(get(A) == 0x1111 && fimg_counts.reads == 1, "A: 0x1111 with one read (%d)", fimg_counts.reads);
    CHECK(get(B) == 0x2222 && fimg_counts.reads == 2, "then B: 0x2222 with one more read (%d)", fimg_counts.reads);
    CHECK(get(A) == 0x1111 && fimg_counts.reads == 3, "then A again: 0x1111 with one more read (%d)", fimg_counts.reads);
    CHECK(get(A) == 0x1111 && get(A + 1) == (int)durable_entry(0, A + 1) && fimg_counts.reads == 3,
          "then A and its neighbour without a read (%d)", fimg_counts.reads);
    CHECK(fimg_read_count(fat_lba(0, A)) == 2 && fimg_read_count(fat_lba(0, B)) == 1, "reads per sector");

    // BIG.BIN relinked: first half and second half alternate, so every link changes FAT sector.
    fill_big(2);
    int n = write_big();
    static int order[BIG_CLUSTERS];
    int h = n / 2;
    for (int i = 0; i < h; i++)
    {
        order[2 * i] = big_chain[i];
        order[2 * i + 1] = big_chain[h + i];
    }
    for (int i = 0; i < n; i++)
    {
        poke_both(order[i], i + 1 < n ? order[i + 1] : 0xFFFF);
    }
    int bad_sectors = 0;
    for (int i = 0; i + 1 < n; i++)
    {
        bad_sectors += fat_lba(0, order[i]) == fat_lba(0, order[i + 1]);
    }
    CHECK(bad_sectors == 0 && h >= 256, "relinked chain: %d links stay in one sector", bad_sectors);
    static unsigned char want[BIG_CLUSTERS * 4 * S];
    int cluster_bytes = 4 * S;
    for (int i = 0; i < n; i++)
    {
        int k = i % 2 == 0 ? i / 2 : h + i / 2;
        memcpy(want + i * cluster_bytes, big_data + k * cluster_bytes, cluster_bytes);
    }
    CHECK(fimg_mount() == 0, "mount the relinked chain");
    d = disk_get(0);
    int res = read_file("0:/BIG.BIN", big_back, big_bytes);
    CHECK(res == big_bytes && memcmp(big_back, want, big_bytes) == 0, "relinked BIG.BIN reads back in chain order (%d)", res);
    CHECK(fat_reads(0) <= runs(order, n) && fat_reads(1) == 0,
          "relinked read: %d first-FAT and %d second-FAT reads (want at most %d and 0)", fat_reads(0), fat_reads(1),
          runs(order, n));
}

static void c_failed_fill(void)
{
    // Warm A, B's fill fails: the next lookup of A reads A's sector again.
    mount_ab();
    CHECK(get(A) == 0x1111, "A");
    fimg_fail_read(1, -ENOMEM);
    CHECK(get(B) == -ENOMEM, "B with its read failing: the read's error");
    fimg_reset_counts();
    CHECK(get(A) == 0x1111 && fimg_read_count(fat_lba(0, A)) == 1,
          "A after the failed fill: 0x1111 from a read of its sector (%d reads)", fimg_read_count(fat_lba(0, A)));

    // Warm A, B's fill fails: the next lookup of B reads B's sector (never A's bytes).
    mount_ab();
    CHECK(get(A) == 0x1111, "A");
    fimg_fail_read(1, -ENOMEM);
    CHECK(get(B) == -ENOMEM, "B with its read failing: the read's error");
    fimg_reset_counts();
    int v = get(B);
    CHECK(v == 0x2222 && fimg_read_count(fat_lba(0, B)) == 1,
          "B after its failed fill: 0x%x from %d reads of its sector (want 0x2222 from 1)", v,
          fimg_read_count(fat_lba(0, B)));

    // Cold cache, A's fill fails: A and then B each read their sector.
    mount_ab();
    fimg_fail_read(1, -EIO);
    CHECK(get(A) == -EIO, "A with its read failing: -EIO");
    fimg_reset_counts();
    v = get(A);
    CHECK(v == 0x1111 && fimg_counts.reads == 1, "A after a cold failed fill: 0x%x with %d reads", v, fimg_counts.reads);
    CHECK(get(B) == 0x2222 && fimg_counts.reads == 2, "then B: one more read");
}

static void c_both_copies(void)
{
    mount_ab();
    CHECK(fat16_prepare_write(d) == 0, "prepare");
    CHECK(get(A) == 0x1111, "warm A");
    for (int copy = 0; copy < 2; copy++)
    {
        CHECK(fat16_set_entry(d, copy, A, 0x3333 + copy) == 0, "set A in copy %d", copy + 1);
        fimg_reset_counts();  // the 2-byte write read its sector first (read-modify-write)
        int v = get(A);
        // Copy 1 holds A's entry: the lookup must read the new sector. Copy 2 is not what lookups
        // read, so only the value is checked there.
        CHECK(v == 0x3333 && (copy == 1 || fimg_read_count(fat_lba(0, A)) == 1),
              "A after the write of copy %d: 0x%x from %d reads (want 0x3333%s)", copy + 1, v,
              fimg_read_count(fat_lba(0, A)), copy == 0 ? " from 1" : "");
        CHECK(live_entry(copy, A) == 0x3333u + copy, "copy %d holds the new value (pending)", copy + 1);
    }
    // FAT 2 got 0x3334: write the mirror value, flush, and the next lookup still gives the new one.
    CHECK(fat16_set_entry(d, 1, A, 0x3333) == 0 && fat16_flush(d) == 0, "set copy 2 again, flush");
    CHECK(durable_entry(0, A) == 0x3333 && durable_entry(1, A) == 0x3333, "both durable copies hold 0x3333");
    CHECK(get(A) == 0x3333 && get(B) == 0x2222 && get(A) == 0x3333, "A, B, A after the flush");

    // A file read (warm cache), rewritten shorter and then longer, and a second file in between.
    fill_big(3);
    write_big();
    CHECK(read_file("0:/BIG.BIN", big_back, big_bytes) == big_bytes && memcmp(big_back, big_data, big_bytes) == 0,
          "BIG.BIN");
    static unsigned char small[3 * 4 * S + 100], other[7 * 4 * S];
    for (int i = 0; i < (int)sizeof(small); i++)
    {
        small[i] = (unsigned char)(i * 5 + 1);
    }
    for (int i = 0; i < (int)sizeof(other); i++)
    {
        other[i] = (unsigned char)(i * 3 + 2);
    }
    CHECK(write_file("0:/BIG.BIN", small, sizeof(small)) == (int)sizeof(small), "rewrite BIG.BIN short");
    CHECK(write_file("0:/OTHER.BIN", other, sizeof(other)) == (int)sizeof(other), "write OTHER.BIN");
    fill_big(4);
    CHECK(write_file("0:/BIG.BIN", big_data, big_bytes) == big_bytes, "rewrite BIG.BIN long");
    CHECK(read_file("0:/BIG.BIN", big_back, big_bytes) == big_bytes && memcmp(big_back, big_data, big_bytes) == 0,
          "BIG.BIN reads back after the rewrites");
    static unsigned char other_back[sizeof(other)];
    CHECK(read_file("0:/OTHER.BIN", other_back, sizeof(other)) == (int)sizeof(other) &&
          memcmp(other_back, other, sizeof(other)) == 0, "OTHER.BIN reads back");

    // Two write calls on one handle, then a read: the second call walks the chain the first one
    // linked (FAT entries written while their sector may be cached), and the read walks it all.
    static unsigned char two[5 * 4 * S + 77], two_back[sizeof(two)];
    for (int i = 0; i < (int)sizeof(two); i++)
    {
        two[i] = (unsigned char)(i * 7 + 3);
    }
    int first = 4 * S + 10;
    int fd = file_open("0:/TWO.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "open TWO.BIN");
    CHECK(file_write(fd, two, first) == first, "TWO.BIN first write");
    CHECK(file_write(fd, two + first, sizeof(two) - first) == (int)(sizeof(two) - first), "TWO.BIN second write");
    CHECK(fclose(fd) == 0, "close TWO.BIN");
    int got = read_file("0:/TWO.BIN", two_back, sizeof(two));
    CHECK(got == (int)sizeof(two) && memcmp(two_back, two, sizeof(two)) == 0,
          "TWO.BIN, written with two calls on one handle, reads back (%d)", got);

    long differ = 0;
    for (long s = 0; s < spf; s++)
    {
        differ += memcmp((void *)(fimg_durable() + (reserved + s) * S), (void *)(fimg_durable() + (reserved + spf + s) * S), S) != 0;
    }
    CHECK(differ == 0 && fimg_cut_pending(fimg_ledger_count()) == 0, "the two FATs agree on the media (%ld sectors differ)", differ);
}

static void c_uncertain(void)
{
    // A write to a free data sector at the end fails at the drive.
    mount_ab();
    CHECK(fat16_prepare_write(d) == 0, "prepare");
    CHECK(get(A) == 0x1111, "warm A");
    fimg_fail_write(1, -EIO);
    unsigned char byte = 0x77;
    CHECK(fat16_write_at(d, (int)((fimg_sectors() - 1) * S), &byte, 1) == -EIO, "failed write: -EIO");
    fimg_reset_counts();
    CHECK(get(A) == 0x1111 && fimg_read_count(fat_lba(0, A)) == 1, "A after the failed write: %d reads (want 1)",
          fimg_read_count(fat_lba(0, A)));

    mount_ab();
    CHECK(fat16_prepare_write(d) == 0, "prepare");
    CHECK(get(A) == 0x1111, "warm A");
    fimg_fail_flush(1, -EIO);
    CHECK(fat16_flush(d) == -EIO, "failed flush: -EIO");
    fimg_reset_counts();
    CHECK(get(A) == 0x1111 && fimg_read_count(fat_lba(0, A)) == 1, "A after the failed flush: %d reads (want 1)",
          fimg_read_count(fat_lba(0, A)));
}

static void c_prepare(void)
{
    // Preparation drops a warm cache: the entries are read from the disk again after it.
    mount_ab();
    CHECK(get(A) == 0x1111, "warm A");
    poke_both(A, 0x4444);
    CHECK(fat16_prepare_write(d) == 0, "prepare with equal copies");
    fimg_reset_counts();
    int v = get(A);
    CHECK(v == 0x4444 && fimg_counts.reads == 1, "A after the preparation: 0x%x with %d reads (want 0x4444, 1)", v,
          fimg_counts.reads);

    // FAT 2 differs from a warm FAT 1 sector.
    mount_ab();
    CHECK(get(A) == 0x1111, "warm A");
    poke(1, A, 0x5555);
    CHECK(fat16_prepare_write(d) == -EIO, "FAT 2 changed behind a warm cache: -EIO");

    // FAT 1 on the disk differs from what the warm cache holds (and from FAT 2).
    mount_ab();
    CHECK(get(A) == 0x1111, "warm A");
    poke(0, A, 0x5555);
    CHECK(fat16_prepare_write(d) == -EIO, "FAT 1 changed behind a warm cache: -EIO");
    // ... also through a file write.
    mount_ab();
    CHECK(get(A) == 0x1111, "warm A");
    poke(1, A, 0x6666);
    static unsigned char one[10];
    int res = write_file("0:/NEW.TXT", one, sizeof(one));
    CHECK(res < 0, "file write with the copies different: refused (%d)", res);
    CHECK(fimg_counts.writes == 0, "nothing written (%d writes)", fimg_counts.writes);
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "one read per sector", c_one_read },
    { "eviction", c_eviction },
    { "failed fill", c_failed_fill },
    { "both copies", c_both_copies },
    { "uncertain write", c_uncertain },
    { "preparation", c_prepare },
};

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: fat-cache <image>\n");
        return 2;
    }
    fimg_open(argv[1]);
    const unsigned char *b = fimg_durable();
    reserved = le16(b + 14);
    spc = b[13];
    spf = le16(b + 22);
    root_entries = le16(b + 17);
    root_lba = reserved + 2 * spf;
    long data_lba = root_lba + root_entries * 32 / S;
    long clusters = (fimg_sectors() - data_lba) / spc;
    // A and B: the same offset in two FAT sectors, past every cluster in use, free in both copies.
    A = (int)((clusters / 2) & ~255L) + 17;
    B = A + 256;
    if (le16(b + 11) != S || spc != 4 || b[16] != 2 || B + 2 >= clusters || durable_entry(0, A) || durable_entry(1, A) ||
        durable_entry(0, B) || durable_entry(1, B))
    {
        printf("fat-cache: unexpected image geometry (sector %u, cluster %ld sectors, %u FATs, %ld clusters)\n",
               le16(b + 11), spc, b[16], clusters);
        return 2;
    }
    big_data = malloc(big_bytes);
    big_back = malloc(big_bytes);
    int failed = 0, total = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
            cases[i].run();
            printf("  %s: %d checks, %d failed\n", cases[i].name, checks, bad);
            fflush(0);
            _exit(bad || kalloc_bad_frees() || kalloc_poison_errors() ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("  FAIL %s%s\n", cases[i].name, WIFEXITED(status) ? "" : " (killed)");
            failed++;
        }
    }
    printf("fat-cache host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
