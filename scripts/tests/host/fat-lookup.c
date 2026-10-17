/*
 * Host check for scripts/tests/fat-lookup.sh: FAT16 directory lookup and fseek() through the real
 * kernel/fs/file.c, pparser.c, fat/fat16.c and disk/streamer.c, built with the host C compiler,
 * on copies of the RorthOX image (fat-host.c is the disk, kalloc.c the heap).
 *
 * deleted image: the root holds a deleted slot (zap.elf) right before later.elf, and directory
 *   ldir the same.
 *   later      0:/later.elf and 0:/ldir/later.elf open and read back as blank.elf
 *   gone       the deleted entry ("\xE5" "ap.elf") is not found, in the root or in ldir
 * full image: all root slots are used (no 0x00 slot), last.bin in the last one.
 *   root       setting up the disk reads no sector past the root directory; 0:/last.bin opens
 *              and reads back; a missing name is not found without any disk read, and neither
 *              is a name that only exists in stale heap bytes after the root directory buffer
 *              (looked up with fat16_get_directory_entry() before anything else is allocated)
 * odd image (fixtures a FAT16 tool doesn't make): two root entries named DUP1.BIN (1000 and
 *   2000 bytes), hiword.bin with its high first-cluster word set to 0x4000, seek.bin (5000
 *   bytes) and empty.bin (0 bytes).
 *   first      0:/dup1.bin is the first DUP1.BIN (1000 bytes), only one entry copy is made, and
 *              fclose() gives everything back
 *   hiword     0:/hiword.bin reads back from its low-word cluster
 *   seek-*     SEEK_SET to 0, to the end, past the end; SEEK_CUR to the end, past it, with an
 *              oversized offset and with offsets whose sum with the position wraps past 32 bits
 *              (back to an earlier byte, or to 0); SEEK_END (still -EUNIMP) and a bad mode (-EINVARG); the same on
 *              empty.bin. A failed seek keeps the position (checked by reading the next byte), and
 *              a read at the end returns 0 without a disk read.
 *   missing    (deleted image) fopen_status() gives -ENOENT for a name that is nowhere, in the
 *              root, in ldir and under a missing directory, and fopen() still gives 0. A file
 *              used as a directory, a path with no file, a missing disk and bad syntax give
 *              other errors, and so does every failed allocation and disk read on the way to
 *              later.elf, ldir/later.elf, nosuch.elf and ldir/nosuch.elf (nothing left
 *              allocated). Program lookup tries the next directory only on -ENOENT.
 *
 * Usage: fat-lookup <deleted image> <full image> <odd image> <blank.elf> <last.bin> <dup1.bin>
 *                   <hiword.bin> <seek.bin>
 * Prints "fat-lookup host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "kernel.h"
#include "string/string.h"
#include "fs/file.h"
#include "fs/pparser.h"
#include "disk/disk.h"
#include "fat-host.h"

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd);
void *malloc(unsigned long size);
void *fat16_get_directory_entry(struct disk *disk, struct path_part *path);
extern struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

static const char *images[3];
static unsigned char *blank, *last, *dup1, *hiword, *seekf;
static long blank_size, last_size, dup1_size, hiword_size, seek_size;

#define FAIL(...) do { printf("  FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

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

// Open <path>, check its size and bytes, close it; 0 if all is well.
static int read_back(const char *path, const unsigned char *want, long want_size)
{
    int fd = fopen(path, "r");
    struct file_stat stat;
    if (fd <= 0)
        FAIL("%s: not found", path);
    if (fstat(fd, &stat) != 0 || (long)stat.filesize != want_size)
        FAIL("%s: size %u, expected %ld", path, stat.filesize, want_size);
    unsigned char *data = malloc(want_size + 1);
    int res = fread(data, want_size, 1, fd);
    if (res != 1 || !same(data, want, want_size))
        FAIL("%s: fread returned %d or the bytes are wrong", path, res);
    if (fclose(fd) != 0)
        FAIL("%s: fclose failed", path);
    return 0;
}

static int later_case(void)
{
    fat_host_open_image(images[0]);
    fat_host_setup();
    return read_back("0:/later.elf", blank, blank_size) || read_back("0:/ldir/later.elf", blank, blank_size);
}

static int gone_case(void)
{
    fat_host_open_image(images[0]);
    fat_host_setup();
    int live = kalloc_live();
    int a = fopen("0:/\xE5" "ap.elf", "r");
    int b = fopen("0:/ldir/\xE5" "ap.elf", "r");
    if (a != 0 || b != 0)
        FAIL("a deleted entry was opened (root %d, ldir %d)", a, b);
    if (kalloc_live() != live)
        FAIL("looking for a deleted entry left %d allocations", kalloc_live() - live);
    return 0;
}

// Stale heap bytes after the root directory buffer that look like entries named STALE.BIN: a root
// scan that doesn't stop at the BPB's root entry count would find them. The pattern repeats every
// 16 bytes (the host heap's alignment), so every 32-byte step through it starts with the name.
static void stale_heap(void)
{
    unsigned char record[16];
    memset(record, 0, sizeof(record));
    memcpy(record, "STALE   BIN", 11);
    record[11] = 0x20;
    kalloc_fill_unused(record, sizeof(record), 256 * 1024);
}

static int root_case(void)
{
    fat_host_open_image(images[1]);
    fat_host_before_resolve = stale_heap;
    fat_host_setup();
    if (setup_max_lba < 0 || setup_max_lba >= geo.data_sector)
        FAIL("root: setting up the disk read sector %ld, past the root directory (data starts at %ld)",
             setup_max_lba, geo.data_sector);
    // Look the stale name up before anything else is allocated after the root directory buffer
    // (fopen() would put its parsed path there first).
    struct path_part stale = { "stale.bin", 0 };
    if (fat16_get_directory_entry(disk_get(0), &stale) != ERROR(-ENOENT))
        FAIL("root: STALE.BIN, which is only in heap memory past the root directory, was found");
    if (read_back("0:/last.bin", last, last_size))
        return 1;
    fat_host_clear_logs();
    int fd = fopen("0:/nosuch.bin", "r");
    if (fd != 0)
        FAIL("root: 0:/nosuch.bin opened (%d)", fd);
    if (read_count)
        FAIL("root: looking for 0:/nosuch.bin read %d sector(s) (first: %ld)", read_count, read_log[0]);
    return 0;
}

static int first_case(void)
{
    fat_host_open_image(images[2]);
    fat_host_setup();
    int live = kalloc_live();
    int calls = kalloc_calls();
    int fd = fopen("0:/dup1.bin", "r");
    int copies = 0;
    for (int n = calls + 1; n <= kalloc_calls(); n++)
    {
        copies += strncmp(kalloc_caller(n), "fat_16_clone_directory_item", 40) == 0;
    }
    if (fd <= 0)
        FAIL("first: 0:/dup1.bin not found");
    if (copies != 1)
        FAIL("first: %d directory entry copies made for one open, expected 1", copies);
    fclose(fd);
    if (kalloc_live() != live || kalloc_bad_frees() || kalloc_poison_errors())
        FAIL("first: %d allocations left after fclose (bad frees %d)", kalloc_live() - live, kalloc_bad_frees());
    return read_back("0:/dup1.bin", dup1, dup1_size);
}

static int hiword_case(void)
{
    fat_host_open_image(images[2]);
    fat_host_setup();
    return read_back("0:/hiword.bin", hiword, hiword_size);
}

// ---- fseek ----

static int seek_fd;

static int open_seek(const char *path)
{
    fat_host_open_image(images[2]);
    fat_host_setup();
    seek_fd = fopen(path, "r");
    if (seek_fd <= 0)
    {
        printf("  FAIL %s: not found\n", path);
        exit(1);
    }
    return seek_fd;
}

// The next byte read, or a negative/large value for "none".
static int next_byte(void)
{
    unsigned char b;
    int res = fread(&b, 1, 1, seek_fd);
    return res == 1 ? b : res == 0 ? -1000 : -2000 + res;
}

#define SEEK_OK(off, mode) do { int r_ = fseek(seek_fd, (off), (mode)); \
    if (r_ != 0) FAIL("%s: fseek(%ld, %s) returned %d, expected 0", what, (long)(off), #mode, r_); } while (0)
#define SEEK_BAD(off, mode) do { int r_ = fseek(seek_fd, (off), (mode)); \
    if (r_ >= 0) FAIL("%s: fseek(%ld, %s) returned %d, expected an error", what, (long)(off), #mode, r_); } while (0)
#define AT(byte_pos) do { int b_ = next_byte(); \
    if (b_ != seekf[(byte_pos)]) FAIL("%s: next byte %d, expected byte %ld (%d)", what, b_, (long)(byte_pos), seekf[(byte_pos)]); } while (0)
#define AT_END() do { fat_host_clear_logs(); int b_ = next_byte(); \
    if (b_ != -1000) FAIL("%s: read at the end gave %d, expected 0 items", what, b_); \
    if (read_count) FAIL("%s: read at the end made %d disk read(s)", what, read_count); } while (0)

static int seek_set_case(void)
{
    const char *what = "seek-set";
    long s = seek_size;
    open_seek("0:/seek.bin");
    SEEK_OK(0, SEEK_SET);
    AT(0);
    SEEK_OK(s, SEEK_SET);
    AT_END();
    SEEK_OK(100, SEEK_SET);
    SEEK_BAD(s + 1, SEEK_SET);
    AT(100);
    SEEK_BAD(-1, SEEK_SET);
    AT(101);
    return 0;
}

static int seek_cur_case(void)
{
    const char *what = "seek-cur";
    long s = seek_size;
    open_seek("0:/seek.bin");
    SEEK_OK(10, SEEK_SET);
    SEEK_OK(s - 10, SEEK_CUR);
    AT_END();
    SEEK_OK(100, SEEK_SET);
    SEEK_BAD(s - 50, SEEK_CUR);
    AT(100);
    SEEK_BAD((int)0xFFFFFF00, SEEK_CUR);
    AT(101);
    // pos + offset wraps: 102 + 0xFFFFFFFF = 101, 103 + 0xFFFFFF99 = 0.
    SEEK_BAD(-1, SEEK_CUR);
    AT(102);
    SEEK_BAD(-103, SEEK_CUR);
    AT(103);
    SEEK_OK(0, SEEK_CUR);
    AT(104);
    SEEK_OK(s - 105, SEEK_CUR);
    AT_END();
    SEEK_BAD(1, SEEK_CUR);
    AT_END();
    return 0;
}

static int seek_other_case(void)
{
    const char *what = "seek-other";
    open_seek("0:/seek.bin");
    SEEK_OK(200, SEEK_SET);
    int r = fseek(seek_fd, 0, SEEK_END);
    if (r != -EUNIMP)
        FAIL("%s: fseek(0, SEEK_END) returned %d, expected -EUNIMP", what, r);
    r = fseek(seek_fd, 0, 7);
    if (r != -EINVARG)
        FAIL("%s: fseek(0, mode 7) returned %d, expected -EINVARG", what, r);
    AT(200);
    return 0;
}

static int seek_empty_case(void)
{
    const char *what = "seek-empty";
    open_seek("0:/empty.bin");
    SEEK_OK(0, SEEK_SET);
    SEEK_OK(0, SEEK_CUR);
    AT_END();
    SEEK_BAD(1, SEEK_SET);
    SEEK_BAD(1, SEEK_CUR);
    AT_END();
    return 0;
}

// ---- not found (program lookup tries the next directory only for -ENOENT) ----

// fopen_status(path) on a fresh disk, with allocation call <fail_alloc> or disk read <fail_read>
// from now made to fail (0 = none). *failed says whether that failure happened.
static int open_status(const char *path, int fail_alloc, int fail_read, int *failed)
{
    fat_host_setup();
    int live = kalloc_live();
    kalloc_fail_at(fail_alloc ? kalloc_calls() + fail_alloc : 0);
    fat_host_fail_read(fail_read, -EIO);
    int res = fopen_status(path, "r");
    *failed = kalloc_failed() || failed_reads;
    if (res > 0)
    {
        fclose(res);
    }
    if (kalloc_live() != live || kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf("  FAIL missing: %s left %d allocation(s) or freed badly\n", path, kalloc_live() - live);
        return -1000;
    }
    return res;
}

static int missing_case(void)
{
    // Names that are nowhere: -ENOENT from fopen_status, 0 from fopen.
    static const char *absent[] = { "0:/nosuch.elf", "0:/ldir/nosuch.elf", "0:/nodir/later.elf" };
    // Not absence: a file used as a directory, no first part, no such disk, bad syntax.
    static const char *other[] = { "0:/later.elf/x.elf", "0:/", "5:/later.elf", "later.elf" };
    // Every allocation and disk read on the way to these must fail with something else.
    static const char *swept[] = { "0:/later.elf", "0:/ldir/later.elf", "0:/nosuch.elf", "0:/ldir/nosuch.elf" };
    int failed;
    fat_host_open_image(images[0]);
    for (int i = 0; i < 3; i++)
    {
        int res = open_status(absent[i], 0, 0, &failed);
        if (res != -ENOENT)
            FAIL("missing: fopen_status(%s) returned %d, expected %d", absent[i], res, -ENOENT);
        fat_host_setup();
        if ((res = fopen(absent[i], "r")) != 0)
            FAIL("missing: fopen(%s) returned %d, expected 0", absent[i], res);
    }
    for (int i = 0; i < 4; i++)
    {
        int res = open_status(other[i], 0, 0, &failed);
        if (res >= 0 || res == -ENOENT || res == -1000)
            FAIL("missing: fopen_status(%s) returned %d, expected an error other than %d", other[i], res, -ENOENT);
    }
    for (int i = 0; i < 4; i++)
    {
        int alloc_cases = 0, read_cases = 0;
        for (int n = 1; n < 100; n++, alloc_cases++)
        {
            int res = open_status(swept[i], n, 0, &failed);
            if (!failed)
                break;
            if (res >= 0 || res == -ENOENT || res == -1000)
                FAIL("missing: %s with allocation %d failing returned %d", swept[i], n, res);
        }
        for (int n = 1; n < 100; n++, read_cases++)
        {
            int res = open_status(swept[i], 0, n, &failed);
            if (!failed)
                break;
            if (res >= 0 || res == -ENOENT || res == -1000)
                FAIL("missing: %s with disk read %d failing returned %d", swept[i], n, res);
        }
        if (!alloc_cases)
            FAIL("missing: no allocation to fail for %s", swept[i]);
        if (i % 2 && !read_cases)
            FAIL("missing: no disk read to fail for %s", swept[i]);
    }
    return 0;
}

static int (*const cases[])(void) = { later_case, gone_case, root_case, first_case, hiword_case,
                                      seek_set_case, seek_cur_case, seek_other_case, seek_empty_case,
                                      missing_case };
static const char *names[] = { "later", "gone", "root", "first", "hiword", "seek-set", "seek-cur", "seek-other",
                               "seek-empty", "missing" };

int main(int argc, char **argv)
{
    if (argc != 9)
    {
        printf("usage: fat-lookup <deleted> <full> <odd> <blank.elf> <last.bin> <dup1.bin> <hiword.bin> <seek.bin>\n");
        return 2;
    }
    images[0] = argv[1];
    images[1] = argv[2];
    images[2] = argv[3];
    blank = fat_host_read_file(argv[4], &blank_size);
    last = fat_host_read_file(argv[5], &last_size);
    dup1 = fat_host_read_file(argv[6], &dup1_size);
    hiword = fat_host_read_file(argv[7], &hiword_size);
    seekf = fat_host_read_file(argv[8], &seek_size);
    if (seek_size < 300)
    {
        printf("fat-lookup: seek.bin must be 300 bytes or more\n");
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
    printf("fat-lookup host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
