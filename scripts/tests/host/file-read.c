/*
 * Host check for scripts/tests/file-read.sh: fread() through the real kernel/fs/file.c,
 * pparser.c, fat/fat16.c and disk/streamer.c, built with the host C compiler, on a copy of the
 * RorthOX image (fat-host.c is the disk, kalloc.c the heap).
 *
 * The image has pat.bin (3 clusters and 333 bytes, its second cluster moved so the chain jumps),
 * small.bin (5000 bytes), empty.bin (0 bytes), huge.bin (its size field set to 0xFFFFFFF0),
 * far.bin (its first cluster set to 0xFF00, whose byte position doesn't fit an int) and a
 * directory "dir". Every case opens the file again on a fresh disk and heap:
 *
 *   next      three fread(100, 1) return 1 each and give bytes 0-99, 100-199, 200-299
 *   eof       a whole-file fread(size, 1) returns 1; the next fread returns 0 with no disk read
 *   tail      with 10 bytes left, fread(4, 3) returns 2 and copies exactly the 10 bytes
 *   empty     fread on empty.bin returns 0 with no disk read
 *   nmemb     fread(4096, 0x100001) of small.bin (4096 * 0x100001 wraps to 4096 in 32 bits)
 *             returns 1, copies the 5000 bytes and no more
 *   progress  fread(512, 3) with the second item's sector failing: an error, and the next read
 *             starts at byte 512 (the first item counts)
 *   failed    fread(2048, 1) with its third sector failing: an error, and the next read starts
 *             at byte 0
 *   boundary  fseek to cluster size - 7, fread(14, 1): the 7 bytes after the cluster come from
 *             the next cluster of the chain, not from the cluster next to it on disk
 *   directory fread on an open directory: an error, no disk read
 *   huge      fread(16, 1) at 0x7FFFFFF8 of huge.bin (the end doesn't fit an int): an error, no
 *             disk read, nothing copied
 *   far       fread of far.bin: an error, no disk read, nothing copied
 *   zero      fread with size 0 or nmemb 0 is -EINVARG, as before
 *
 * Usage: file-read <image> <pat.bin source> <small.bin source>
 * Prints "file-read host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "string/string.h"
#include "fs/file.h"
#include "fat-host.h"

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd);
void *malloc(unsigned long size);
void *memset(void *ptr, int c, unsigned long size);

#define CANARY 0xEE
#define BUF (8192 + 64)

static unsigned char *pat, *small;
static long pat_size, small_size;
static long pat_first, pat_chain[16];
static int pat_clusters;
static unsigned char buf[BUF];

static int open_file(const char *path)
{
    fat_host_setup();
    int fd = fopen(path, "r");
    if (fd <= 0)
    {
        printf("  FAIL %s: did not open\n", path);
        exit(1);
    }
    fat_host_clear_logs();
    memset(buf, CANARY, sizeof(buf));
    return fd;
}

static int same(const unsigned char *a, const unsigned char *b, long n)
{
    for (long i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

static int untouched(long from)
{
    for (long i = from; i < BUF; i++)
    {
        if (buf[i] != CANARY)
        {
            return 0;
        }
    }
    return 1;
}

#define FAIL(...) do { printf("  FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

// The byte at the file position, read with fread(1, 1).
static int next_byte(int fd)
{
    unsigned char b = 0;
    int res = fread(&b, 1, 1, fd);
    return res == 1 ? b : -1000 + res;
}

static int next_case(void)
{
    int fd = open_file("0:/pat.bin");
    for (int i = 0; i < 3; i++)
    {
        int res = fread(buf, 100, 1, fd);
        if (res != 1)
            FAIL("next: fread %d of 100 bytes returned %d, expected 1", i + 1, res);
        if (!same(buf, pat + 100 * i, 100))
            FAIL("next: fread %d gave the wrong bytes (byte 0 is %02x, the file has %02x at %d)", i + 1, buf[0],
                 pat[100 * i], 100 * i);
    }
    return 0;
}

static int eof_case(void)
{
    int fd = open_file("0:/pat.bin");
    unsigned char *all = malloc(pat_size);
    int res = fread(all, pat_size, 1, fd);
    if (res != 1 || !same(all, pat, pat_size))
        FAIL("eof: whole-file fread returned %d or gave the wrong bytes", res);
    fat_host_clear_logs();
    res = fread(buf, 1, 1, fd);
    if (res != 0)
        FAIL("eof: fread at the end of the file returned %d, expected 0", res);
    if (read_count || seek_count)
        FAIL("eof: fread at the end of the file made %d disk read(s) and %d seek(s)", read_count, seek_count);
    if (!untouched(0))
        FAIL("eof: fread at the end of the file wrote to the buffer");
    return 0;
}

static int tail_case(void)
{
    int fd = open_file("0:/small.bin");
    unsigned char *head = malloc(small_size);
    int res = fread(head, small_size - 10, 1, fd);
    if (res != 1)
        FAIL("tail: fread of the first %ld bytes returned %d, expected 1", small_size - 10, res);
    res = fread(buf, 4, 3, fd);
    if (res != 2)
        FAIL("tail: fread(4, 3) with 10 bytes left returned %d, expected 2", res);
    if (!same(buf, small + small_size - 10, 10))
        FAIL("tail: fread(4, 3) did not copy the last 10 bytes");
    if (!untouched(10))
        FAIL("tail: fread(4, 3) wrote past the 10 bytes left in the file");
    fat_host_clear_logs();
    res = fread(buf, 4, 1, fd);
    if (res != 0 || read_count || seek_count)
        FAIL("tail: the next fread returned %d with %d disk read(s), expected 0 and none", res, read_count);
    return 0;
}

static int empty_case(void)
{
    int fd = open_file("0:/empty.bin");
    int res = fread(buf, 1, 1, fd);
    if (res != 0)
        FAIL("empty: fread of a 0-byte file returned %d, expected 0", res);
    if (read_count || seek_count)
        FAIL("empty: fread of a 0-byte file made %d disk read(s) and %d seek(s)", read_count, seek_count);
    if (!untouched(0))
        FAIL("empty: fread of a 0-byte file wrote to the buffer");
    return 0;
}

static int nmemb_case(void)
{
    int fd = open_file("0:/small.bin");
    int res = fread(buf, 4096, 0x100001, fd);
    if (res != 1)
        FAIL("nmemb: fread(4096, 0x100001) of %ld bytes returned %d, expected 1", small_size, res);
    if (!same(buf, small, small_size))
        FAIL("nmemb: fread(4096, 0x100001) gave the wrong bytes");
    if (!untouched(small_size))
        FAIL("nmemb: fread(4096, 0x100001) wrote past the end of the file's %ld bytes", small_size);
    fat_host_clear_logs();
    res = fread(buf, 4096, 0x100001, fd);
    if (res != 0 || read_count)
        FAIL("nmemb: the next fread returned %d with %d disk read(s), expected 0 and none", res, read_count);
    return 0;
}

static int progress_case(void)
{
    int fd = open_file("0:/pat.bin");
    long sector = cluster_pos(pat_first) / geo.sector_size;
    fat_host_fail_sector(sector + 1, -EIO);
    int res = fread(buf, 512, 3, fd);
    if (!failed_reads)
        FAIL("progress: the second item's sector %ld was never read", sector + 1);
    if (res >= 0)
        FAIL("progress: fread(512, 3) with the second item failing returned %d, expected an error", res);
    if (!same(buf, pat, 512))
        FAIL("progress: the first item's bytes were not copied");
    fat_host_fail_sector(-1, 0);
    int b = next_byte(fd);
    if (b != pat[512])
        FAIL("progress: the next read gave %d, expected byte 512 of the file (%d); byte 0 is %d", b, pat[512], pat[0]);
    return 0;
}

static int failed_case(void)
{
    int fd = open_file("0:/pat.bin");
    long sector = cluster_pos(pat_first) / geo.sector_size;
    fat_host_fail_sector(sector + 2, -EIO);
    int res = fread(buf, 2048, 1, fd);
    if (!failed_reads)
        FAIL("failed: sector %ld was never read", sector + 2);
    if (res >= 0)
        FAIL("failed: fread(2048, 1) with its third sector failing returned %d, expected an error", res);
    fat_host_fail_sector(-1, 0);
    int b = next_byte(fd);
    if (b != pat[0])
        FAIL("failed: the next read gave %d, expected byte 0 of the file (%d)", b, pat[0]);
    return 0;
}

static int in_pat_chain(long cluster)
{
    for (int i = 0; i < pat_clusters; i++)
    {
        if (pat_chain[i] == cluster)
        {
            return 1;
        }
    }
    return 0;
}

static int boundary_case(void)
{
    int fd = open_file("0:/pat.bin");
    long at = geo.cluster_bytes - 7;
    int res = fseek(fd, at, SEEK_SET);
    if (res != 0)
        FAIL("boundary: fseek to %ld returned %d", at, res);
    res = fread(buf, 14, 1, fd);
    if (res != 1)
        FAIL("boundary: fread(14, 1) at %ld returned %d, expected 1", at, res);
    for (int i = 0; i < read_count; i++)
    {
        long c = cluster_of_pos(read_log[i] * geo.sector_size);
        if (c >= 0 && !in_pat_chain(c))
            FAIL("boundary: read sector %ld of cluster %ld, which is not in the chain of pat.bin", read_log[i], c);
    }
    if (!same(buf, pat + at, 14))
        FAIL("boundary: bytes %ld-%ld are wrong (byte 7 is %02x, expected %02x)", at, at + 13, buf[7], pat[at + 7]);
    if (!untouched(14))
        FAIL("boundary: fread(14, 1) wrote more than 14 bytes");
    return 0;
}

static int directory_case(void)
{
    int fd = open_file("0:/dir");
    int res = fread(buf, 1, 1, fd);
    if (res >= 0)
        FAIL("directory: fread of a directory returned %d, expected an error", res);
    if (read_count)
        FAIL("directory: fread of a directory made %d disk read(s)", read_count);
    if (!untouched(0))
        FAIL("directory: fread of a directory wrote to the buffer");
    return 0;
}

static int huge_case(void)
{
    int fd = open_file("0:/huge.bin");
    int res = fseek(fd, 0x7FFFFFF8, SEEK_SET);
    if (res != 0)
        FAIL("huge: fseek to 0x7FFFFFF8 of a 0xFFFFFFF0-byte file returned %d", res);
    fat_host_clear_logs();
    res = fread(buf, 16, 1, fd);
    if (res >= 0)
        FAIL("huge: fread(16, 1) at 0x7FFFFFF8 returned %d, expected an error", res);
    if (read_count)
        FAIL("huge: fread(16, 1) at 0x7FFFFFF8 made %d disk read(s) (first: sector %ld)", read_count, read_log[0]);
    if (!untouched(0))
        FAIL("huge: fread(16, 1) at 0x7FFFFFF8 wrote to the buffer");
    return 0;
}

static int far_case(void)
{
    int fd = open_file("0:/far.bin");
    int res = fread(buf, 100, 1, fd);
    if (res >= 0)
        FAIL("far: fread of a file in cluster 0xFF00 returned %d, expected an error", res);
    if (read_count)
        FAIL("far: fread of a file in cluster 0xFF00 made %d disk read(s) (first: sector %ld)", read_count,
             read_log[0]);
    if (!untouched(0))
        FAIL("far: fread of a file in cluster 0xFF00 wrote to the buffer");
    return 0;
}

static int zero_case(void)
{
    int fd = open_file("0:/small.bin");
    int a = fread(buf, 0, 1, fd), b = fread(buf, 1, 0, fd);
    if (a != -EINVARG || b != -EINVARG)
        FAIL("zero: fread with size 0 / nmemb 0 returned %d / %d, expected -EINVARG", a, b);
    if (next_byte(fd) != small[0])
        FAIL("zero: fread with size 0 or nmemb 0 moved the position");
    return 0;
}

static int (*const cases[])(void) = { next_case, eof_case, tail_case, empty_case, nmemb_case, progress_case,
                                      failed_case, boundary_case, directory_case, huge_case, far_case,
                                      zero_case };
static const char *names[] = { "next", "eof", "tail", "empty", "nmemb", "progress",
                               "failed", "boundary", "directory", "huge", "far", "zero" };

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        printf("usage: file-read <image> <pat.bin> <small.bin>\n");
        return 2;
    }
    fat_host_open_image(argv[1]);
    pat = fat_host_read_file(argv[2], &pat_size);
    small = fat_host_read_file(argv[3], &small_size);
    long size;
    if (fat_host_root_entry("PAT     BIN", &pat_first, &size) != 0 || size != pat_size ||
        (pat_clusters = fat_host_chain(pat_first, pat_chain, 16)) < 3 || pat_chain[1] == pat_chain[0] + 1 ||
        small_size + 64 > BUF || small_size <= 4096 || pat_size < 3 * geo.cluster_bytes)
    {
        printf("file-read: pat.bin must be 3 or more clusters with a jump after the first, small.bin 4097 to %d bytes\n",
               BUF - 64);
        return 2;
    }

    int failed = 0, total = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            int rc = cases[i]();
            fflush(0);
            _exit(rc);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            printf("  ok   %s\n", names[i]);
            continue;
        }
        if (WIFSIGNALED(status))
        {
            printf("  FAIL %s: crashed (signal %d)\n", names[i], WTERMSIG(status));
        }
        failed++;
    }
    printf("file-read host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
