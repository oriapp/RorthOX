/*
 * Host check for scripts/tests/path-release.sh: fopen() must give back the path it parsed on
 * every way out, after the filesystem is done with it. Builds the real kernel/fs/file.c,
 * kernel/fs/pparser.c, kernel/fs/fat/fat16.c and kernel/disk/streamer.c with the host C compiler.
 * Disk reads come from a copy of the RorthOX image (with a directory "dir" holding a second
 * blank.elf), and the heap is scripts/tests/host/kalloc.c (freed memory is poisoned, so a path
 * freed before the filesystem reads it makes the open fail). Each case runs in its own process.
 *
 * Every case checks fopen()'s result and that nothing it allocated is left afterwards (after
 * fclose() for the files that open), with no double free and no write to freed memory:
 *   0:/blank.elf and 0:/dir/blank.elf, opened, read back and closed
 *   0:/nosuch (no such file), 0:/blank.elf with mode "x", 1:/blank.elf (no disk 1),
 *   0:/blank.elf on a disk without a filesystem, 0:/ (only the root),
 *   0:/blank.elf with every descriptor slot taken, and with the descriptor allocation failing.
 *
 * Usage: path-release <image> <blank.elf>
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "string/string.h"
#include "fs/file.h"
#include "fs/pparser.h"
#include "disk/disk.h"

int fstat(int fd, struct file_stat *stat);
int fclose(int fd);
int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd);
extern struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

#define MAX_FILE (64 * 1024)

static int image_fd = -1;
static struct disk the_disk;
static unsigned char blank[MAX_FILE];
static int blank_size;

// Stubs for the disk driver and the screen.
struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    int size = total * RORTHOX_SECTOR_SIZE;
    if (idisk != &the_disk || pread(image_fd, buf, size, (long)lba * RORTHOX_SECTOR_SIZE) != size)
    {
        return -EIO;
    }
    return 0;
}

void print(const char *str) {}

// A fresh heap and disk 0 with FAT16 on it. Returns the number of live allocations.
static int setup_disk(void)
{
    kalloc_init();
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.type = RORTHOX_DISK_TYPE_REAL;
    the_disk.sector_size = RORTHOX_SECTOR_SIZE;
    fs_init();
    the_disk.filesystem = fs_resolve(&the_disk);
    if (!the_disk.filesystem)
    {
        printf("path-release: the test image did not resolve as FAT16\n");
        exit(2);
    }
    return kalloc_live();
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

// After a case: nothing new left allocated, no bad free, no write to freed memory.
static int clean(const char *what, int live_before)
{
    if (kalloc_live() != live_before)
    {
        printf("%s: %d allocations left\n", what, kalloc_live() - live_before);
        return 1;
    }
    if (kalloc_bad_frees())
    {
        printf("%s: kfree of something that was not allocated\n", what);
        return 1;
    }
    if (kalloc_poison_errors())
    {
        printf("%s: freed memory was written to\n", what);
        return 1;
    }
    return 0;
}

// The file opens, reads back as blank.elf and closes.
static int open_read_close(const char *what, const char *path)
{
    int live = setup_disk();
    int fd = fopen(path, "r");
    struct file_stat stat;
    if (fd <= 0 || fstat(fd, &stat) != 0 || (int)stat.filesize != blank_size)
    {
        printf("open %s: did not open with the size of blank.elf (fd %d)\n", path, fd);
        return 1;
    }
    unsigned char *data = kzalloc(blank_size);
    if (!data || fread(data, blank_size, 1, fd) != 1 || memcmp(data, blank, blank_size) != 0)
    {
        printf("open %s: did not read back blank.elf\n", path);
        return 1;
    }
    kfree(data);
    if (fclose(fd) != 0 || !no_descriptors())
    {
        printf("open %s: fclose failed\n", path);
        return 1;
    }
    return clean(what, live);
}

// fopen(path, mode) must return 0 and take no descriptor slot.
static int open_fails(const char *what, const char *path, const char *mode, int live)
{
    int fd = fopen(path, mode);
    if (fd != 0)
    {
        printf("%s: fopen returned %d, expected 0\n", what, fd);
        return 1;
    }
    if (!no_descriptors())
    {
        printf("%s: a descriptor slot was filled\n", what);
        return 1;
    }
    return clean(what, live);
}

static int missing_file(void)
{
    int live = setup_disk();
    return open_fails("open 0:/nosuch", "0:/nosuch", "r", live);
}

static int bad_mode(void)
{
    int live = setup_disk();
    return open_fails("open 0:/blank.elf with mode x", "0:/blank.elf", "x", live);
}

static int missing_disk(void)
{
    int live = setup_disk();
    return open_fails("open 1:/blank.elf", "1:/blank.elf", "r", live);
}

static int no_filesystem(void)
{
    int live = setup_disk();
    the_disk.filesystem = 0;
    return open_fails("open 0:/blank.elf without a filesystem", "0:/blank.elf", "r", live);
}

static int root_only(void)
{
    int live = setup_disk();
    return open_fails("open 0:/", "0:/", "r", live);
}

// Every descriptor slot is taken (by something that isn't a real descriptor).
static int slots_full(void)
{
    static struct file_descriptor dummy;
    int live = setup_disk();
    for (int i = 0; i < RORTHOX_MAX_FILE_DESCRIPTORS; i++)
    {
        file_descriptors[i] = &dummy;
    }
    int fd = fopen("0:/blank.elf", "r");
    for (int i = 0; i < RORTHOX_MAX_FILE_DESCRIPTORS; i++)
    {
        if (file_descriptors[i] != &dummy)
        {
            printf("open with every slot taken: slot %d changed\n", i);
            return 1;
        }
        file_descriptors[i] = 0;
    }
    if (fd != 0)
    {
        printf("open with every slot taken: fopen returned %d, expected 0\n", fd);
        return 1;
    }
    return clean("open with every slot taken", live);
}

// The descriptor is fopen()'s last allocation: count the calls of a good open, then fail the last.
static int descriptor_fails(void)
{
    int live = setup_disk();
    int base = kalloc_calls();
    int fd = fopen("0:/blank.elf", "r");
    int calls = kalloc_calls() - base;
    if (fd <= 0 || calls < 1 || fclose(fd) != 0)
    {
        printf("open with the descriptor allocation failing: the first open failed\n");
        return 1;
    }
    live = setup_disk();
    kalloc_fail_at(kalloc_calls() + calls);
    int res = open_fails("open with the descriptor allocation failing", "0:/blank.elf", "r", live);
    if (!kalloc_failed())
    {
        printf("open with the descriptor allocation failing: the allocation was not made\n");
        return 1;
    }
    return res;
}

static int dispatch(int kind)
{
    switch (kind)
    {
    case 0: return open_read_close("open and close 0:/blank.elf", "0:/blank.elf");
    case 1: return open_read_close("open and close 0:/dir/blank.elf", "0:/dir/blank.elf");
    case 2: return missing_file();
    case 3: return bad_mode();
    case 4: return missing_disk();
    case 5: return no_filesystem();
    case 6: return root_only();
    case 7: return slots_full();
    case 8: return descriptor_fails();
    }
    return 1;
}

#define CASES 9

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: path-release <image> <blank.elf>\n");
        return 2;
    }
    image_fd = open(argv[1], O_RDONLY);
    int blank_fd = open(argv[2], O_RDONLY);
    if (image_fd < 0 || blank_fd < 0)
    {
        printf("path-release: cannot open the image or blank.elf\n");
        return 2;
    }
    blank_size = read(blank_fd, blank, sizeof(blank));
    if (blank_size <= 0 || blank_size == (int)sizeof(blank))
    {
        printf("path-release: blank.elf must be 1 to %d bytes\n", MAX_FILE - 1);
        return 2;
    }

    int failures = 0;
    for (int kind = 0; kind < CASES; kind++)
    {
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 2;
        }
        if (pid == 0)
        {
            int rc = dispatch(kind);
            fflush(NULL);
            _exit(rc);
        }
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            if (WIFSIGNALED(status))
            {
                printf("case %d: crashed (signal %d)\n", kind, WTERMSIG(status));
            }
            failures++;
        }
    }
    printf("path-release host check: %d of %d cases failed\n", failures, CASES);
    return failures ? 1 : 0;
}
