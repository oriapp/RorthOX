/*
 * Host check for scripts/tests/fat-write-cursor.sh: how kernel/fs/fat/fat16.c finds the clusters
 * a file write goes to. It runs the real file.c, pparser.c, fat16.c and streamer.c on the image
 * disk model (fat-image.c) and watches them from outside only: sector reads and writes, the
 * ledger of writes and flushes, heap calls, and what the files read back.
 *
 * The holes: before the mount, a file FILL.BIN takes every free cluster up to the last hole, and
 * leaves free exactly one cluster per FAT sector (cluster 256 * k + 100). A file written on that
 * image gets the holes, so each link of its chain is in another FAT sector, and with the one-sector
 * FAT cache each link the code follows is one read of the first FAT. Allocation scans and FAT entry
 * writes (read first, then written) read both copies alike, so "link reads" = reads of the first
 * FAT minus reads of the second one.
 *
 *   overwrite      a file on 8 holes, then 64-byte writes from its start to its end in the same
 *                  handle: at most one link read per cluster, no second-FAT read, no heap call,
 *                  and the right bytes; a write after a seek back reads at most the links up to
 *                  its cluster
 *   growth         a new file grown by 64-byte writes up to 8 clusters, then by appends through
 *                  a second handle: no link read, at most one heap call of at most 16 bytes per
 *                  new cluster, the right bytes
 *   peer truncate  a writer with a cluster in hand, its file truncated and rewritten through
 *                  another handle (on other clusters): its next writes go to the file's new
 *                  clusters, and the file in between keeps its bytes
 *   two appenders  two append handles on one file take turns, growing it: each appends at the
 *                  current end of the file, the chain covers the size, no cluster is lost
 *   same size      a peer's overwrite that keeps the size doesn't make an appender walk its chain
 *   no space       a write that needs one cluster more than is free gives -ENOSPC, writes and
 *                  flushes nothing, leaves every sector and the heap as it was and the
 *                  position where it was; a write that fits works after it
 *   failed write   a FAT read failing before any data is written gives the error and leaves the
 *                  position, the next write goes to the right place; when the new cluster's
 *                  FAT entry, or the directory entry after the link, can't be written, a reader
 *                  standing in the last cluster reads the old bytes, then the end of the file
 *   order          the sectors a write changes, in order: data, flush, the new clusters' entries
 *                  in each FAT with a flush after each, the old last cluster's entry in each FAT
 *                  with a flush after each, the directory entry, flush
 *
 * Usage: fat-write-cursor <image> <dir>. The cases that end with a valid volume export it to
 * <dir>/<case>.img (for fsck.fat). Prints one line per failed check and
 * "fat-write-cursor host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "fat-image.h"
#include "string/string.h"
#include "memory/memory.h"
#include "disk/disk.h"

void *malloc(unsigned long size);
unsigned int alarm(unsigned int seconds);

#define S FIMG_SECTOR
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

#define HOLES 8
#define MAX_BYTES (16 * 4096)

static int checks, bad;
static long reserved, spf, spc, root_lba, root_entries, data_lba, clusters;
static int cl;  // bytes per cluster
static int holes[HOLES];
static const char *outdir;
static unsigned char buf[MAX_BYTES], back[MAX_BYTES];

static unsigned int le16(const unsigned char *p)
{
    return p[0] | (p[1] << 8);
}

static void put16(unsigned char *p, unsigned int v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static long fat_lba(int copy, int cluster)
{
    return reserved + copy * spf + (cluster * 2L) / S;
}

static unsigned int durable_entry(int copy, int cluster)
{
    return le16(fimg_durable() + fat_lba(copy, cluster) * S + (cluster * 2) % S);
}

static void poke_both(int cluster, unsigned int value)
{
    for (int copy = 0; copy < 2; copy++)
    {
        put16(fimg_durable() + fat_lba(copy, cluster) * S + (cluster * 2) % S, value);
    }
}

static int fat_reads(int copy)
{
    int n = 0;
    for (long s = 0; s < spf; s++)
    {
        n += fimg_read_count(reserved + copy * spf + s);
    }
    return n;
}

// Links followed since the last fimg_reset_counts() (see the top of the file).
static int link_reads(void)
{
    return fat_reads(0) - fat_reads(1);
}

static unsigned char pat(int seed, long i)
{
    return (unsigned char)(i * 7 + (i >> 9) * 13 + seed * 31 + 1);
}

static void fill(unsigned char *p, int seed, long from, int n)
{
    for (int i = 0; i < n; i++)
    {
        p[i] = pat(seed, from + i);
    }
}

// Bytes the heap handed out between two marks (the host heap never reuses memory).
static unsigned char *heap_mark(void)
{
    unsigned char *p = kzalloc(1);
    kfree(p);
    return p;
}

static long heap_bytes(unsigned char *from)
{
    return heap_mark() - from - 16;
}

// The root entry of <name83> on the live disk, NULL if there is none.
static const unsigned char *live_root_entry(const char *name83)
{
    const unsigned char **v = fimg_view_live();
    for (long i = 0; i < root_entries; i++)
    {
        const unsigned char *e = v[root_lba + i * 32 / S] + (i * 32) % S;
        if (memcmp((void *)e, (void *)name83, 11) == 0)
        {
            return e;
        }
    }
    return 0;
}

// The chain of <name83> on the live disk in both FATs: its length if both copies have the same
// chain ending in an end mark, else -1.
static int live_chain(const char *name83, int *out, int max)
{
    const unsigned char *e = live_root_entry(name83);
    if (!e)
    {
        return -1;
    }
    const unsigned char **v = fimg_view_live();
    int n = 0;
    unsigned int c = le16(e + 26);
    while (c)
    {
        if (c < 2 || c >= clusters + 2 || n >= max)
        {
            return -1;
        }
        out[n++] = c;
        unsigned int a = le16(v[fat_lba(0, c)] + (c * 2) % S);
        unsigned int b = le16(v[fat_lba(1, c)] + (c * 2) % S);
        if (a != b)
        {
            return -1;
        }
        if (a >= 0xFFF8)
        {
            break;
        }
        c = a;
    }
    return n;
}

// The size in <name83>'s live root entry.
static long live_size(const char *name83)
{
    const unsigned char *e = live_root_entry(name83);
    return e ? (long)(e[28] | e[29] << 8 | e[30] << 16 | (unsigned long)e[31] << 24) : -1;
}

// Reads <path> whole through a new handle into back; returns the bytes read.
static int read_all(const char *path)
{
    int fd = file_open(path, FILE_MODE_READ);
    if (fd <= 0)
    {
        return fd;
    }
    int res = file_read(fd, back, MAX_BYTES);
    fclose(fd);
    return res;
}

// <path> reads back as want[0..bytes), and its chain in both FATs is exactly long enough.
static void check_file(const char *path, const char *name83, const unsigned char *want, int bytes)
{
    int res = read_all(path);
    CHECK(res == bytes && memcmp(back, (void *)want, bytes) == 0, "%s reads back %d bytes (want %d, %s)", path, res, bytes,
          res == bytes ? "different bytes" : "different size");
    static int chain[4096];
    int n = live_chain(name83, chain, 4096);
    CHECK(n == (bytes + cl - 1) / cl && live_size(name83) == bytes, "%s: chain of %d clusters, size %ld (want %d, %d)",
          path, n, live_size(name83), (bytes + cl - 1) / cl, bytes);
}

static void export_image(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.img", outdir, name);
    CHECK(fimg_cut_pending(fimg_ledger_count()) == 0, "%s: nothing pending at the end", name);
    CHECK(fimg_export(fimg_view_live(), path) == 0, "export %s", path);
}

// FILL.BIN takes every free cluster from 2 up to the last hole except the holes; then mount.
static void mount_holes(void)
{
    unsigned char *root = fimg_durable() + root_lba * S;
    long slot = -1;
    for (long i = 0; i < root_entries && slot < 0; i++)
    {
        if (root[i * 32] == 0 || root[i * 32] == 0xE5)
        {
            slot = i;
        }
    }
    int first = 0, prev = 0, count = 0;
    for (int c = 2; c <= holes[HOLES - 1]; c++)
    {
        int hole = 0;
        for (int k = 0; k < HOLES; k++)
        {
            hole |= c == holes[k];
        }
        if (hole || durable_entry(0, c) || durable_entry(1, c))
        {
            continue;
        }
        if (prev)
        {
            poke_both(prev, c);
        }
        else
        {
            first = c;
        }
        prev = c;
        count++;
    }
    poke_both(prev, 0xFFFF);
    unsigned char *e = root + slot * 32;
    memset(e, 0, 32);
    memcpy(e, "FILL    BIN", 11);
    e[11] = 0x20;
    put16(e + 24, 0x0021);
    put16(e + 26, first);
    long size = (long)count * cl;
    for (int i = 0; i < 4; i++)
    {
        e[28 + i] = size >> (8 * i);
    }
    CHECK(slot >= 0 && fimg_mount() == 0, "FILL.BIN in root slot %ld, mount", slot);
}

// ---------------------------------------------------------------------------------------------

static void c_overwrite(void)
{
    mount_holes();
    int bytes = HOLES * cl;
    int fd = file_open("0:/FRAG.BIN", FILE_MODE_WRITE);
    fill(buf, 1, 0, bytes);
    CHECK(fd > 0 && file_write(fd, buf, bytes) == bytes, "FRAG.BIN: %d bytes", bytes);
    static int chain[HOLES + 1];
    int n = live_chain("FRAG    BIN", chain, HOLES + 1);
    int on_holes = n == HOLES;
    for (int i = 0; i < n && i < HOLES; i++)
    {
        on_holes &= chain[i] == holes[i];
    }
    CHECK(on_holes, "FRAG.BIN is on the holes (%d clusters)", n);

    CHECK(file_seek(fd, 0, SEEK_SET) == 0, "seek to 0");
    fimg_reset_counts();
    int calls = kalloc_calls();
    unsigned char *mark = heap_mark();
    calls++;  // the mark's own call
    int wrong = 0;
    for (int at = 0; at < bytes; at += 64)
    {
        fill(buf + at, 2, at, 64);
        wrong += file_write(fd, buf + at, 64) != 64;
    }
    int links = link_reads(), second = fat_reads(1), heap = kalloc_calls() - calls;
    long heap_used = heap_bytes(mark);
    printf("    %d overwrites of 64 bytes over %d clusters: %d link reads, %d second-FAT reads, %d heap calls\n",
           bytes / 64, HOLES, links, second, heap);
    CHECK(wrong == 0, "%d of %d writes failed", wrong, bytes / 64);
    CHECK(links <= HOLES && second == 0, "%d link reads and %d second-FAT reads (want at most %d and 0)", links, second,
          HOLES);
    CHECK(heap == 0 && heap_used == 0, "%d heap calls, %ld bytes (want none)", heap, heap_used);

    // Back to the middle of cluster 5 (index 4): at most the 4 links up to it, then one more
    // for the next cluster.
    int at = 4 * cl + cl / 2;
    CHECK(file_seek(fd, at, SEEK_SET) == at, "seek to %d", at);
    fimg_reset_counts();
    fill(buf + at, 3, at, cl);
    CHECK(file_write(fd, buf + at, cl) == cl, "a cluster's worth from the middle of cluster 5");
    CHECK(link_reads() <= 5, "%d link reads (want at most 5)", link_reads());
    CHECK(file_seek(fd, 0, SEEK_CUR) == at + cl, "position %d after it", at + cl);
    CHECK(fclose(fd) == 0, "close");
    check_file("0:/FRAG.BIN", "FRAG    BIN", buf, bytes);
    export_image("overwrite");
}

static void c_growth(void)
{
    mount_holes();
    int bytes = HOLES * cl;
    int fd = file_open("0:/GROW.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "open GROW.BIN");
    fimg_reset_counts();
    int calls = kalloc_calls();
    unsigned char *mark = heap_mark();
    calls++;
    int wrong = 0;
    for (int at = 0; at < bytes; at += 64)
    {
        fill(buf + at, 4, at, 64);
        wrong += file_write(fd, buf + at, 64) != 64;
    }
    int links = link_reads(), heap = kalloc_calls() - calls;
    long heap_used = heap_bytes(mark);
    printf("    %d writes of 64 bytes growing a file to %d clusters: %d link reads, %d heap calls, %ld heap bytes\n",
           bytes / 64, HOLES, links, heap, heap_used);
    CHECK(wrong == 0, "%d of %d writes failed", wrong, bytes / 64);
    CHECK(links == 0, "%d link reads (want 0)", links);
    CHECK(heap <= HOLES && heap_used <= HOLES * 16, "%d heap calls, %ld bytes (want at most %d and %d)", heap, heap_used,
          HOLES, HOLES * 16);
    CHECK(fclose(fd) == 0, "close");

    // Appends through a new handle (it checks the chain when it opens): two more clusters.
    int ap = file_open("0:/GROW.BIN", FILE_MODE_APPEND);
    CHECK(ap > 0, "open GROW.BIN to append");
    fimg_reset_counts();
    wrong = 0;
    for (int at = bytes; at < bytes + 2 * cl; at += 64)
    {
        fill(buf + at, 4, at, 64);
        wrong += file_write(ap, buf + at, 64) != 64;
    }
    CHECK(wrong == 0 && link_reads() == 0, "%d failed appends, %d link reads (want 0, 0)", wrong, link_reads());
    CHECK(fclose(ap) == 0, "close");
    check_file("0:/GROW.BIN", "GROW    BIN", buf, bytes + 2 * cl);
    export_image("growth");
}

static void c_peer_truncate(void)
{
    CHECK(fimg_mount() == 0, "mount");
    static unsigned char x[3 * 4096], y[4096];
    int a = file_open("0:/X.BIN", FILE_MODE_WRITE);
    fill(x, 5, 0, 3 * cl);
    CHECK(a > 0 && file_write(a, x, 3 * cl) == 3 * cl, "A writes X.BIN, 3 clusters");
    // A's last write was in cluster 2 (index 1): it has that cluster in hand.
    CHECK(file_seek(a, cl + 100, SEEK_SET) == cl + 100 && file_write(a, x + cl + 100, 64) == 64, "A rewrites in cluster 2");

    // B truncates X.BIN; Y.BIN takes its old first cluster; B writes 3 clusters again.
    int b = file_open("0:/X.BIN", FILE_MODE_WRITE);
    CHECK(b > 0, "B truncates X.BIN");
    int c = file_open("0:/Y.BIN", FILE_MODE_WRITE);
    fill(y, 6, 0, cl);
    CHECK(c > 0 && file_write(c, y, cl) == cl && fclose(c) == 0, "Y.BIN, one cluster");
    fill(x, 7, 0, 3 * cl);
    CHECK(file_write(b, x, 3 * cl) == 3 * cl, "B writes X.BIN again, 3 clusters");

    // A writes in its cluster 2 and in the last cluster.
    int at = cl + 164;
    CHECK(file_seek(a, 0, SEEK_CUR) == at, "A's position is %d", at);
    fill(x + at, 8, at, 64);
    CHECK(file_write(a, x + at, 64) == 64, "A writes at %d", at);
    at = 2 * cl + 300;
    CHECK(file_seek(a, at, SEEK_SET) == at, "A seeks to %d", at);
    fill(x + at, 9, at, 64);
    CHECK(file_write(a, x + at, 64) == 64, "A writes at %d", at);
    CHECK(fclose(a) == 0 && fclose(b) == 0, "close");
    check_file("0:/X.BIN", "X       BIN", x, 3 * cl);
    check_file("0:/Y.BIN", "Y       BIN", y, cl);
    export_image("peer-truncate");
}

static void c_two_appenders(void)
{
    CHECK(fimg_mount() == 0, "mount");
    static unsigned char want[8 * 4096];
    int len = cl - 100;
    int w = file_open("0:/Z.BIN", FILE_MODE_WRITE);
    fill(want, 10, 0, len);
    CHECK(w > 0 && file_write(w, want, len) == len && fclose(w) == 0, "Z.BIN, %d bytes", len);
    int a = file_open("0:/Z.BIN", FILE_MODE_APPEND);
    int b = file_open("0:/Z.BIN", FILE_MODE_APPEND);
    CHECK(a > 0 && b > 0, "two append handles");
    // A grows the file into cluster 2, B appends in it, A grows it by two clusters, B by one,
    // A by a few bytes.
    const int steps[] = { 200, 200, 2 * cl + 10, cl, 30 };
    for (int i = 0; i < 5; i++)
    {
        int n = steps[i];
        int fd = i % 2 ? b : a;
        fill(want + len, 11 + i, len, n);
        int res = file_write(fd, want + len, n);
        len += n;
        CHECK(res == n && file_seek(fd, 0, SEEK_CUR) == len, "%s appends %d bytes: %d, position %d (want %d)",
              i % 2 ? "B" : "A", n, res, file_seek(fd, 0, SEEK_CUR), len);
    }
    CHECK(fclose(a) == 0 && fclose(b) == 0, "close");
    check_file("0:/Z.BIN", "Z       BIN", want, len);
    export_image("two-appenders");
}

static void c_same_size(void)
{
    // The append stays in the last cluster: with the chain still known, no link to follow.
    mount_holes();
    int bytes = 4 * cl - 200;
    int w = file_open("0:/S.BIN", FILE_MODE_WRITE);
    fill(buf, 12, 0, bytes);
    CHECK(w > 0 && file_write(w, buf, bytes) == bytes, "S.BIN, 4 clusters on the holes");
    int a = file_open("0:/S.BIN", FILE_MODE_APPEND);
    CHECK(a > 0 && file_write(a, buf, 0) == 0, "append handle");
    // The peer rewrites bytes inside the file: same size, same clusters.
    CHECK(file_seek(w, 100, SEEK_SET) == 100, "peer seeks");
    fill(buf + 100, 13, 100, 64);
    CHECK(file_write(w, buf + 100, 64) == 64, "peer rewrites 64 bytes");
    fimg_reset_counts();
    fill(buf + bytes, 14, bytes, 64);
    CHECK(file_write(a, buf + bytes, 64) == 64, "append after the peer's write");
    CHECK(link_reads() == 0, "%d link reads for the append (want 0)", link_reads());
    CHECK(fclose(a) == 0 && fclose(w) == 0, "close");
    check_file("0:/S.BIN", "S       BIN", buf, bytes + 64);
    export_image("same-size");
}

static void c_no_space(void)
{
    // The drive ends right after the last cluster in use: the free clusters are the ones below it
    // plus one.
    int last = 2;
    for (int c = 2; c < clusters + 2; c++)
    {
        if (durable_entry(0, c) || durable_entry(1, c))
        {
            last = c;
        }
    }
    fimg_set_capacity((unsigned int)(data_lba + (long)last * spc));
    CHECK(fimg_mount() == 0, "mount");
    int free_clusters = 0;
    for (int c = 2; c <= last + 1; c++)
    {
        free_clusters += !durable_entry(0, c) && !durable_entry(1, c);
    }
    CHECK(free_clusters >= 1 && free_clusters <= 4, "%d free clusters", free_clusters);

    // A file with one cluster, then a write that needs every free cluster and one more.
    int fd = file_open("0:/FULL.BIN", FILE_MODE_WRITE);
    fill(buf, 15, 0, 100);
    CHECK(fd > 0 && file_write(fd, buf, 100) == 100, "FULL.BIN, 100 bytes");
    free_clusters--;
    int want = free_clusters * cl + cl - 100 + 1;
    const unsigned char **v = fimg_view_live();
    static unsigned char before[2048 * S];
    long sectors = fimg_sectors() < 2048 ? fimg_sectors() : 2048;  // everything up to past the last cluster
    CHECK(data_lba + (long)(last + 1) * spc <= sectors, "the snapshot covers the volume");
    for (long s = 0; s < sectors; s++)
    {
        memcpy(before + s * S, (void *)v[s], S);
    }
    fimg_reset_counts();
    int live = kalloc_live();
    fill(buf + 100, 16, 100, want);
    int res = file_write(fd, buf + 100, want);
    CHECK(res == -ENOSPC, "a write of %d bytes: %d (want -ENOSPC)", want, res);
    CHECK(fimg_counts.writes == 0 && fimg_counts.flushes == 0, "%d writes and %d flushes (want none)", fimg_counts.writes,
          fimg_counts.flushes);
    v = fimg_view_live();
    long differ = 0;
    for (long s = 0; s < sectors; s++)
    {
        differ += memcmp(before + s * S, (void *)v[s], S) != 0;
    }
    CHECK(differ == 0, "%ld sectors changed", differ);
    CHECK(kalloc_live() == live, "%d heap allocations left (want 0)", kalloc_live() - live);
    CHECK(file_seek(fd, 0, SEEK_CUR) == 100, "position still 100");
    // What fits still works.
    want--;
    CHECK(file_write(fd, buf + 100, want) == want, "a write of %d bytes after it", want);
    CHECK(fclose(fd) == 0, "close");
    check_file("0:/FULL.BIN", "FULL    BIN", buf, 100 + want);
    export_image("no-space");
}

static void c_failed(void)
{
    // A FAT read fails before any data is written: the error, the position stays, the next
    // write lands where it should.
    mount_holes();
    int bytes = HOLES * cl;
    int fd = file_open("0:/FRAG.BIN", FILE_MODE_WRITE);
    fill(buf, 17, 0, bytes);
    CHECK(fd > 0 && file_write(fd, buf, bytes) == bytes, "FRAG.BIN on the holes");
    int at = 4 * cl + 10;
    CHECK(file_seek(fd, at, SEEK_SET) == at, "seek to cluster 5");
    fimg_reset_counts();
    fimg_fail_read(1, -EIO);
    unsigned char junk[64];
    memset(junk, 0xEE, sizeof(junk));
    int res = file_write(fd, junk, 64);
    CHECK(res < 0 && fimg_counts.failed == 1 && fimg_counts.writes == 0, "write with the first FAT read failing: %d, %d writes",
          res, fimg_counts.writes);
    CHECK(file_seek(fd, 0, SEEK_CUR) == at, "position still %d", at);
    fill(buf + at, 18, at, 2 * cl);
    CHECK(file_write(fd, buf + at, 2 * cl) == 2 * cl, "the next write works");
    CHECK(fclose(fd) == 0, "close");
    check_file("0:/FRAG.BIN", "FRAG    BIN", buf, bytes);

    // The new cluster's FAT entry can't be written: a reader opened before keeps the old size and
    // bytes, and so does the media. Then the same with the directory entry failing, after the old
    // last cluster was linked to the new one in both FATs: the chain changed but the size didn't,
    // and a reader standing in the last cluster still reads its bytes and then the end.
    for (int fail_dir = 0; fail_dir < 2; fail_dir++)
    {
        CHECK(fimg_mount() == 0, "mount again");
        int r = file_open("0:/FRAG.BIN", FILE_MODE_READ);
        int ap = file_open("0:/FRAG.BIN", FILE_MODE_APPEND);
        CHECK(r > 0 && ap > 0, "reader and appender");
        CHECK(file_read(r, back, bytes - 10) == bytes - 10, "the reader reads up to 10 bytes before the end");
        fimg_reset_counts();
        fimg_ledger_reset();
        // The new cluster zeroed sector by sector, the data, then FAT 1 (or, for the directory
        // entry, the new entry in both FATs and the link in both FATs first).
        fimg_fail_write(cl / S + 1 + (fail_dir ? 5 : 1), -EIO);
        res = file_write(ap, junk, 64);
        int failed_lba = -1;
        for (int i = 0; i < fimg_ledger_count(); i++)
        {
            failed_lba = fimg_ledger(i)->status < 0 ? (int)fimg_ledger(i)->lba : failed_lba;
        }
        CHECK(res < 0 && (fail_dir ? failed_lba >= root_lba && failed_lba < data_lba
                                   : failed_lba >= reserved && failed_lba < reserved + spf),
              "append with a write of %s failing: %d (lba %d)", fail_dir ? "the directory" : "FAT 1", res, failed_lba);
        static int linked[HOLES + 2];
        CHECK(!fail_dir || live_chain("FRAG    BIN", linked, HOLES + 2) == HOLES + 1, "the chain has the new cluster");
        CHECK(file_seek(ap, 0, SEEK_CUR) == bytes, "appender's position still %d", bytes);
        res = file_read(r, back + bytes - 10, MAX_BYTES);
        CHECK(res == 10 && memcmp(back, buf, bytes) == 0, "the reader reads the last %d bytes (want the old 10)", res);
        CHECK(file_read(r, back, 1) == 0, "then the end of the file");
        CHECK(file_seek(r, 0, SEEK_SET) == 0 && file_read(r, back, MAX_BYTES) == bytes && memcmp(back, buf, bytes) == 0,
              "read again from the start: the old size and bytes");
        CHECK(live_size("FRAG    BIN") == bytes, "size on the disk %ld", live_size("FRAG    BIN"));
        CHECK(fclose(r) == 0 && fclose(ap) == 0, "close");
    }
}

// The ledger's writes and flushes as letters: D data, a/b an entry of a new cluster in FAT 1/2,
// t/u the old last cluster's entry in FAT 1/2, R root directory, | flush, ? anything else. Runs of
// D are one D.
static void ledger_letters(char *out, int max, int old_tail)
{
    int n = 0;
    for (int i = 0; i < fimg_ledger_count() && n + 1 < max; i++)
    {
        const struct fimg_record *r = fimg_ledger(i);
        char ch;
        if (r->kind == FIMG_READ)
        {
            continue;
        }
        if (r->kind == FIMG_FLUSH)
        {
            ch = '|';
        }
        else if (r->lba >= data_lba)
        {
            ch = 'D';
        }
        else if (r->lba >= root_lba)
        {
            ch = 'R';
        }
        else if (r->lba >= reserved && r->lba < reserved + 2 * spf)
        {
            int copy = r->lba >= reserved + spf;
            ch = old_tail && r->lba == fat_lba(copy, old_tail) ? "tu"[copy] : "ab"[copy];
        }
        else
        {
            ch = '?';
        }
        if (ch == 'D' && n && out[n - 1] == 'D')
        {
            continue;
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

static void c_order(void)
{
    mount_holes();
    char got[256];
    int fd = file_open("0:/ORD.BIN", FILE_MODE_WRITE);
    CHECK(fd > 0, "open ORD.BIN");
    // On the holes every cluster's entry is in a FAT sector of its own: one letter per entry.
    struct
    {
        const char *what;
        int bytes;
        const char *want;
    } steps[] = {
        { "first write of a new file", cl + cl / 2, "D|aa|bb|R|" },
        { "write inside the last cluster", 64, "D|R|" },
        { "write that grows the file by two clusters", 2 * cl, "D|aa|bb|t|u|R|" },
        { "write that grows it by one more", cl, "D|a|b|t|u|R|" },
    };
    static int chain[HOLES + 1];
    int len = 0;
    for (int i = 0; i < 4; i++)
    {
        int n = steps[i].bytes;
        int count = live_chain("ORD     BIN", chain, HOLES + 1);
        int tail = count > 0 ? chain[count - 1] : 0;
        fimg_ledger_reset();
        fill(buf + len, 19 + i, len, n);
        CHECK(file_write(fd, buf + len, n) == n, "%s", steps[i].what);
        len += n;
        ledger_letters(got, sizeof(got), tail);
        CHECK(strlen(got) == strlen(steps[i].want) && strncmp(got, steps[i].want, sizeof(got)) == 0, "%s: %s (want %s)", steps[i].what, got, steps[i].want);
        printf("    %s: %s\n", steps[i].what, got);
    }
    CHECK(fclose(fd) == 0, "close");
    check_file("0:/ORD.BIN", "ORD     BIN", buf, len);
    export_image("order");
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "overwrite", c_overwrite },
    { "growth", c_growth },
    { "peer truncate", c_peer_truncate },
    { "two appenders", c_two_appenders },
    { "same size", c_same_size },
    { "no space", c_no_space },
    { "failed write", c_failed },
    { "order", c_order },
};

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: fat-write-cursor <image> <dir>\n");
        return 2;
    }
    outdir = argv[2];
    fimg_open(argv[1]);
    const unsigned char *b = fimg_durable();
    reserved = le16(b + 14);
    spc = b[13];
    spf = le16(b + 22);
    root_entries = le16(b + 17);
    root_lba = reserved + 2 * spf;
    data_lba = root_lba + root_entries * 32 / S;
    clusters = (fimg_sectors() - data_lba) / spc;
    cl = spc * S;
    int last = 0;
    for (int c = 2; c < clusters + 2 && c < spf * S / 2; c++)
    {
        last = durable_entry(0, c) ? c : last;
    }
    for (int k = 0; k < HOLES; k++)
    {
        holes[k] = 256 * (k + 1) + 100;
    }
    if (le16(b + 11) != S || spc != 4 || b[16] != 2 || last >= holes[0] || holes[HOLES - 1] + 16 >= clusters ||
        durable_entry(0, holes[0]) || durable_entry(1, holes[0]))
    {
        printf("fat-write-cursor: unexpected image geometry (sector %u, cluster %ld sectors, %u FATs, %ld clusters, "
               "last cluster in use %d)\n", le16(b + 11), spc, b[16], clusters, last);
        return 2;
    }
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
    printf("fat-write-cursor host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
