/*
 * Host check for scripts/tests/fat-cursor.sh: the read cursor of kernel/fs/fat/fat16.c (each
 * open file remembers the cluster of its last read, so the next read goes on from there instead
 * of walking the chain from the first cluster). The real file.c, pparser.c, fat16.c and
 * streamer.c run on the image disk model (fat-image.c) with bin/os.bin as the disk.
 *
 * The test file FRAG.BIN (16 clusters, the last one partly used) is put on the image by hand:
 * its clusters alternate between two FAT sectors (A, B, A + 1, B + 1, ...), so every link
 * lookup misses the one-sector FAT cache and reads a sector of the first FAT. The number of
 * first-FAT sector reads is then the number of links walked (one less when a call's first lookup
 * is in the sector the cache still holds): that is how the chain work is measured, from outside
 * (the cursor's fields are never looked at). Every byte of the file depends on its position and
 * cluster, and every free cluster holds poison bytes.
 *
 *   sequential reads   the whole file with file_read and fread in 1-byte and 4096-byte steps,
 *                      and with fread of 1-byte items: at most one link per cluster boundary
 *                      (15 for the file), whatever the number of calls
 *   cluster boundaries reads that end exactly at a boundary, and reads that start at one or
 *                      cross one: the right bytes, at most one link for the next cluster
 *   seeks              forward seeks walk only from the last cluster read; backward seeks
 *                      (file_seek and the legacy fseek) read the right bytes; two open files
 *                      keep their own place
 *   failed seeks       each kind of refused seek (file_seek and fseek) leaves the position and
 *                      the cursor as they were: the next read gives the byte there with no link
 *   failed reads       the n-th disk read of a 3-cluster read fails, for every n (FAT and data
 *                      sectors, with and without a cursor): file_read keeps its position,
 *                      fread keeps the items read before the failure; a retry gives the right
 *                      bytes and walks only from the last cluster read
 *   truncate           another handle truncates the file (open for writing) and writes it again
 *                      somewhere else, while the old clusters hold poison: the reader sees end of
 *                      file, then the new bytes at its old position, never the old clusters
 *   looped chain       FRAG.BIN's chain looped back to its start and its size past the longest
 *                      chain the volume can have: a read in the cluster after that longest
 *                      chain gives -EIO with no link walked, also right after a read one
 *                      cluster before it (the cursor stands next to it)
 *
 * Usage: fat-cursor <image> <out dir>. Writes <out dir>/fixture.img (the image with FRAG.BIN)
 * and <out dir>/rewritten.img (after the truncate case) for fsck.fat, prints "clusters <n>",
 * one line per failed check and "fat-cursor host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "string/string.h"
#include "fat-image.h"

int sprintf(char *str, const char *format, ...);
unsigned int alarm(unsigned int seconds);

#define S FIMG_SECTOR
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

#define N 16                      // clusters of FRAG.BIN
#define CB (4 * S)                // cluster bytes (spc is checked to be 4 in main)
#define LEN (N * CB - 700)        // FRAG.BIN's size: the last cluster is partly used
#define POISON 0xE7
#define PATH "0:/frag.bin"

static int checks, bad;
static const char *out_dir;
static long reserved, spf, spc, root_lba, root_entries, data_lba, clusters;
static int chain[N];
static unsigned char buf[LEN + 64];

static unsigned int le16(const unsigned char *p)
{
    return p[0] | (p[1] << 8);
}

static void put16(unsigned char *p, unsigned int v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put32(unsigned char *p, unsigned int v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

// The byte at offset <i> of FRAG.BIN as written with <seed>: differs between clusters, sectors and bytes.
static unsigned char pattern(int seed, int i)
{
    return (unsigned char)(i * 7 + (i / CB) * 29 + (i / S) * 3 + seed);
}

// <n> bytes of buf are FRAG.BIN's bytes (seed 1) from offset <at>.
static int bytes_are(int seed, int at, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (buf[i] != pattern(seed, at + i))
        {
            printf("    byte %d: 0x%02x, want 0x%02x\n", at + i, buf[i], pattern(seed, at + i));
            return 0;
        }
    }
    return 1;
}

static long fat_lba(int copy, int cluster)
{
    return reserved + copy * spf + (cluster * 2L) / S;
}

static unsigned int fat_entry(int cluster)
{
    return le16(fimg_durable() + fat_lba(0, cluster) * S + (cluster * 2) % S);
}

static unsigned char *cluster_bytes(int cluster)
{
    return fimg_durable() + (data_lba + (cluster - 2) * spc) * S;
}

// Sector reads of the first / second FAT since the last fimg_reset_counts(): with FRAG.BIN's
// chain, one per link walked.
static int fat_reads(int copy)
{
    int n = 0;
    for (long s = 0; s < spf; s++)
    {
        n += fimg_read_count(reserved + copy * spf + s);
    }
    return n;
}

static int links(void)
{
    return fat_reads(0) + fat_reads(1);
}

// FRAG.BIN on the durable image (both FATs, a free root slot, the data), poison in every free
// cluster. 0 or -1.
static int make_fixture(void)
{
    unsigned char *b = fimg_durable();
    long a = (clusters / 2) & ~255L;
    for (int i = 0; i < N; i++)
    {
        chain[i] = (int)(a + 17 + i / 2 + (i % 2) * 256);
        if (fat_entry(chain[i]) != 0)
        {
            return -1;
        }
    }
    for (long c = 2; c < clusters + 2; c++)
    {
        if (fat_entry((int)c) == 0)
        {
            memset(cluster_bytes((int)c), POISON, CB);
        }
    }
    for (int i = 0; i < N; i++)
    {
        for (int copy = 0; copy < 2; copy++)
        {
            put16(b + fat_lba(copy, chain[i]) * S + (chain[i] * 2) % S, i + 1 < N ? chain[i + 1] : 0xFFFF);
        }
        for (int j = 0; j < CB; j++)
        {
            int at = i * CB + j;
            cluster_bytes(chain[i])[j] = at < LEN ? pattern(1, at) : 0;
        }
    }
    for (long i = 0; i < root_entries; i++)
    {
        unsigned char *e = b + root_lba * S + i * 32;
        if (e[0] == 0 || e[0] == 0xE5)
        {
            memset(e, 0, 32);
            memcpy(e, "FRAG    BIN", 11);
            e[11] = 0x20;
            put16(e + 26, chain[0]);
            put32(e + 28, LEN);
            return 0;
        }
    }
    return -1;
}

static void export_live(const char *name)
{
    char path[512];
    sprintf(path, "%s/%s", out_dir, name);
    CHECK(fimg_export(fimg_view_live(), path) == 0, "export %s", path);
}

static int open_read(void)
{
    CHECK(fimg_mount() == 0, "mount");
    int fd = file_open(PATH, FILE_MODE_READ);
    CHECK(fd > 0, "open %s: %d", PATH, fd);
    return fd;
}

static int pos_of(int fd)
{
    return file_seek(fd, 0, SEEK_CUR);
}

// One read of <n> bytes at the current position <at> (file_read, or fread of one item), its
// bytes checked, at most <max> links walked.
static void read_at(int fd, int legacy, int at, int n, int max, const char *what)
{
    fimg_reset_counts();
    int res = legacy ? fread(buf, n, 1, fd) : file_read(fd, buf, n);
    int got = links();
    CHECK(res == (legacy ? 1 : n) && bytes_are(1, at, n), "%s: %s of %d bytes at %d gave %d", what,
          legacy ? "fread" : "file_read", n, at, res);
    CHECK(got <= max, "%s: %d bytes at %d walked %d links (want at most %d)", what, n, at, got, max);
}

// ---------------------------------------------------------------------------------------------

static void c_sequential(void)
{
    static const struct { int legacy, step, items; const char *name; } modes[] = {
        { 0, 1, 0, "file_read, 1 byte" },
        { 0, 4096, 0, "file_read, 4096 bytes" },
        { 1, 1, 0, "fread, one 1-byte item" },
        { 1, 4096, 0, "fread, one 4096-byte item" },
        { 1, 4096, 1, "fread, 4096 1-byte items" },
    };
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    {
        int fd = open_read();
        fimg_reset_counts();
        int ok = 1, calls = 0;
        for (int at = 0; at < LEN && ok; at += modes[m].step, calls++)
        {
            int n = LEN - at < modes[m].step ? LEN - at : modes[m].step;
            int res = !modes[m].legacy ? file_read(fd, buf, n)
                      : modes[m].items ? fread(buf, 1, n, fd) : fread(buf, n, 1, fd);
            ok = res == (modes[m].legacy && !modes[m].items ? 1 : n) && bytes_are(1, at, n);
        }
        int walked = links();
        CHECK(ok, "%s: the whole file reads back", modes[m].name);
        CHECK(walked <= N - 1 && fat_reads(1) == 0, "%s: %d calls walked %d links, %d of them in the second FAT (want at most %d and 0)",
              modes[m].name, calls, walked, fat_reads(1), N - 1);
        CHECK(file_read(fd, buf, 1) == 0 && fread(buf, 1, 1, fd) == 0 && pos_of(fd) == LEN, "%s: end of file", modes[m].name);
        printf("    %s: %d calls, %d links\n", modes[m].name, calls, walked);
        CHECK(fclose(fd) == 0, "close");
    }
}

static void c_boundaries(void)
{
    for (int legacy = 0; legacy < 2; legacy++)
    {
        int fd = open_read();
        read_at(fd, legacy, 0, CB, 0, "cluster 0, exactly");
        read_at(fd, legacy, CB, 1, 1, "first byte of cluster 1");
        read_at(fd, legacy, CB + 1, CB - 1, 0, "rest of cluster 1, ends at the boundary");
        read_at(fd, legacy, 2 * CB, CB, 1, "cluster 2, exactly");
        read_at(fd, legacy, 3 * CB, CB - 2, 1, "cluster 3 but its last 2 bytes");
        read_at(fd, legacy, 4 * CB - 2, 4, 1, "4 bytes across the boundary of clusters 3 and 4");
        read_at(fd, legacy, 4 * CB + 2, 3 * CB - 2, 2, "to the end of cluster 6");
        read_at(fd, legacy, 7 * CB, 1, 1, "first byte of cluster 7");
        CHECK(fclose(fd) == 0, "close");
    }
}

static void c_seeks(void)
{
    int fd = open_read();
    read_at(fd, 0, 0, 1, 0, "byte 0");
    CHECK(file_seek(fd, 2 * CB + 10, SEEK_SET) == 2 * CB + 10, "file_seek forward");
    read_at(fd, 0, 2 * CB + 10, 1, 2, "after file_seek forward 2 clusters");
    CHECK(fseek(fd, 2 * CB + 100, SEEK_SET) == 0, "fseek forward in the cluster");
    read_at(fd, 1, 2 * CB + 100, 1, 0, "after fseek forward in the cluster");
    CHECK(fseek(fd, 3 * CB, SEEK_CUR) == 0, "fseek SEEK_CUR forward 3 clusters");
    read_at(fd, 0, 5 * CB + 101, 1, 3, "after fseek SEEK_CUR forward 3 clusters");
    CHECK(file_seek(fd, 10 * CB - 1, SEEK_CUR) == 15 * CB + 101, "file_seek SEEK_CUR forward 10 clusters");
    read_at(fd, 1, 15 * CB + 101, 1, 10, "after file_seek SEEK_CUR forward 10 clusters");

    CHECK(file_seek(fd, CB + 5, SEEK_SET) == CB + 5, "file_seek back to cluster 1");
    read_at(fd, 0, CB + 5, 1, 1, "after file_seek back to cluster 1");
    read_at(fd, 0, CB + 6, CB, 1, "on from there");
    CHECK(fseek(fd, 0, SEEK_SET) == 0, "fseek back to 0");
    read_at(fd, 1, 0, 1, 0, "after fseek back to 0");
    CHECK(file_seek(fd, 9 * CB, SEEK_SET) == 9 * CB, "file_seek to cluster 9");
    read_at(fd, 0, 9 * CB, 1, 9, "cluster 9");
    CHECK(fseek(fd, 6 * CB + 7, SEEK_SET) == 0, "fseek back to cluster 6");
    read_at(fd, 1, 6 * CB + 7, 1, 6, "after fseek back to cluster 6");
    CHECK(file_seek(fd, -(3 * CB), SEEK_CUR) == 3 * CB + 8, "file_seek SEEK_CUR back 3 clusters");
    read_at(fd, 0, 3 * CB + 8, 1, 3, "after file_seek SEEK_CUR back 3 clusters");
    CHECK(file_seek(fd, -1, SEEK_END) == LEN - 1, "file_seek to the last byte");
    read_at(fd, 0, LEN - 1, 1, N - 1 - 3, "last byte");

    // Two open files, each at its own place: byte by byte through the file, in turns.
    int fd2 = file_open(PATH, FILE_MODE_READ);
    CHECK(fd2 > 0, "second open");
    CHECK(file_seek(fd, 0, SEEK_SET) == 0 && file_seek(fd2, 5 * CB + 3, SEEK_SET) == 5 * CB + 3, "seek both");
    fimg_reset_counts();
    int ok = 1;
    for (int i = 0; i + 5 * CB + 3 < LEN && ok; i++)
    {
        ok = file_read(fd, buf, 1) == 1 && bytes_are(1, i, 1);
        ok = ok && file_read(fd2, buf, 1) == 1 && bytes_are(1, 5 * CB + 3 + i, 1);
    }
    int walked = links();
    CHECK(ok, "two files read in turns give their own bytes");
    // fd walks clusters 0..10, fd2 walks 0..15 (from the first cluster once, then on).
    CHECK(walked <= 10 + 15, "two files read in turns: %d links (want at most 25)", walked);
    CHECK(fclose(fd2) == 0 && fclose(fd) == 0, "close");
}

static void c_failed_seeks(void)
{
    int fd = open_read();
    CHECK(file_seek(fd, 5 * CB + 9, SEEK_SET) == 5 * CB + 9, "seek to cluster 5");
    read_at(fd, 0, 5 * CB + 9, 1, 5, "a byte of cluster 5");
    int at = 5 * CB + 10;
    struct { int legacy, offset, whence, want; const char *what; } bad_seeks[] = {
        { 0, -1, SEEK_SET, -EINVARG, "file_seek to -1" },
        { 0, LEN + 1, SEEK_SET, -EINVARG, "file_seek past the end" },
        { 0, -(LEN + 1), SEEK_CUR, -EINVARG, "file_seek SEEK_CUR before the start" },
        { 0, LEN + 1, SEEK_CUR, -EINVARG, "file_seek SEEK_CUR past the end" },
        { 0, 1, SEEK_END, -EINVARG, "file_seek SEEK_END + 1" },
        { 0, -(LEN + 1), SEEK_END, -EINVARG, "file_seek SEEK_END before the start" },
        { 0, 0, 7, -EINVARG, "file_seek with whence 7" },
        { 1, LEN + 1, SEEK_SET, -EIO, "fseek past the end" },
        { 1, -1, SEEK_SET, -EIO, "fseek to -1 (0xFFFFFFFF)" },
        { 1, LEN + 1, SEEK_CUR, -EIO, "fseek SEEK_CUR past the end" },
        { 1, 0, SEEK_END, -EUNIMP, "fseek SEEK_END" },
        { 1, 0, 7, -EINVARG, "fseek with whence 7" },
    };
    for (unsigned i = 0; i < sizeof(bad_seeks) / sizeof(bad_seeks[0]); i++)
    {
        int res = bad_seeks[i].legacy ? fseek(fd, bad_seeks[i].offset, bad_seeks[i].whence)
                                       : file_seek(fd, bad_seeks[i].offset, bad_seeks[i].whence);
        CHECK(res == bad_seeks[i].want, "%s: %d (want %d)", bad_seeks[i].what, res, bad_seeks[i].want);
        CHECK(pos_of(fd) == at, "%s: position %d (want %d)", bad_seeks[i].what, pos_of(fd), at);
        read_at(fd, i % 2, at, 1, 0, bad_seeks[i].what);
        at++;
    }
    CHECK(fclose(fd) == 0, "close");
}

// The read <legacy ? fread(buf, CB, 3) : file_read(buf, 3 * CB)> from <start> with its <n>-th disk read
// failing (warm: after a 1-byte read just before <start>). 1 if the read failed, 0 if it succeeded.
static int failed_read(int legacy, int warm, int start, int n)
{
    int fd = open_read();
    int at = warm ? start - 1 : start;
    CHECK(file_seek(fd, at, SEEK_SET) == at, "seek");
    if (warm)
    {
        read_at(fd, 0, at, 1, at / CB, "warm the cursor");
    }
    memset(buf, 0, 3 * CB);
    fimg_reset_counts();
    fimg_fail_read(n, -EIO);
    int res = legacy ? fread(buf, CB, 3, fd) : file_read(fd, buf, 3 * CB);
    int failed = fimg_counts.failed > 0;
    fimg_fail_read(0, 0);
    int pos = pos_of(fd);
    if (!failed)
    {
        CHECK(res == (legacy ? 3 : 3 * CB) && bytes_are(1, start, 3 * CB), "read %d: no failure, right bytes", n);
        CHECK(fclose(fd) == 0, "close");
        return 0;
    }
    int done = (pos - start) / CB;
    CHECK(res == -EIO, "%s %s, disk read %d fails: %d (want -EIO)", legacy ? "fread" : "file_read", warm ? "warm" : "cold", n, res);
    if (legacy)
    {
        CHECK(pos - start == done * CB && done >= 0 && done < 3 && bytes_are(1, start, done * CB),
              "fread, disk read %d fails: position %d, the items before it read (start %d)", n, pos, start);
    }
    else
    {
        CHECK(pos == start, "file_read, disk read %d fails: position %d (want %d)", n, pos, start);
    }
    // The retry walks only from the last cluster read: the read's last cluster is at most 3
    // clusters on from the cluster before <start> (cold: from the first cluster).
    int last = (start + 3 * CB - 1) / CB;
    int from = warm ? (start - 1) / CB : 0;
    read_at(fd, legacy, pos, start + 3 * CB - pos, last - from, "retry");
    CHECK(fclose(fd) == 0, "close");
    return 1;
}

static void c_failed_reads(void)
{
    for (int legacy = 0; legacy < 2; legacy++)
    {
        for (int warm = 0; warm < 2; warm++)
        {
            int start = 4 * CB + 100, failures = 0;
            for (int n = 1; n < 200 && failed_read(legacy, warm, start, n); n++)
            {
                failures++;
            }
            // 4 clusters of data, 4 sectors each, and 3 links (+ 4 for a cold one).
            // At least one read per cluster of data (4) and per link (3).
            CHECK(failures >= 4 + 3, "%s %s: %d disk reads failed in turn", legacy ? "fread" : "file_read", warm ? "warm" : "cold", failures);
            printf("    %s %s: each of %d disk reads failed in turn\n", legacy ? "fread" : "file_read", warm ? "warm" : "cold", failures);
        }
    }
}

static void c_truncate(void)
{
    int fd = open_read();
    CHECK(file_seek(fd, 4 * CB + 3, SEEK_SET) == 4 * CB + 3, "seek to cluster 4");
    read_at(fd, 0, 4 * CB + 3, 1, 4, "a byte of cluster 4");
    int fd2 = file_open(PATH, FILE_MODE_READ);
    CHECK(fd2 > 0 && file_seek(fd2, 9 * CB, SEEK_SET) == 9 * CB, "second reader at cluster 9");
    read_at(fd2, 1, 9 * CB, 2, 9, "two bytes of cluster 9");

    int writer = file_open(PATH, FILE_MODE_WRITE);
    CHECK(writer > 0, "open for writing (truncates): %d", writer);
    // Both now stand past the end (file_seek can't tell where: it refuses that position too).
    CHECK(file_read(fd, buf, 1) == 0, "reader after the truncate: end of file");
    CHECK(fread(buf, 1, 1, fd2) == -EIO, "second reader after the truncate: fread past the end is -EIO");
    // The old clusters (free now) hold poison, as if a later file had taken them.
    for (int i = 0; i < N; i++)
    {
        memset(cluster_bytes(chain[i]), POISON, CB);
    }
    static unsigned char data[LEN];
    for (int i = 0; i < LEN; i++)
    {
        data[i] = pattern(2, i);
    }
    CHECK(file_write(writer, data, LEN) == LEN && fclose(writer) == 0, "write the file again");
    const unsigned char *root = fimg_durable() + root_lba * S;
    int first = -1;
    for (long i = 0; i < root_entries; i++)
    {
        if (memcmp((void *)(root + i * 32), "FRAG    BIN", 11) == 0)
        {
            first = le16(root + i * 32 + 26);
        }
    }
    int moved = 1;
    for (int i = 0; i < N; i++)
    {
        moved &= first != chain[i];
    }
    CHECK(moved, "the new file starts at cluster %d, not in the old chain", first);

    int ok = file_read(fd, buf, 2 * CB) == 2 * CB;
    for (int i = 0; ok && i < 2 * CB; i++)
    {
        ok = buf[i] == pattern(2, 4 * CB + 4 + i);
    }
    CHECK(ok, "reader after the rewrite: the new bytes at its position (byte 0x%02x)", buf[0]);
    ok = fread(buf, CB, 1, fd2) == 1;
    for (int i = 0; ok && i < CB; i++)
    {
        ok = buf[i] == pattern(2, 9 * CB + 2 + i);
    }
    CHECK(ok, "second reader after the rewrite: the new bytes at its position (byte 0x%02x)", buf[0]);
    CHECK(fclose(fd2) == 0 && fclose(fd) == 0, "close");
    CHECK(fimg_cut_pending(fimg_ledger_count()) == 0, "nothing left pending");
    export_live("rewritten.img");
}

// FRAG.BIN's last link goes back to its first cluster and its size says one cluster more than
// the volume has: no chain can be that long, so a read in cluster index clusters (the volume's
// cluster count) or later is -EIO, also when the cursor stands one cluster before it.
static void c_loop(void)
{
    unsigned char *b = fimg_durable();
    for (int copy = 0; copy < 2; copy++)
    {
        put16(b + fat_lba(copy, chain[N - 1]) * S + (chain[N - 1] * 2) % S, chain[0]);
    }
    for (long i = 0; i < root_entries; i++)
    {
        if (memcmp((void *)(b + root_lba * S + i * 32), "FRAG    BIN", 11) == 0)
        {
            put32(b + root_lba * S + i * 32 + 28, (clusters + 1) * CB);
        }
    }
    int fd = open_read();
    read_at(fd, 0, 0, 1, 0, "byte 0");
    int last = clusters * CB;  // cluster index clusters: one past the longest chain
    int at = last - CB + 5;
    CHECK(file_seek(fd, at, SEEK_SET) == at, "file_seek forward to the last cluster a chain can have");
    int index = (clusters - 1) % N;  // the loop's cluster there
    fimg_reset_counts();
    CHECK(file_read(fd, buf, 1) == 1 && buf[0] == pattern(1, index * CB + 5), "a byte there reads (round the loop)");
    CHECK(file_seek(fd, last, SEEK_SET) == last, "file_seek forward to the next cluster");
    fimg_reset_counts();
    int res = file_read(fd, buf, 1);
    CHECK(res == -EIO && links() == 0, "a byte in cluster index %ld: %d after %d links (want -EIO, 0)", clusters, res, links());
    CHECK(pos_of(fd) == last, "position still %d", last);
    CHECK(file_seek(fd, last + 7, SEEK_SET) == last + 7 && fread(buf, 1, 1, fd) == -EIO, "fread there: -EIO");
    CHECK(file_seek(fd, CB, SEEK_SET) == CB && file_read(fd, buf, 1) == 1 && buf[0] == pattern(1, CB),
          "back in cluster 1: reads");
    CHECK(fclose(fd) == 0, "close");
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "sequential reads", c_sequential },
    { "cluster boundaries", c_boundaries },
    { "seeks", c_seeks },
    { "failed seeks", c_failed_seeks },
    { "failed reads", c_failed_reads },
    { "truncate", c_truncate },
    { "looped chain", c_loop },
};

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: fat-cursor <image> <out dir>\n");
        return 2;
    }
    out_dir = argv[2];
    fimg_open(argv[1]);
    const unsigned char *b = fimg_durable();
    reserved = le16(b + 14);
    spc = b[13];
    spf = le16(b + 22);
    root_entries = le16(b + 17);
    root_lba = reserved + 2 * spf;
    data_lba = root_lba + root_entries * 32 / S;
    long total = le16(b + 19) ? le16(b + 19) : (long)(b[32] | (b[33] << 8) | (b[34] << 16) | ((long)b[35] << 24));
    clusters = (total - data_lba) / spc;
    if (le16(b + 11) != S || spc != 4 || b[16] != 2 || clusters / 2 + 17 + N / 2 + 256 >= clusters + 2 ||
        (long)total * S > fimg_file_bytes() || make_fixture() < 0)
    {
        printf("fat-cursor: unexpected image (sector %u, cluster %ld sectors, %u FATs, %ld clusters)\n",
               le16(b + 11), spc, b[16], clusters);
        return 2;
    }
    printf("clusters %ld\n", clusters);
    printf("chain");
    for (int i = 0; i < N; i++)
    {
        printf(" %d", chain[i]);
    }
    printf("\n");
    export_live("fixture.img");
    int failed = 0, count = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < count; i++)
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
    printf("fat-cursor host check: %d of %d cases failed\n", failed, count);
    fflush(0);
    return failed ? 1 : 0;
}
