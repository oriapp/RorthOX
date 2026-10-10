/*
 * Host check of the file open path (scripts/tests/open-oom.sh): the real kernel/fs/pparser.c,
 * kernel/fs/file.c, kernel/fs/fat/fat16.c and kernel/disk/streamer.c, built with the host C
 * compiler. Disk reads come from a copy of the RorthOX image (with a directory "dir" holding a
 * second blank.elf), and the heap is scripts/tests/host/kalloc.c, which can make any one
 * allocation call fail. Each case runs in its own process, so a crash fails that case only.
 *
 *   parse    pathparser_parse() of "0:/aa/bb/cc" with each allocation failing in turn: NULL and
 *            nothing left allocated (never a shorter path). Plus "0:/", "0:/aa//bb", the
 *            108-character limit, a bad drive and pathparser_free(NULL).
 *   resolve  fat16_resolve() with each allocation, then each disk read, failing in turn, and
 *            with a bad FAT signature: an error, the disk left without a filesystem, nothing
 *            left allocated.
 *   open     fopen("0:/blank.elf") and fopen("0:/dir/blank.elf") with each allocation failing in
 *            turn: 0, no descriptor slot taken, and nothing left allocated except, for now, the
 *            finished parsed path. Allocations inside fat16_load_fat_directory() are skipped:
 *            its failure path is a separate known bug. Without a failure the file opens, has the
 *            right size and contents, and fclose() gives everything back.
 *   walk     "0:/blank.elf/x" (a name after a file), "0:/dir/nosuch/x" (a missing directory in
 *            the middle) and "0:/nosuch": 0, no crash, nothing left allocated but the path.
 *
 * Usage: open-path <image> <blank.elf>
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
int fat16_resolve(struct disk *disk);
extern struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

#define MAX_CASE_ALLOCS 64
#define MAX_FILE (64 * 1024)

static int image_fd = -1;
static int read_count;
static int read_fail_at;
static int bad_signature;
static struct disk the_disk;
static unsigned char blank[MAX_FILE];
static int blank_size;
static int quiet;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

// Stubs for the disk driver and the screen.
struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (++read_count == read_fail_at)
    {
        return -EIO;
    }
    int size = total * RORTHOX_SECTOR_SIZE;
    if (pread(image_fd, buf, size, (long)lba * RORTHOX_SECTOR_SIZE) != size)
    {
        return -EIO;
    }
    if (lba == 0 && bad_signature)
    {
        // The extended BPB signature (0x29) at byte 38.
        ((unsigned char *)buf)[38] = 0;
    }
    return 0;
}

void print(const char *str) {}

static void disk_reset(void)
{
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.type = RORTHOX_DISK_TYPE_REAL;
    the_disk.sector_size = RORTHOX_SECTOR_SIZE;
    the_disk.id = 0;
    read_count = 0;
    read_fail_at = 0;
    bad_signature = 0;
}

// Heap state that every case must leave clean.
static int heap_clean(const char *what)
{
    if (kalloc_poison_errors())
    {
        say("%s: freed memory was written to\n", what);
        return 0;
    }
    if (kalloc_bad_frees())
    {
        say("%s: kfree of something that was not allocated\n", what);
        return 0;
    }
    return 1;
}

// ---- live allocations before and after a call ----

struct snapshot
{
    int n;
    void *ptr[512];
};

static void snapshot_take(struct snapshot *s)
{
    s->n = 0;
    void *p;
    while (s->n < 512 && (p = kalloc_live_at(s->n)))
    {
        s->ptr[s->n++] = p;
    }
}

// Live allocations that were not live in *before.
static int new_allocations(const struct snapshot *before, void **out, int max)
{
    int n = 0;
    void *p;
    for (int i = 0; (p = kalloc_live_at(i)); i++)
    {
        int old = 0;
        for (int j = 0; j < before->n; j++)
        {
            old |= before->ptr[j] == p;
        }
        if (!old && n < max)
        {
            out[n++] = p;
        }
    }
    return n;
}

static int in_set(void **set, int n, const void *p)
{
    for (int i = 0; i < n; i++)
    {
        if (set[i] == p)
        {
            return 1;
        }
    }
    return 0;
}

// 1 if set[0..n) is exactly one finished parse of the given components: the root, one node per
// component and its string. fopen() doesn't free the parsed path yet (a separate leak).
static int is_parsed_path(void **set, int n, const char **parts, int count)
{
    if (n != 1 + 2 * count)
    {
        return 0;
    }
    for (int i = 0; i < n; i++)
    {
        if (kalloc_size(set[i]) != sizeof(struct path_root))
        {
            continue;
        }
        struct path_root *root = set[i];
        struct path_part *part = root->first;
        int k = 0;
        while (k < count && part && in_set(set, n, part) && kalloc_size(part) == sizeof(struct path_part) &&
               in_set(set, n, part->part) && strncmp(part->part, parts[k], RORTHOX_MAX_PATH) == 0)
        {
            part = part->next;
            k++;
        }
        if (root->drive_no == 0 && k == count && !part)
        {
            return 1;
        }
    }
    return 0;
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

// ---- cases ----

static int parse_fail_case(int k)
{
    kalloc_init();
    kalloc_fail_at(k);
    struct path_root *root = pathparser_parse("0:/aa/bb/cc", NULL);
    if (!kalloc_failed())
    {
        // Past the last allocation: the whole path must be there.
        const char *parts[] = { "aa", "bb", "cc" };
        void *all[16];
        struct snapshot none = { 0 };
        int n = new_allocations(&none, all, 16);
        if (!root || !is_parsed_path(all, n, parts, 3))
        {
            say("parse 0:/aa/bb/cc: wrong result without a failure\n");
            return 1;
        }
        pathparser_free(root);
        if (kalloc_live() != 0)
        {
            say("parse 0:/aa/bb/cc: pathparser_free left %d allocations\n", kalloc_live());
            return 1;
        }
        return heap_clean("parse") ? 2 : 1;
    }
    if (root)
    {
        int parts = 0;
        for (struct path_part *p = root->first; p; p = p->next)
        {
            parts++;
        }
        say("parse 0:/aa/bb/cc, allocation %d failing: returned a path with %d of 3 parts\n", k, parts);
        return 1;
    }
    if (kalloc_live() != 0)
    {
        say("parse 0:/aa/bb/cc, allocation %d failing: %d allocations left\n", k, kalloc_live());
        return 1;
    }
    return heap_clean("parse") ? 0 : 1;
}

static int parse_shapes_case(void)
{
    char long_path[RORTHOX_MAX_PATH + 2];
    kalloc_init();

    struct path_root *root = pathparser_parse("0:/", NULL);
    if (!root || root->first || root->drive_no != 0)
    {
        say("parse 0:/: expected a root without parts\n");
        return 1;
    }
    pathparser_free(root);

    root = pathparser_parse("0:/aa//bb", NULL);
    if (!root || !root->first || strncmp(root->first->part, "aa", 3) != 0 || root->first->next)
    {
        say("parse 0:/aa//bb: expected only \"aa\" (an empty part ends the path)\n");
        return 1;
    }
    pathparser_free(root);

    // The parser takes paths up to RORTHOX_MAX_PATH characters.
    strcpy(long_path, "0:/");
    for (int i = 3; i < RORTHOX_MAX_PATH; i++)
    {
        long_path[i] = 'a';
    }
    long_path[RORTHOX_MAX_PATH] = 0;
    root = pathparser_parse(long_path, NULL);
    if (!root || !root->first || strlen(root->first->part) != RORTHOX_MAX_PATH - 3)
    {
        say("parse: a %d-character path was not taken whole\n", RORTHOX_MAX_PATH);
        return 1;
    }
    pathparser_free(root);
    long_path[RORTHOX_MAX_PATH] = 'a';
    long_path[RORTHOX_MAX_PATH + 1] = 0;
    if (pathparser_parse(long_path, NULL))
    {
        say("parse: a %d-character path was taken\n", RORTHOX_MAX_PATH + 1);
        return 1;
    }

    if (pathparser_parse("x:/aa", NULL))
    {
        say("parse x:/aa: a bad drive was taken\n");
        return 1;
    }

    pathparser_free(NULL);

    if (kalloc_live() != 0)
    {
        say("parse shapes: %d allocations left\n", kalloc_live());
        return 1;
    }
    return heap_clean("parse shapes") ? 0 : 1;
}

// mode 0: allocation k fails; 1: disk read k fails; 2: bad signature.
static int resolve_case(int mode, int k)
{
    kalloc_init();
    disk_reset();
    fs_init();
    if (mode == 0)
    {
        kalloc_fail_at(k);
    }
    else if (mode == 1)
    {
        read_fail_at = k;
    }
    else
    {
        bad_signature = 1;
    }
    int res = fat16_resolve(&the_disk);
    int failed = mode == 0 ? kalloc_failed() : mode == 1 ? read_count >= k : 1;
    char what[40];
    strcpy(what, mode == 0 ? "allocation " : "disk read ");
    strcpy(what + strlen(what), itoa(k));
    strcpy(what + strlen(what), " failing");

    if (!failed)
    {
        if (res != 0 || !the_disk.filesystem || !the_disk.fs_private)
        {
            say("resolve: failed without an injected failure (%d)\n", res);
            return 1;
        }
        return heap_clean("resolve") ? 2 : 1;
    }
    if (mode == 2)
    {
        strcpy(what, "bad FAT signature");
    }
    if (res >= 0)
    {
        say("resolve, %s: returned %d, expected an error\n", what, res);
        return 1;
    }
    if (the_disk.filesystem || the_disk.fs_private)
    {
        say("resolve, %s: the disk still has a filesystem%s\n", what,
            the_disk.fs_private ? " and private data" : "");
        return 1;
    }
    if (kalloc_live() != 0)
    {
        say("resolve, %s: %d allocations left\n", what, kalloc_live());
        return 1;
    }
    return heap_clean("resolve") ? 0 : 1;
}

static int setup_disk(void)
{
    kalloc_init();
    disk_reset();
    fs_init();
    the_disk.filesystem = fs_resolve(&the_disk);
    if (!the_disk.filesystem)
    {
        printf("open: the test image did not resolve as FAT16\n");
        exit(1);
    }
    return kalloc_calls();
}

static const char *blank_parts[] = { "blank.elf" };
static const char *dir_parts[] = { "dir", "blank.elf" };

// Allocation call k of fopen(path) fails (0 = none). Returns 0 pass, 1 fail, 2 pass and no failure
// happened, 3 skipped (the failing allocation was in fat16_load_fat_directory).
static int open_case(const char *path, const char **parts, int count, int k)
{
    struct snapshot before;
    int base = setup_disk();
    snapshot_take(&before);
    if (k)
    {
        kalloc_fail_at(base + k);
    }
    int fd = fopen(path, "r");
    if (k && kalloc_failed() && strncmp(kalloc_caller(base + k), "fat16_load_fat_directory", 30) == 0)
    {
        return 3;
    }
    if (k && !kalloc_failed())
    {
        k = 0;
    }

    void *extra[MAX_CASE_ALLOCS];
    if (k)
    {
        if (fd != 0)
        {
            say("open %s, allocation %d (%s) failing: fopen returned %d, expected 0\n", path, k,
                kalloc_caller(base + k), fd);
            return 1;
        }
        if (!no_descriptors())
        {
            say("open %s, allocation %d failing: a descriptor slot was filled\n", path, k);
            return 1;
        }
        int n = new_allocations(&before, extra, MAX_CASE_ALLOCS);
        if (n && !is_parsed_path(extra, n, parts, count))
        {
            say("open %s, allocation %d (%s) failing: %d allocations left\n", path, k, kalloc_caller(base + k), n);
            return 1;
        }
        return heap_clean(path) ? 0 : 1;
    }

    // No failure: the file must open and read back.
    kalloc_fail_at(0);
    struct file_stat stat;
    if (fd <= 0 || fstat(fd, &stat) != 0 || (int)stat.filesize != blank_size)
    {
        say("open %s: did not open with the size of blank.elf (fd %d)\n", path, fd);
        return 1;
    }
    unsigned char *data = kzalloc(blank_size);
    if (!data || fread(data, blank_size, 1, fd) != 1 || memcmp(data, blank, blank_size) != 0)
    {
        say("open %s: did not read back blank.elf\n", path);
        return 1;
    }
    kfree(data);
    if (fclose(fd) != 0 || !no_descriptors())
    {
        say("open %s: fclose failed\n", path);
        return 1;
    }
    int n = new_allocations(&before, extra, MAX_CASE_ALLOCS);
    if (n && !is_parsed_path(extra, n, parts, count))
    {
        say("open %s: %d allocations left after fclose\n", path, n);
        return 1;
    }
    return heap_clean(path) ? 2 : 1;
}

static int walk_case(const char *path, const char **parts, int count)
{
    struct snapshot before;
    setup_disk();
    snapshot_take(&before);
    int fd = fopen(path, "r");
    if (fd != 0)
    {
        say("open %s: returned %d, expected 0\n", path, fd);
        return 1;
    }
    void *extra[MAX_CASE_ALLOCS];
    int n = new_allocations(&before, extra, MAX_CASE_ALLOCS);
    if (n && !is_parsed_path(extra, n, parts, count))
    {
        say("open %s: %d allocations left\n", path, n);
        return 1;
    }
    if (!no_descriptors())
    {
        say("open %s: a descriptor slot was filled\n", path);
        return 1;
    }
    return heap_clean(path) ? 0 : 1;
}

// ---- runner ----

enum
{
    PARSE_FAIL,
    PARSE_SHAPES,
    RESOLVE_ALLOC,
    RESOLVE_READ,
    RESOLVE_SIGNATURE,
    OPEN_FILE,
    OPEN_DIR_FILE,
    WALK_AFTER_FILE,
    WALK_MISSING_DIR,
    WALK_MISSING
};

static int dispatch(int kind, int k)
{
    static const char *after_file[] = { "blank.elf", "x" };
    static const char *missing_dir[] = { "dir", "nosuch", "x" };
    static const char *missing[] = { "nosuch" };
    switch (kind)
    {
    case PARSE_FAIL: return parse_fail_case(k);
    case PARSE_SHAPES: return parse_shapes_case();
    case RESOLVE_ALLOC: return resolve_case(0, k);
    case RESOLVE_READ: return resolve_case(1, k);
    case RESOLVE_SIGNATURE: return resolve_case(2, 0);
    case OPEN_FILE: return open_case("0:/blank.elf", blank_parts, 1, k);
    case OPEN_DIR_FILE: return open_case("0:/dir/blank.elf", dir_parts, 2, k);
    case WALK_AFTER_FILE: return walk_case("0:/blank.elf/x", after_file, 2);
    case WALK_MISSING_DIR: return walk_case("0:/dir/nosuch/x", missing_dir, 3);
    case WALK_MISSING: return walk_case("0:/nosuch", missing, 1);
    }
    return 1;
}

static const char *kind_name[] = { "parse", "parse shapes", "resolve (allocation)", "resolve (disk read)",
                                   "resolve (signature)", "open 0:/blank.elf", "open 0:/dir/blank.elf",
                                   "open 0:/blank.elf/x", "open 0:/dir/nosuch/x", "open 0:/nosuch" };

static int cases;
static int failures;
static int skipped;
static int kind_failures[WALK_MISSING + 1];

// Runs one case in a child process: 0 pass, 1 fail, 2 pass without an injected failure, 3 skipped.
static int run(int kind, int k)
{
    // Show the first two failures of each kind; the count says how many there were.
    quiet = kind_failures[kind] >= 2;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        int rc = dispatch(kind, k);
        fflush(NULL);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc == 3)
    {
        skipped++;
        return rc;
    }
    cases++;
    if (rc == 0 || rc == 2)
    {
        return rc;
    }
    if (WIFSIGNALED(status))
    {
        if (k)
        {
            say("%s, failure %d: crashed (signal %d)\n", kind_name[kind], k, WTERMSIG(status));
        }
        else
        {
            say("%s: crashed (signal %d)\n", kind_name[kind], WTERMSIG(status));
        }
    }
    failures++;
    kind_failures[kind]++;
    return 1;
}

// Fails call 1, 2, ... until a run gets through without reaching the failing call.
static void run_each(int kind, int max)
{
    for (int k = 1; k <= max; k++)
    {
        int rc = run(kind, k);
        if (rc == 2)
        {
            return;
        }
    }
    say("%s: still failing after %d calls\n", kind_name[kind], MAX_CASE_ALLOCS);
    failures++;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: open-path <image> <blank.elf>\n");
        return 2;
    }
    image_fd = open(argv[1], O_RDONLY);
    int blank_fd = open(argv[2], O_RDONLY);
    if (image_fd < 0 || blank_fd < 0)
    {
        printf("open-path: cannot open the image or blank.elf\n");
        return 2;
    }
    blank_size = read(blank_fd, blank, sizeof(blank));
    if (blank_size <= 0 || blank_size == (int)sizeof(blank))
    {
        printf("open-path: blank.elf must be 1 to %d bytes\n", MAX_FILE - 1);
        return 2;
    }

    run_each(PARSE_FAIL, MAX_CASE_ALLOCS);
    run(PARSE_SHAPES, 0);
    run_each(RESOLVE_ALLOC, MAX_CASE_ALLOCS);
    run_each(RESOLVE_READ, 400);
    run(RESOLVE_SIGNATURE, 0);
    run(OPEN_FILE, 0);
    run(OPEN_DIR_FILE, 0);
    run_each(OPEN_FILE, MAX_CASE_ALLOCS);
    run_each(OPEN_DIR_FILE, MAX_CASE_ALLOCS);
    run(WALK_AFTER_FILE, 0);
    run(WALK_MISSING_DIR, 0);
    run(WALK_MISSING, 0);

    printf("open-path host check: %d of %d cases failed (%d skipped inside fat16_load_fat_directory)\n", failures,
           cases, skipped);
    return failures ? 1 : 0;
}
