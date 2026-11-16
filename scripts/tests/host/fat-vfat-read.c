/*
 * Host check for scripts/tests/fat-vfat-read.sh: opening files by VFAT long name or by short
 * alias through the extended READ path (file_open_policy with FILE_OPEN_EXTENDED), and the old
 * entry points on the same images. The real file.c, pparser.c, fat16.c and streamer.c run on the
 * image disk model (fat-image.c). Only public file calls are used, so the check builds on a
 * kernel without long-name support too (there, every extended open is refused).
 *
 * The cases come from <dir>/cases.txt (scripts/tests/lib/vfat-fixtures.py writes it), one per
 * line, tab-separated: <kind> <image> <path> <expected>. Kinds:
 *   ext       file_open_policy(path, FILE_MODE_READ, FILE_OPEN_EXTENDED)
 *   fopen     fopen_status(path, "r"), the old entry point (syscall 6/7 program lookup)
 *   open      file_open(path, FILE_MODE_READ), the old entry point of syscall 13
 *   prefixes  every proper prefix of the path's last component, through ext: none may open
 *   oom       ext, with the n-th allocation of the open failing, for every n until it succeeds
 *   readfail  ext open and read, with the n-th disk read failing, for every n until none fails
 * Expected: "=<file>" (opens, and reading to the end gives exactly that file's bytes), "-ENOENT",
 * "-EIO", "-EINVARG" (that status), "error" (any negative status) or "none" (prefixes).
 * Every case also checks that no allocation is left behind once the file is closed, and that
 * nothing was freed twice or written after being freed.
 *
 * Usage: fat-vfat-read <dir>. Prints one line per failed case and per kind a count, then
 * "fat-vfat-read host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "string/string.h"
#include "fat-image.h"

// kernel/string/string.c has its own snprintf, so format with the C library's.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define host_snprintf(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)
unsigned int alarm(unsigned int seconds);
void *malloc(unsigned long size);

#define MAX_CASES 1024
#define MAX_TEXT (256 * 1024)
#define MAX_FILE (64 * 1024)

struct vcase
{
    const char *kind, *image, *path, *expect;
};

static char text[MAX_TEXT];
static struct vcase cases[MAX_CASES];
static int case_count;
static const char *dir;
static unsigned char got[MAX_FILE + 1];

#define FAIL(...) do { printf("  FAIL %s %s \"%s\": ", c->kind, c->image, c->path); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static long read_host_file(const char *path, unsigned char *buf, long max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    long n = 0, r;
    while (n < max && (r = read(fd, buf + n, max - n)) > 0)
    {
        n += r;
    }
    close(fd);
    return n;
}

static int status_of(const char *name)
{
    static const struct { const char *name; int status; } names[] = {
        {"-ENOENT", -ENOENT}, {"-EIO", -EIO}, {"-EINVARG", -EINVARG}, {"-ENOMEM", -ENOMEM},
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcmp((char *)name, (char *)names[i].name) == 0)
        {
            return names[i].status;
        }
    }
    return 1;
}

static int open_kind(const char *kind, const char *path)
{
    if (strcmp((char *)kind, "fopen") == 0)
    {
        return fopen_status(path, "r");
    }
    if (strcmp((char *)kind, "open") == 0)
    {
        return file_open(path, FILE_MODE_READ);
    }
    return file_open_policy(path, FILE_MODE_READ, FILE_OPEN_EXTENDED);
}

// Read an open file to its end (several calls of odd sizes). Bytes read, or the error.
static long read_all(int fd)
{
    long n = 0;
    for (int step = 1;; step = step * 3 % 1000 + 1)
    {
        if (n + step > MAX_FILE)
        {
            return -1000;
        }
        int r = file_read(fd, got + n, step);
        if (r < 0)
        {
            return r;
        }
        if (r == 0)
        {
            return n;
        }
        n += r;
    }
}

// 0 if the <n> bytes in got[] are exactly the bytes of the host file <want>.
static int compare_got(const struct vcase *c, long n, const char *want)
{
    static unsigned char expected[MAX_FILE];
    long size = read_host_file(want, expected, MAX_FILE);
    if (size < 0)
    {
        FAIL("cannot read %s", want);
    }
    if (n != size || memcmp(got, expected, size) != 0)
    {
        FAIL("read %ld bytes, %s has %ld%s", n, want, size, n == size ? " (contents differ)" : "");
    }
    return 0;
}

// 0 if <fd> reads exactly the bytes of the host file <want>.
static int compare(const struct vcase *c, int fd, const char *want)
{
    return compare_got(c, read_all(fd), want);
}

static int mount_case(const struct vcase *c)
{
    if (fimg_mount() != 0)
    {
        FAIL("the image does not mount");
    }
    return 0;
}

static int leaks(const struct vcase *c, int live, const char *when)
{
    if (kalloc_live() != live)
    {
        FAIL("%d allocation(s) left %s", kalloc_live() - live, when);
    }
    return 0;
}

static int run_open(const struct vcase *c)
{
    if (mount_case(c))
    {
        return 1;
    }
    int live = kalloc_live();
    int fd = open_kind(c->kind, c->path);
    if (c->expect[0] == '=')
    {
        if (fd <= 0)
        {
            FAIL("open gave %d, expected the file", fd);
        }
        if (compare(c, fd, c->expect + 1))
        {
            return 1;
        }
        if (fclose(fd) != 0)
        {
            FAIL("fclose failed");
        }
        return leaks(c, live, "after fclose");
    }
    if (fd > 0)
    {
        fclose(fd);
        FAIL("opened (fd %d), expected %s", fd, c->expect);
    }
    if (strcmp((char *)c->expect, "error") == 0 ? fd >= 0 : fd != status_of(c->expect))
    {
        FAIL("open gave %d, expected %s", fd, c->expect);
    }
    return leaks(c, live, "after a refused open");
}

static int run_prefixes(const struct vcase *c)
{
    char path[512];
    int len = strlen(c->path), last = len;
    while (last > 0 && c->path[last - 1] != '/')
    {
        last--;
    }
    if (mount_case(c))
    {
        return 1;
    }
    int live = kalloc_live();
    for (int n = last + 1; n < len; n++)
    {
        memcpy(path, c->path, n);
        path[n] = 0;
        int fd = file_open_policy(path, FILE_MODE_READ, FILE_OPEN_EXTENDED);
        if (fd > 0)
        {
            fclose(fd);
            FAIL("the %d-character prefix \"%s\" opens", n - last, path + last);
        }
    }
    return leaks(c, live, "after the prefixes");
}

static int run_oom(const struct vcase *c)
{
    for (int n = 1; n <= 64; n++)
    {
        if (mount_case(c))
        {
            return 1;
        }
        int live = kalloc_live();
        kalloc_fail_at(kalloc_calls() + n);
        int fd = file_open_policy(c->path, FILE_MODE_READ, FILE_OPEN_EXTENDED);
        int failed = kalloc_failed();
        kalloc_fail_at(0);
        if (!failed)
        {
            if (fd <= 0)
            {
                FAIL("open gave %d with allocation %d not reached", fd, n);
            }
            if (compare(c, fd, c->expect + 1) || fclose(fd) != 0 || leaks(c, live, "after fclose"))
            {
                return 1;
            }
            if (n < 2)
            {
                FAIL("the open made no allocation");
            }
            return 0;
        }
        if (fd != -ENOMEM)
        {
            FAIL("allocation %d failed and open gave %d, expected -ENOMEM", n, fd);
        }
        if (leaks(c, live, "after a failed allocation"))
        {
            return 1;
        }
    }
    FAIL("still failing after 64 allocations");
}

static int run_readfail(const struct vcase *c)
{
    for (int n = 1; n <= 512; n++)
    {
        if (mount_case(c))
        {
            return 1;
        }
        int live = kalloc_live();
        fimg_reset_counts();
        fimg_fail_read(n, -EIO);
        int fd = file_open_policy(c->path, FILE_MODE_READ, FILE_OPEN_EXTENDED);
        long r = fd > 0 ? read_all(fd) : fd;
        int failed = fimg_counts.failed;
        fimg_fail_read(0, 0);
        if (!failed)
        {
            if (fd <= 0)
            {
                FAIL("open gave %d with no failed read", fd);
            }
            if (compare_got(c, r, c->expect + 1) || fclose(fd) != 0 || leaks(c, live, "after fclose"))
            {
                return 1;
            }
            if (n < 2)
            {
                FAIL("the open and the read made no disk read");
            }
            return 0;
        }
        if (fd > 0 && r >= 0)
        {
            FAIL("read %d failed but the open and the read succeeded (%ld bytes)", n, r);
        }
        if (fd > 0)
        {
            fclose(fd);
        }
        else if (fd >= 0)
        {
            FAIL("read %d failed and open gave %d", n, fd);
        }
        if (leaks(c, live, "after a failed read"))
        {
            return 1;
        }
    }
    FAIL("still failing after 512 reads");
}

static int run_case(const struct vcase *c)
{
    if (strcmp((char *)c->kind, "prefixes") == 0)
    {
        return run_prefixes(c);
    }
    if (strcmp((char *)c->kind, "oom") == 0)
    {
        return run_oom(c);
    }
    if (strcmp((char *)c->kind, "readfail") == 0)
    {
        return run_readfail(c);
    }
    return run_open(c);
}

static void load_cases(void)
{
    char path[512];
    host_snprintf(path, sizeof(path), "%s/cases.txt", dir);
    long n = read_host_file(path, (unsigned char *)text, MAX_TEXT - 1);
    if (n <= 0 || n >= MAX_TEXT - 1)
    {
        printf("fat-vfat-read: cannot read %s\n", path);
        exit(2);
    }
    text[n] = 0;
    for (char *p = text; *p && case_count < MAX_CASES;)
    {
        const char *field[4];
        for (int i = 0; i < 4; i++)
        {
            field[i] = p;
            while (*p && *p != '\t' && *p != '\n')
            {
                p++;
            }
            if ((i < 3 && *p != '\t') || (i == 3 && *p != '\n'))
            {
                printf("fat-vfat-read: bad line %d in %s\n", case_count + 1, path);
                exit(2);
            }
            *p++ = 0;
        }
        cases[case_count++] = (struct vcase){field[0], field[1], field[2], field[3]};
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: fat-vfat-read <dir>\n");
        return 2;
    }
    dir = argv[1];
    load_cases();
    static const char *kinds[] = {"ext", "fopen", "open", "prefixes", "oom", "readfail"};
    int total[6] = {0}, bad[6] = {0}, failed = 0;
    const char *mounted = "";
    for (int i = 0; i < case_count; i++)
    {
        const struct vcase *c = &cases[i];
        int k = 0;
        while (k < 6 && strcmp((char *)c->kind, (char *)kinds[k]) != 0)
        {
            k++;
        }
        if (k == 6)
        {
            printf("fat-vfat-read: unknown kind %s\n", c->kind);
            return 2;
        }
        if (strcmp((char *)mounted, (char *)c->image) != 0)
        {
            char path[512];
            host_snprintf(path, sizeof(path), "%s/%s", dir, c->image);
            fimg_open(path);
            mounted = c->image;
        }
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
            int r = run_case(c);
            fflush(0);
            _exit(r || kalloc_bad_frees() || kalloc_poison_errors() ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        total[k]++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            if (!WIFEXITED(status))
            {
                printf("  FAIL %s %s \"%s\": killed by signal %d\n", c->kind, c->image, c->path, WTERMSIG(status));
            }
            bad[k]++;
            failed++;
        }
    }
    for (int k = 0; k < 6; k++)
    {
        printf("  %s: %d cases, %d failed\n", kinds[k], total[k], bad[k]);
    }
    printf("fat-vfat-read host check: %d of %d cases failed\n", failed, case_count);
    fflush(0);
    return failed ? 1 : 0;
}
