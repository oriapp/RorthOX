/*
 * Host check for scripts/tests/fat-entry.sh: what fat16.c does with each kind of FAT16 entry when
 * a read needs the next cluster. Builds the real kernel/fs/file.c, pparser.c, fat/fat16.c and
 * disk/streamer.c with the host C compiler; fat-host.c is the disk (a copy of the RorthOX image)
 * and changes the first link of pair.bin (2 clusters) on the way. Each case runs in its own process
 * and reads the whole file with one fread() from position 0.
 *
 *   marker  first link = 0x0000, 0x0001, every reserved value 0xFFF0-0xFFF6, the bad cluster mark
 *           0xFFF7 and every end of chain mark 0xFFF8-0xFFFF: fread() returns -EIO, and no seek or
 *           sector read goes anywhere but the first cluster's data and its own FAT entry (the
 *           value is never used as a cluster or looked up as a link).
 *   link    first link = 0x0FF0, 0x0FF6, 0x0FF7, 0x0FF8, 0x0FFF (FAT12 marks, but ordinary FAT16
 *           clusters; the model disk serves them from pair.bin's real second cluster and their own
 *           entry is end of chain): the whole file reads back.
 *   error   four.bin (4 clusters) with the k-th FAT entry read failing, for every k, with -ENOMEM
 *           (an error the FAT code doesn't make itself): fread() returns that error unchanged, and
 *           nothing is sought or read after the failed read (the error is never used as a cluster).
 *   start   the first cluster of a file (from its directory entry) given straight to fat16.c's
 *           static fat16_get_cluster_for_offset() at offset 0, which is why this file includes
 *           fat16.c: -1, 0, 1, every value 0xFFF0-0xFFFF and 0x10000 give -EIO, and 2, 3,
 *           0x0FF0, 0x0FF6, 0x0FF7, 0x0FF8, 0x0FFF and 0xFFEF come back as they are; neither
 *           seeks or reads the disk (0 or 1 used to be read as the sectors before the data area).
 * Plus pair.bin and four.bin as they are on disk.
 *
 * Usage: fat-entry <image> <pair.bin source> <four.bin source>
 * Prints one line per failing case and "fat-entry host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "fat-host.h"
#include "fs/fat/fat16.c"

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd);
void *malloc(unsigned long size);
void *memset(void *ptr, int c, unsigned long size);
int memcmp(void *s1, void *s2, int count);
// kernel/string/string.c has its own snprintf (which calls itself), so format with the C library's.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define host_snprintf(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)

static const char *pair_source, *four_source;

struct file
{
    const char *path;
    long size, n, chain[8];
    unsigned char *want;
};

static int load(struct file *f, const char *path, const char *name83, const char *source, int clusters)
{
    long src_size;
    f->path = path;
    f->want = fat_host_read_file(source, &src_size);
    if (fat_host_root_entry(name83, &f->chain[0], &f->size) != 0 || f->size != src_size)
    {
        printf("    %s: no root entry of %ld bytes\n", path, src_size);
        return 1;
    }
    f->n = fat_host_chain(f->chain[0], f->chain, 8);
    if (f->n != clusters)
    {
        printf("    %s: chain of %ld clusters, expected %d\n", path, f->n, clusters);
        return 1;
    }
    return 0;
}

// Open the file and read it whole with one fread() (logs cleared just before). Returns fread's result.
static int read_whole(struct file *f, unsigned char *data)
{
    int fd = fopen(f->path, "r");
    if (fd <= 0)
    {
        printf("    %s: did not open\n", f->path);
        return -1000;
    }
    memset(data, 0, f->size);
    fat_host_clear_logs();
    int res = fread(data, f->size, 1, fd);
    fclose(fd);
    return res;
}

static int seen_seek(long pos)
{
    for (int i = 0; i < seek_count; i++)
    {
        if (seek_log[i] == pos)
        {
            return 1;
        }
    }
    return 0;
}

// Every seek and sector read stayed in the data of the given clusters or at the link entries
// of the given clusters.
static int stayed_in(const char *what, const long *data_clusters, int nd, const long *links, int nl)
{
    for (int i = 0; i < seek_count; i++)
    {
        long pos = seek_log[i], c = cluster_of_pos(pos);
        int ok = 0;
        for (int k = 0; k < nd; k++)
        {
            ok |= c == data_clusters[k];
        }
        for (int k = 0; k < nl; k++)
        {
            ok |= pos == fat_entry_pos(links[k]);
        }
        if (!ok)
        {
            printf("    %s: seek to byte %ld (cluster %ld, FAT entry %ld)\n", what, pos, c,
                   (pos - geo.reserved_sectors * geo.sector_size) / 2);
            return 0;
        }
    }
    for (int i = 0; i < read_count; i++)
    {
        long pos = read_log[i] * geo.sector_size, c = cluster_of_pos(pos);
        int ok = 0;
        for (int k = 0; k < nd; k++)
        {
            ok |= c == data_clusters[k];
        }
        for (int k = 0; k < nl; k++)
        {
            ok |= read_log[i] == fat_entry_pos(links[k]) / geo.sector_size;
        }
        if (!ok)
        {
            printf("    %s: read sector %ld\n", what, read_log[i]);
            return 0;
        }
    }
    return 1;
}

static unsigned char data[8 << 20];

// The file as it is on disk reads back whole.
static int on_disk(const char *path, const char *name83, const char *source, int clusters)
{
    struct file f;
    fat_host_setup();
    if (load(&f, path, name83, source, clusters))
    {
        return 1;
    }
    int res = read_whole(&f, data);
    if (res != 1 || memcmp(data, f.want, f.size) != 0)
    {
        printf("    %s: fread returned %d or the bytes differ\n", path, res);
        return 1;
    }
    if (!stayed_in(path, f.chain, f.n, f.chain, f.n - 1))
    {
        return 1;
    }
    return 0;
}

static int c_pair(void) { return on_disk("0:/pair.bin", "PAIR    BIN", pair_source, 2); }
static int c_four(void) { return on_disk("0:/four.bin", "FOUR    BIN", four_source, 4); }

// pair.bin's first link is <value>, which can't be a next cluster.
static int marker(unsigned int value)
{
    struct file f;
    char what[64];
    fat_host_setup();
    if (load(&f, "0:/pair.bin", "PAIR    BIN", pair_source, 2))
    {
        return 1;
    }
    fat_host_set_entry(f.chain[0], value);
    int res = read_whole(&f, data);
    host_snprintf(what, sizeof(what), "first link %#06x", value);
    if (!seen_seek(fat_entry_pos(f.chain[0])))
    {
        printf("    %s: the link was never looked up\n", what);
        return 1;
    }
    if (!stayed_in(what, f.chain, 1, f.chain, 1))
    {
        return 1;
    }
    if (res != -EIO)
    {
        printf("    %s: fread returned %d, expected %d\n", what, res, -EIO);
        return 1;
    }
    return 0;
}

// pair.bin's first link is <value>, an ordinary FAT16 cluster that holds the second cluster's bytes.
static int link(unsigned int value)
{
    struct file f;
    char what[64];
    fat_host_setup();
    if (load(&f, "0:/pair.bin", "PAIR    BIN", pair_source, 2))
    {
        return 1;
    }
    fat_host_set_entry(f.chain[0], value);
    fat_host_set_entry(value, 0xFFFF);
    fat_host_map_cluster(value, f.chain[1]);
    int res = read_whole(&f, data);
    host_snprintf(what, sizeof(what), "first link %#06x", value);
    long used[2] = { f.chain[0], value };
    if (!stayed_in(what, used, 2, used, 1))
    {
        return 1;
    }
    if (res != 1 || memcmp(data, f.want, f.size) != 0)
    {
        printf("    %s: fread returned %d or the bytes differ, expected the whole file\n", what, res);
        return 1;
    }
    return 0;
}

// The k-th FAT entry read of four.bin fails.
static int lookup_fails(int k)
{
    struct file f;
    fat_host_setup();
    if (load(&f, "0:/four.bin", "FOUR    BIN", four_source, 4))
    {
        return 1;
    }
    int fd = fopen(f.path, "r");
    if (fd <= 0)
    {
        printf("    FAT read %d fails: four.bin did not open\n", k);
        return 1;
    }
    fat_host_clear_logs();
    fat_host_fail_fat_lookup(k, -ENOMEM);
    int res = fread(data, f.size, 1, fd);
    fclose(fd);
    if (!failed_reads)
    {
        printf("    FAT read %d fails: there were fewer FAT reads\n", k);
        return 1;
    }
    // Nothing after the failed read: it was the last seek's read and the last read.
    if (read_count != failed_read_index + 1 || seek_count != failed_seek_count)
    {
        printf("    FAT read %d fails: %d more seek(s) and %d more sector read(s) after the failed read"
               " (first seek after it: byte %ld)\n", k, seek_count - failed_seek_count,
               read_count - failed_read_index - 1, seek_count > failed_seek_count ? seek_log[failed_seek_count] : 0);
        return 1;
    }
    if (res != -ENOMEM)
    {
        printf("    FAT read %d fails: fread returned %d, expected the read's own error %d\n", k, res, -ENOMEM);
        return 1;
    }
    return 0;
}

#define MARKERS 18
static const unsigned int markers[MARKERS] = { 0x0000, 0x0001, 0xFFF0, 0xFFF1, 0xFFF2, 0xFFF3, 0xFFF4, 0xFFF5,
    0xFFF6, 0xFFF7, 0xFFF8, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFD, 0xFFFE, 0xFFFF };
#define LINKS 5
static const unsigned int links[LINKS] = { 0x0FF0, 0x0FF6, 0x0FF7, 0x0FF8, 0x0FFF };
// four.bin is read in 4 cluster steps from its start: 0 + 1 + 2 + 3 FAT reads.
#define FOUR_LOOKUPS 6

static int run_case(int (*fn)(int), int arg, const char *name, int index)
{
    fflush(0);
    pid_t pid = fork();
    if (pid == 0)
    {
        int res = fn(arg);
        fflush(0);
        _exit(res ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        printf("  FAIL %s %#x%s\n", name, index, WIFSIGNALED(status) ? " (crashed)" : "");
        return 1;
    }
    return 0;
}

#define STARTS_BAD 20
static const int starts_bad[STARTS_BAD] = { -1, 0x0000, 0x0001, 0xFFF0, 0xFFF1, 0xFFF2, 0xFFF3, 0xFFF4,
    0xFFF5, 0xFFF6, 0xFFF7, 0xFFF8, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFD, 0xFFFE, 0xFFFF, 0x10000 };
#define STARTS_OK 8
static const int starts_ok[STARTS_OK] = { 2, 3, 0x0FF0, 0x0FF6, 0x0FF7, 0x0FF8, 0x0FFF, 0xFFEF };

// A file whose directory entry starts at <start>: <expected> from fat16_get_cluster_for_offset(),
// and no seek or read.
static int start_cluster(int start, int expected)
{
    fat_host_setup();
    fat_host_clear_logs();
    int res = fat16_get_cluster_for_offset(disk_get(0), start, 0);
    if (res != expected || seek_count || read_count)
    {
        printf("    first cluster %#x: returned %d, expected %d (%d seek(s), %d read(s))\n", start, res,
               expected, seek_count, read_count);
        return 1;
    }
    return 0;
}

static int case_start_bad(int i) { return start_cluster(starts_bad[i], -EIO); }
static int case_start_ok(int i) { return start_cluster(starts_ok[i], starts_ok[i]); }

static int case_disk(int which) { return which ? c_four() : c_pair(); }
static int case_marker(int i) { return marker(markers[i]); }
static int case_link(int i) { return link(links[i]); }
static int case_lookup(int k) { return lookup_fails(k); }

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        printf("usage: fat-entry <image> <pair.bin source> <four.bin source>\n");
        return 2;
    }
    fat_host_open_image(argv[1]);
    pair_source = argv[2];
    four_source = argv[3];
    int cases = 0, failed = 0;
    for (int i = 0; i < 2; i++, cases++)
    {
        failed += run_case(case_disk, i, i ? "four.bin as on disk" : "pair.bin as on disk", i);
    }
    for (int i = 0; i < MARKERS; i++, cases++)
    {
        failed += run_case(case_marker, i, "first link", markers[i]);
    }
    for (int i = 0; i < LINKS; i++, cases++)
    {
        failed += run_case(case_link, i, "first link (ordinary cluster)", links[i]);
    }
    for (int k = 1; k <= FOUR_LOOKUPS; k++, cases++)
    {
        failed += run_case(case_lookup, k, "failed FAT read", k);
    }
    for (int i = 0; i < STARTS_BAD; i++, cases++)
    {
        failed += run_case(case_start_bad, i, "refused first cluster", starts_bad[i]);
    }
    for (int i = 0; i < STARTS_OK; i++, cases++)
    {
        failed += run_case(case_start_ok, i, "first cluster", starts_ok[i]);
    }
    printf("fat-entry host check: %d of %d cases failed\n", failed, cases);
    fflush(0);
    return failed ? 1 : 0;
}
