/*
 * Host check for scripts/tests/file-descriptors.sh: the file descriptor table of the real
 * kernel/fs/file.c (with pparser.c, fat/fat16.c and disk/streamer.c), built with the host C
 * compiler, on a copy of the RorthOX image (fat-host.c is the disk, kalloc.c the heap).
 * Descriptors are 1 to RORTHOX_MAX_FILE_DESCRIPTORS and use slots 0 to MAX - 1.
 *
 *   fill    MAX opens of 0:/blank.elf give 1, 2, ..., MAX; one more open returns 0 and keeps
 *           nothing; fstat, fread, fseek and fclose work on descriptor MAX; closing every
 *           descriptor empties the table and gives every allocation back
 *   reuse   with the table full, closing MAX and opening again gives MAX; closing 1 and opening
 *           again gives 1
 *   bad     0, -1, MAX + 1, INT_MIN and INT_MAX are refused by fstat, fread, fseek and fclose,
 *           and descriptor 1 still works afterwards
 *
 * Usage: file-descriptors <image> <blank.elf>
 * Prints "file-descriptors host check: <failed> of <cases> cases failed".
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
extern struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

#define MAX RORTHOX_MAX_FILE_DESCRIPTORS
#define PATH "0:/blank.elf"

static unsigned char *blank;
static long blank_size;

#define FAIL(...) do { printf("  FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

static int table_empty(void)
{
    for (int i = 0; i < MAX; i++)
    {
        if (file_descriptors[i])
        {
            return 0;
        }
    }
    return 1;
}

// Open PATH MAX times; the descriptors must be 1..MAX in order.
static int fill(void)
{
    for (int i = 1; i <= MAX; i++)
    {
        int fd = fopen(PATH, "r");
        if (fd != i)
        {
            printf("  FAIL open number %d returned %d, expected %d\n", i, fd, i);
            return 1;
        }
    }
    return 0;
}

// fstat, fread, fseek and a one-byte fread on <fd>.
static int use(const char *what, int fd)
{
    struct file_stat stat;
    int res = fstat(fd, &stat);
    if (res != 0 || (long)stat.filesize != blank_size)
        FAIL("%s: fstat(%d) returned %d", what, fd, res);
    unsigned char *data = malloc(blank_size);
    res = fread(data, blank_size, 1, fd);
    if (res != 1 || memcmp(data, blank, blank_size) != 0)
        FAIL("%s: fread on %d returned %d or the bytes are wrong", what, fd, res);
    res = fseek(fd, 0, SEEK_SET);
    if (res != 0)
        FAIL("%s: fseek(%d, 0, SEEK_SET) returned %d", what, fd, res);
    unsigned char b = 0;
    res = fread(&b, 1, 1, fd);
    if (res != 1 || b != blank[0])
        FAIL("%s: fread of one byte on %d after fseek returned %d", what, fd, res);
    return 0;
}

static int heap_back(const char *what, int live)
{
    if (!table_empty())
        FAIL("%s: a descriptor slot is still taken", what);
    if (kalloc_live() != live)
        FAIL("%s: %d allocations left", what, kalloc_live() - live);
    if (kalloc_bad_frees() || kalloc_poison_errors())
        FAIL("%s: bad kfree or write after kfree", what);
    return 0;
}

static int fill_case(void)
{
    fat_host_setup();
    int live = kalloc_live();
    if (fill())
        return 1;
    int calls = kalloc_calls(), full_live = kalloc_live();
    int fd = fopen(PATH, "r");
    if (fd != 0)
        FAIL("fill: open number %d returned %d, expected 0", MAX + 1, fd);
    if (kalloc_calls() == calls)
        FAIL("fill: open number %d did not get as far as allocating", MAX + 1);
    if (kalloc_live() != full_live)
        FAIL("fill: open number %d kept %d allocations", MAX + 1, kalloc_live() - full_live);
    if (use("fill", MAX))
        return 1;
    int res = fclose(MAX);
    if (res != 0 || file_descriptors[MAX - 1])
        FAIL("fill: fclose(%d) returned %d", MAX, res);
    for (int i = 1; i < MAX; i++)
    {
        res = fclose(i);
        if (res != 0)
            FAIL("fill: fclose(%d) returned %d", i, res);
    }
    return heap_back("fill", live);
}

static int reuse_case(void)
{
    fat_host_setup();
    int live = kalloc_live();
    if (fill())
        return 1;
    int res = fclose(MAX);
    if (res != 0)
        FAIL("reuse: fclose(%d) returned %d", MAX, res);
    int fd = fopen(PATH, "r");
    if (fd != MAX)
        FAIL("reuse: opening after fclose(%d) returned %d, expected %d", MAX, fd, MAX);
    if (use("reuse", MAX))
        return 1;
    res = fclose(1);
    fd = fopen(PATH, "r");
    if (res != 0 || fd != 1)
        FAIL("reuse: fclose(1) returned %d, opening again gave %d, expected 1", res, fd);
    for (int i = 1; i <= MAX; i++)
    {
        res = fclose(i);
        if (res != 0)
            FAIL("reuse: fclose(%d) returned %d", i, res);
    }
    return heap_back("reuse", live);
}

static int bad_case(void)
{
    static const int bad[] = { 0, -1, MAX + 1, -0x7FFFFFFF - 1, 0x7FFFFFFF };
    fat_host_setup();
    int live = kalloc_live();
    int fd = fopen(PATH, "r");
    if (fd != 1)
        FAIL("bad: the first open returned %d", fd);
    int open_live = kalloc_live();
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        struct file_stat stat;
        unsigned char b;
        int s = fstat(bad[i], &stat), r = fread(&b, 1, 1, bad[i]), k = fseek(bad[i], 0, SEEK_SET), c = fclose(bad[i]);
        if (s >= 0 || r >= 0 || k >= 0 || c >= 0)
            FAIL("bad: descriptor %d: fstat %d, fread %d, fseek %d, fclose %d, expected errors", bad[i], s, r, k, c);
    }
    if (kalloc_live() != open_live)
        FAIL("bad: refused calls changed the heap");
    if (use("bad", 1))
        return 1;
    if (fclose(1) != 0)
        FAIL("bad: fclose(1) failed");
    return heap_back("bad", live);
}

static int (*const cases[])(void) = { fill_case, reuse_case, bad_case };
static const char *names[] = { "fill", "reuse", "bad" };

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: file-descriptors <image> <blank.elf>\n");
        return 2;
    }
    fat_host_open_image(argv[1]);
    blank = fat_host_read_file(argv[2], &blank_size);

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
    printf("file-descriptors host check: %d of %d cases failed (%d descriptors)\n", failed, total, MAX);
    fflush(0);
    return failed ? 1 : 0;
}
