/*
 * Host check for scripts/tests/fat-directory.sh: opening files in subdirectories through the real
 * kernel/fs/file.c, pparser.c, fat/fat16.c and disk/streamer.c, built with the host C compiler,
 * on a copy of the RorthOX image (fat-host.c is the disk, kalloc.c the heap).
 *
 * The image has dir/sub/blank.elf, a normal empty directory "hollow" (only . and ..), a directory
 * "void" whose cluster is all zeros (no entries at all, not even . and ..) and a directory "bad"
 * whose first cluster lies past the end of the image file.
 *
 *   nested   0:/dir/sub/blank.elf opens, reads back whole and fclose() gives everything back.
 *   alloc    the same open with each allocation failing in turn; failures inside
 *            fat16_load_fat_directory() are counted.
 *   read     the same open with each disk read failing in turn; failures inside the clusters of
 *            dir and dir/sub are counted.
 *   void     0:/void opens as a directory and closes cleanly; 0:/void/x is not found.
 *   hollow   0:/hollow opens and closes cleanly; 0:/hollow/x is not found.
 *   bad      0:/bad/blank.elf is not found, after the kernel asked for the bad cluster.
 *
 * Every failed open must return 0, take no descriptor slot, leave no allocation behind, free
 * nothing twice and write nothing to freed memory. After a failed disk read there must be no
 * further allocation call (a failed count must not size an entry buffer).
 *
 * Usage: fat-directory <image> <blank.elf>
 * Prints "fat-directory host check: <failed> of <cases> cases failed", and the counts as
 * "fat-directory: <n> allocation failures inside fat16_load_fat_directory" and
 * "fat-directory: <n> read failures in dir, <m> in dir/sub".
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

#define NESTED "0:/dir/sub/blank.elf"
#define MAX_TRIES 200

static unsigned char *blank;
static long blank_size;
static long dir_cluster, sub_cluster, bad_cluster;
static int quiet;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

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

static int heap_clean(const char *what, int live_before)
{
    if (kalloc_bad_frees())
    {
        say("  FAIL %s: kfree of something that was not allocated (double free)\n", what);
        return 0;
    }
    if (kalloc_poison_errors())
    {
        say("  FAIL %s: freed memory was written to\n", what);
        return 0;
    }
    if (kalloc_live() != live_before)
    {
        say("  FAIL %s: %d allocations left behind\n", what, kalloc_live() - live_before);
        return 0;
    }
    return 1;
}

// A failed open: 0, no descriptor, nothing left.
static int failed_open(const char *what, int fd, int live_before)
{
    if (fd != 0)
    {
        say("  FAIL %s: fopen returned %d, expected 0\n", what, fd);
        return 0;
    }
    if (!no_descriptors())
    {
        say("  FAIL %s: a descriptor slot was filled\n", what);
        return 0;
    }
    return heap_clean(what, live_before);
}

// The first cluster of the entry <name83> in directory cluster <cluster>, -1 if there is none.
static long entry_in_cluster(const char *image, long cluster, const char *name83)
{
    unsigned char e[32];
    int fd = open(image, O_RDONLY);
    long found = -1;
    for (long i = 0; fd >= 0 && i < geo.cluster_bytes / 32; i++)
    {
        if (pread(fd, e, 32, cluster_pos(cluster) + 32 * i) != 32 || e[0] == 0)
        {
            break;
        }
        if (e[0] != 0xE5 && memcmp(e, (void *)name83, 11) == 0)
        {
            found = e[26] | (e[27] << 8);
            break;
        }
    }
    close(fd);
    return found;
}

static int nested_case(void)
{
    fat_host_setup();
    int live = kalloc_live();
    int fd = fopen(NESTED, "r");
    struct file_stat stat;
    if (fd <= 0 || fstat(fd, &stat) != 0 || (long)stat.filesize != blank_size)
    {
        say("  FAIL open %s: did not open with the size of blank.elf (fd %d)\n", NESTED, fd);
        return 1;
    }
    unsigned char *data = malloc(blank_size);
    int res = fread(data, blank_size, 1, fd);
    if (res != 1 || memcmp(data, blank, blank_size) != 0)
    {
        say("  FAIL open %s: fread returned %d or the bytes differ from blank.elf\n", NESTED, res);
        return 1;
    }
    if (fclose(fd) != 0 || !no_descriptors())
    {
        say("  FAIL open %s: fclose failed\n", NESTED);
        return 1;
    }
    return heap_clean("open " NESTED, live) ? 0 : 1;
}

// Exit codes of the failure cases: 0 pass, 1 fail, 2 pass without reaching the failure,
// 3 pass with the failure inside a counted place (see main), 4 the same for the second place.
static int alloc_case(int k)
{
    char what[96];
    fat_host_setup();
    int live = kalloc_live();
    int base = kalloc_calls();
    kalloc_fail_at(base + k);
    int fd = fopen(NESTED, "r");
    if (!kalloc_failed())
    {
        kalloc_fail_at(0);
        if (fd <= 0 || fclose(fd) != 0)
        {
            say("  FAIL open %s: no allocation failed, yet it did not open\n", NESTED);
            return 1;
        }
        return heap_clean("open " NESTED, live) ? 2 : 1;
    }
    const char *caller = kalloc_caller(base + k);
    strcpy(what, "allocation ");
    strcpy(what + strlen(what), itoa(k));
    strcpy(what + strlen(what), " (");
    strncpy(what + strlen(what), caller, 40);
    strcpy(what + strlen(what), ") failing");
    if (!failed_open(what, fd, live))
    {
        return 1;
    }
    return strncmp(caller, "fat16_load_fat_directory", 30) == 0 ? 3 : 0;
}

static int read_case(int k)
{
    char what[64];
    fat_host_setup();
    int live = kalloc_live();
    fat_host_fail_read(k, -EIO);
    int fd = fopen(NESTED, "r");
    if (!failed_reads)
    {
        fat_host_fail_read(0, 0);
        if (fd <= 0 || fclose(fd) != 0)
        {
            say("  FAIL open %s: no read failed, yet it did not open\n", NESTED);
            return 1;
        }
        return heap_clean("open " NESTED, live) ? 2 : 1;
    }
    long lba = read_log[failed_read_index];
    strcpy(what, "disk read ");
    strcpy(what + strlen(what), itoa(k));
    strcpy(what + strlen(what), " (sector ");
    strcpy(what + strlen(what), itoa((int)lba));
    strcpy(what + strlen(what), ") failing");
    if (!failed_open(what, fd, live))
    {
        return 1;
    }
    if (kalloc_calls() != failed_alloc_calls)
    {
        say("  FAIL %s: %d allocation call(s) after the failed read\n", what, kalloc_calls() - failed_alloc_calls);
        return 1;
    }
    long cluster = cluster_of_pos(lba * geo.sector_size);
    return cluster == dir_cluster ? 3 : cluster == sub_cluster ? 4 : 0;
}

static int empty_case(const char *dir, const char *inside)
{
    fat_host_setup();
    int live = kalloc_live();
    int fd = fopen(dir, "r");
    struct file_stat stat;
    if (fd <= 0)
    {
        say("  FAIL open %s: fopen returned %d, expected a descriptor for the directory\n", dir, fd);
        return 1;
    }
    if (fstat(fd, &stat) >= 0)
    {
        say("  FAIL open %s: fstat of a directory succeeded\n", dir);
        return 1;
    }
    if (fclose(fd) != 0 || !no_descriptors())
    {
        say("  FAIL open %s: fclose failed\n", dir);
        return 1;
    }
    if (!heap_clean(dir, live))
    {
        return 1;
    }
    fd = fopen(inside, "r");
    return failed_open(inside, fd, live) ? 0 : 1;
}

static int bad_case(void)
{
    const char *path = "0:/bad/blank.elf";
    fat_host_setup();
    int live = kalloc_live();
    int fd = fopen(path, "r");
    int asked = 0;
    for (int i = 0; i < read_count; i++)
    {
        asked |= cluster_of_pos(read_log[i] * geo.sector_size) == bad_cluster;
    }
    if (!asked || failed_alloc_calls < 0)
    {
        say("  FAIL open %s: the kernel never read the bad directory cluster %ld\n", path, bad_cluster);
        return 1;
    }
    if (!failed_open(path, fd, live))
    {
        return 1;
    }
    if (kalloc_calls() != failed_alloc_calls)
    {
        say("  FAIL open %s: %d allocation call(s) after the failed read\n", path, kalloc_calls() - failed_alloc_calls);
        return 1;
    }
    return 0;
}

enum
{
    NESTED_OPEN,
    ALLOC,
    READ,
    VOID,
    HOLLOW,
    BAD,
    KINDS
};

static const char *kind_name[] = { "open " NESTED, "allocation failing", "disk read failing", "0:/void",
                                   "0:/hollow", "0:/bad/blank.elf" };
static int cases, failures, kind_failures[KINDS];

static int run(int kind, int k)
{
    quiet = kind_failures[kind] >= 3;
    fflush(0);
    pid_t pid = fork();
    if (pid == 0)
    {
        int rc = 1;
        switch (kind)
        {
        case NESTED_OPEN: rc = nested_case(); break;
        case ALLOC: rc = alloc_case(k); break;
        case READ: rc = read_case(k); break;
        case VOID: rc = empty_case("0:/void", "0:/void/x"); break;
        case HOLLOW: rc = empty_case("0:/hollow", "0:/hollow/x"); break;
        case BAD: rc = bad_case(); break;
        }
        fflush(0);
        _exit(rc);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    cases++;
    if (rc == 1 || rc < 0)
    {
        if (WIFSIGNALED(status) && !quiet)
        {
            printf("  FAIL %s %d: crashed (signal %d)\n", kind_name[kind], k, WTERMSIG(status));
        }
        failures++;
        kind_failures[kind]++;
        return 1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: fat-directory <image> <blank.elf>\n");
        return 2;
    }
    fat_host_open_image(argv[1]);
    blank = fat_host_read_file(argv[2], &blank_size);
    long size;
    if (fat_host_root_entry("DIR        ", &dir_cluster, &size) != 0 ||
        fat_host_root_entry("BAD        ", &bad_cluster, &size) != 0 ||
        (sub_cluster = entry_in_cluster(argv[1], dir_cluster, "SUB        ")) < 2)
    {
        printf("fat-directory: the image has no dir, dir/sub or bad\n");
        return 2;
    }

    int in_loader = 0, in_dir = 0, in_sub = 0, done;
    run(NESTED_OPEN, 0);
    done = 0;
    for (int k = 1; k <= MAX_TRIES && !done; k++)
    {
        int rc = run(ALLOC, k);
        in_loader += rc == 3;
        done = rc == 2;
    }
    if (!done)
    {
        printf("  FAIL allocation failing: still failing after %d calls\n", MAX_TRIES);
        failures++;
    }
    done = 0;
    for (int k = 1; k <= MAX_TRIES && !done; k++)
    {
        int rc = run(READ, k);
        in_dir += rc == 3;
        in_sub += rc == 4;
        done = rc == 2;
    }
    if (!done)
    {
        printf("  FAIL disk read failing: still failing after %d reads\n", MAX_TRIES);
        failures++;
    }
    run(VOID, 0);
    run(HOLLOW, 0);
    run(BAD, 0);

    printf("fat-directory: %d allocation failures inside fat16_load_fat_directory\n", in_loader);
    printf("fat-directory: %d read failures in dir, %d in dir/sub\n", in_dir, in_sub);
    printf("fat-directory host check: %d of %d cases failed\n", failures, cases);
    fflush(0);
    return failures ? 1 : 0;
}
