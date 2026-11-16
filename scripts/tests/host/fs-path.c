/*
 * Host check for scripts/tests/fs-path.sh: the extended path parser (pathparser_parse_extended in
 * kernel/fs/pparser.c) and the open policies of kernel/fs/file.c and fat/fat16.c, built with the
 * host C compiler on the image disk model (fat-image.c) with a copy of bin/os.bin. The legacy
 * calls are checked by fs-path-legacy.c; this file checks what is new.
 *
 * The rules (paths are kernel strings):
 *   - "0:/" and then 1-128 components separated by single '/', at most 259 bytes before the NUL
 *     (RORTHOX_FS_PATH_MAX = 260 with it), each component 1-255 characters; the root "0:/" alone
 *     only when the caller allows it (listing); no trailing '/';
 *   - bytes 0x20-0x7E except " * : < > ? \ | ('/' only as the separator); no component starts or
 *     ends with a space or ends with a dot, so "." and ".." are never components;
 *   - a parsed path is one allocation holding the root, the parts and the strings, freed by
 *     pathparser_free_extended; a refused path allocates nothing; the input isn't changed.
 *
 *   known answers   accepted paths give exactly their parts, in one allocation, freed by one
 *                   pathparser_free_extended (and nothing freed that wasn't allocated)
 *   refusals        empty, dot and space-edged components, doubled and trailing separators,
 *                   other drives and prefixes, the root without permission: -EINVARG, nothing
 *                   allocated
 *   characters      every byte 1-255 at the start, in the middle and at the end of the first and
 *                   the second component, and alone, against the rules above
 *   lengths         inputs that end right before an unreadable page: 255 and 256 characters in
 *                   one component, 259 and 260 bytes, 260 bytes with no NUL at all, 128
 *                   components in 258 and 259 bytes, 129 components (260 bytes)
 *   allocation      the parser's one allocation failing: -ENOMEM, nothing left; an extended
 *                   open with each allocation failing in turn: -ENOMEM or -EUNIMP, nothing left,
 *                   no descriptor slot taken
 *   fail closed     an extended open of any FAT path in any mode is -EUNIMP without one disk
 *                   read or write (even with no disk at all); a bad extended path, mode or policy
 *                   is -EINVARG; the legacy policy behaves as file_open
 *
 * Each case runs in its own process, so a crash (a read past an input) fails that case only.
 * Usage: fs-path <image>. Prints one line per failed check, one line per case
 * ("  <case>: <n> checks, <m> failed") and "fs-path host check: <failed> of <cases> cases failed".
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
#include "fat-image.h"

void *memset(void *ptr, int c, unsigned long size);
void *memcpy(void *dest, const void *src, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
unsigned int alarm(unsigned int seconds);

#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static int checks, bad;

static int same(const char *a, const char *b)
{
    return strlen(a) == strlen(b) && memcmp(a, b, strlen(a)) == 0;
}

// A copy of <bytes> bytes of <text> that ends right before an unreadable page.
static char *guarded(const char *text, int bytes)
{
    kalloc_guard_next(bytes, KALLOC_GUARD_ABOVE);
    char *p = kmalloc(bytes);
    memcpy(p, text, bytes);
    return p;
}

// <path> is accepted with exactly the parts in want[0..n-1].
static void accepted(const char *path, int allow_root, int n, const char *const *want)
{
    int len = strlen(path);
    char before[300];
    memcpy(before, path, len + 1);
    int calls = kalloc_calls(), live = kalloc_live(), bad_frees = kalloc_bad_frees();
    struct path_root *r = pathparser_parse_extended(path, allow_root);
    CHECK(!ISERR(r) && r, "%.40s... (%d bytes) refused (%d)", path, len, ISERR(r) ? ERROR_I(r) : 0);
    if (ISERR(r) || !r)
    {
        return;
    }
    CHECK(kalloc_calls() == calls + 1 && kalloc_live() == live + 1 && kalloc_is_live(r),
          "%.40s: one allocation (%d calls)", path, kalloc_calls() - calls);
    const char *start = (const char *)r;
    const char *end = start + kalloc_size(r);
    CHECK(kalloc_size(r) <= (long)(sizeof(*r) + n * sizeof(struct path_part) + len + 1),
          "%.40s: compact (%ld bytes)", path, kalloc_size(r));
    CHECK(r->drive_no == 0, "%.40s: drive %d", path, r->drive_no);
    CHECK(n || !r->first, "root: no parts");
    int i = 0;
    for (struct path_part *part = r->first; part && i <= n; part = part->next, i++)
    {
        const char *s = part->part;
        CHECK((const char *)part >= start && (const char *)(part + 1) <= end && s >= start &&
              s + strlen(s) < end, "%.40s: part %d inside the allocation", path, i);
        CHECK(i < n && same(s, want[i]), "%.40s: part %d is \"%.40s\"", path, i, s);
    }
    CHECK(i == n, "%.40s: %d parts, want %d", path, i, n);
    CHECK(memcmp(before, path, len + 1) == 0, "%.40s: input changed", path);
    pathparser_free_extended(r);
    CHECK(kalloc_live() == live && kalloc_bad_frees() == bad_frees && !kalloc_poison_errors(),
          "%.40s: freed exactly (%d live, %d bad frees)", path, kalloc_live() - live, kalloc_bad_frees() - bad_frees);
}

static void accepted1(const char *path, const char *part)
{
    accepted(path, 0, 1, &part);
}

// <path> is refused with -EINVARG before anything is allocated.
static void refused(const char *path, int allow_root, const char *why)
{
    int calls = kalloc_calls();
    struct path_root *r = pathparser_parse_extended(path, allow_root);
    CHECK(ISERR(r) && ERROR_I(r) == -EINVARG, "%s: %d, want -EINVARG", why, ERROR_I(r));
    CHECK(kalloc_calls() == calls, "%s: allocated before refusing", why);
    if (!ISERR(r) && r)
    {
        pathparser_free_extended(r);
    }
}

// ---------------------------------------------------------------------------------------------

static void c_known(void)
{
    kalloc_init();
    accepted1("0:/a", "a");
    accepted1("0:/BLANK.ELF", "BLANK.ELF");
    accepted1("0:/long name.txt", "long name.txt");
    accepted1("0:/a..b", "a..b");
    accepted1("0:/.hidden", ".hidden");
    accepted1("0:/..a", "..a");
    accepted1("0:/a. b", "a. b");
    accepted1("0:/+,;=[]", "+,;=[]");
    accepted1("0:/!#$%&'()-@^_`{}~", "!#$%&'()-@^_`{}~");
    const char *four[] = {"a b", "a..b", ".hidden", "x.tar.gz"};
    accepted("0:/a b/a..b/.hidden/x.tar.gz", 0, 4, four);
    accepted("0:/a b/a..b/.hidden/x.tar.gz", 1, 4, four);
    accepted("0:/", 1, 0, 0);
}

static void c_refusals(void)
{
    kalloc_init();
    refused(0, 0, "NULL");
    refused(0, 1, "NULL, root allowed");
    refused("", 0, "empty");
    refused("0", 0, "0");
    refused("0:", 0, "0:");
    refused("0:a", 0, "0:a");
    refused("0:/", 0, "the root, not allowed");
    refused("1:/a", 0, "drive 1");
    refused("9:/a", 0, "drive 9");
    refused("a:/a", 0, "drive a");
    refused("/a", 0, "no drive");
    refused("a", 0, "relative");
    refused("0:\\a", 0, "backslash prefix");
    refused("00:/a", 0, "two digits");
    refused("0://", 1, "empty component after the root");
    refused("0://a", 0, "leading double separator");
    refused("0:/a//b", 0, "double separator");
    refused("0:/a/", 0, "trailing separator");
    refused("0:/a/b/", 0, "trailing separator after two");
    refused("0:/.", 0, ". alone");
    refused("0:/..", 0, ".. alone");
    refused("0:/./a", 0, ". first");
    refused("0:/../a", 0, ".. first");
    refused("0:/a/.", 0, ". last");
    refused("0:/a/..", 0, ".. last");
    refused("0:/a/./b", 0, ". between");
    refused("0:/a/../b", 0, ".. between");
    refused("0:/...", 0, "three dots");
    refused("0:/a.", 0, "trailing dot");
    refused("0:/a./b", 0, "trailing dot, first component");
    refused("0:/ ", 0, "a space alone");
    refused("0:/ a", 0, "leading space");
    refused("0:/a ", 0, "trailing space");
    refused("0:/a /b", 0, "trailing space, first component");
    refused("0:/a/ b", 0, "leading space, second component");
    refused("0:/a. ", 0, "dot then trailing space");
    refused("0:/a .", 0, "space then trailing dot");
}

static int allowed(int c)
{
    if (c < 0x20 || c > 0x7E)
    {
        return 0;
    }
    for (const char *b = "\"*:<>?\\|"; *b; b++)
    {
        if (c == *b)
        {
            return 0;
        }
    }
    return 1;
}

static void put(char *s, int c)
{
    for (; *s; s++)
    {
        if (*s == 'X')
        {
            *s = c;
            return;
        }
    }
}

static void c_characters(void)
{
    for (int c = 1; c < 256; c++)
    {
        kalloc_init();
        char first[] = "0:/Xab", middle[] = "0:/aXb", last[] = "0:/abX", alone[] = "0:/X";
        char first2[] = "0:/d/Xab", last2[] = "0:/d/abX";
        put(first, c); put(middle, c); put(last, c); put(alone, c); put(first2, c); put(last2, c);
        int edge = c == '/' || c == ' ';     // never at the start of a component
        int tail = edge || c == '.';         // never at the end of one
        char want[2][8];
        // in the middle: '/' splits the name, anything else allowed stays in it
        if (c == '/')
        {
            const char *two[] = {"a", "b"};
            accepted(middle, 0, 2, two);
        }
        else if (allowed(c))
        {
            accepted1(middle, middle + 3);
        }
        else
        {
            refused(middle, 0, "byte in the middle");
        }
        if (allowed(c) && !edge)
        {
            accepted1(first, first + 3);
            memcpy(want[0], "d", 2);
            memcpy(want[1], first2 + 5, 4);
            const char *w[] = {want[0], want[1]};
            accepted(first2, 0, 2, w);
        }
        else
        {
            refused(first, 0, "byte at the start");
            refused(first2, 0, "byte at the start of the second component");
        }
        if (allowed(c) && !tail)
        {
            accepted1(last, last + 3);
            accepted1(alone, alone + 3);
            memcpy(want[0], "d", 2);
            memcpy(want[1], last2 + 5, 4);
            const char *w[] = {want[0], want[1]};
            accepted(last2, 0, 2, w);
        }
        else
        {
            refused(last, 0, "byte at the end");
            refused(alone, 0, "byte alone");
            refused(last2, 0, "byte at the end of the second component");
        }
        if (bad)
        {
            printf("    (byte 0x%02X)\n", c);
            return;
        }
    }
}

// "0:/" then <n> components of <size> 'a's (the last one <last> 'a's), NUL-terminated in buf.
static int build(char *buf, int n, int size, int last)
{
    int k = 0;
    memcpy(buf, "0:/", 3);
    k = 3;
    for (int i = 0; i < n; i++)
    {
        int m = i == n - 1 ? last : size;
        memset(buf + k, 'a', m);
        k += m;
        if (i != n - 1)
        {
            buf[k++] = '/';
        }
    }
    buf[k] = 0;
    return k;
}

// <path> copied into an input of exactly its bytes and NUL, ending at an unreadable page.
static void guarded_accepted(const char *path, int n, const char *const *want)
{
    char *g = guarded(path, strlen(path) + 1);
    accepted(g, 0, n, want);
    kfree(g);
}

static void guarded_refused(const char *path, int bytes, const char *why)
{
    char *g = guarded(path, bytes);
    refused(g, 0, why);
    kfree(g);
}

static void c_lengths(void)
{
    static char buf[600], part[300];
    const char *parts[130];
    kalloc_init();

    // One component of 255 characters (258 bytes) and of 256 (259 bytes).
    CHECK(build(buf, 1, 0, 255) == 258, "255-character path");
    memset(part, 'a', 255);
    part[255] = 0;
    parts[0] = part;
    guarded_accepted(buf, 1, parts);
    CHECK(build(buf, 1, 0, 256) == 259, "256-character path");
    guarded_refused(buf, 260, "a 256-character component (259 bytes)");
    // 255 characters after another component: too long as a path (260 bytes), refused for that.
    CHECK(build(buf, 2, 1, 255) == 260, "a/255");
    guarded_refused(buf, 261, "260 bytes with the NUL at 261");

    // 259 bytes: the longest path; 260 bytes with its NUL; 260 bytes with no NUL anywhere.
    CHECK(build(buf, 2, 128, 127) == 259, "259 bytes");
    static char p128[129], p127[128];
    memset(p128, 'a', 128);
    memset(p127, 'a', 127);
    const char *two[] = {p128, p127};
    guarded_accepted(buf, 2, two);
    CHECK(build(buf, 2, 128, 128) == 260, "260 bytes");
    guarded_refused(buf, 261, "260 bytes");
    guarded_refused(buf, 260, "260 bytes and no NUL (the parser must stop at 260)");
    memset(buf, 'a', 600);
    memcpy(buf, "0:/", 3);
    guarded_refused(buf, 600, "600 bytes and no NUL");

    // 128 one-character components (258 bytes) and 128 with the last of two (259 bytes).
    for (int i = 0; i < 129; i++)
    {
        parts[i] = "a";
    }
    CHECK(build(buf, 128, 1, 1) == 258, "128 components in 258 bytes");
    guarded_accepted(buf, 128, parts);
    CHECK(build(buf, 128, 1, 2) == 259, "128 components in 259 bytes");
    parts[127] = "aa";
    guarded_accepted(buf, 128, parts);
    // 129 components need at least 260 bytes.
    CHECK(build(buf, 129, 1, 1) == 260, "129 components");
    guarded_refused(buf, 261, "129 components");
    guarded_refused(buf, 260, "129 components, no NUL");
    CHECK(!kalloc_live() && !kalloc_bad_frees(), "inputs freed");
}

static void c_allocation(void)
{
    static char buf[300];
    const char *paths[] = {"0:/a", "0:/a b/c", "0:/", 0};
    build(buf, 128, 1, 2);
    paths[3] = buf;
    for (int i = 0; i < 4; i++)
    {
        kalloc_init();
        kalloc_fail_at(1);
        struct path_root *r = pathparser_parse_extended(paths[i], i == 2);
        CHECK(ISERR(r) && ERROR_I(r) == -ENOMEM && kalloc_failed() && !kalloc_live(),
              "%.20s with its allocation failing: %d, want -ENOMEM", paths[i], ERROR_I(r));
    }

    // An extended open, each allocation failing in turn: refused, nothing left, no slot taken.
    int n;
    for (n = 1; n < 16; n++)
    {
        CHECK(fimg_mount() == 0, "mount");
        int live = kalloc_live();
        fimg_fail_alloc(n);
        int res = file_open_policy("0:/blank.elf", FILE_MODE_READ, FILE_OPEN_EXTENDED);
        int failed = kalloc_failed();
        fimg_fail_alloc(0);
        CHECK(res == (failed ? -ENOMEM : -EUNIMP), "allocation %d failing: %d", n, res);
        CHECK(kalloc_live() == live && !kalloc_bad_frees() && !kalloc_poison_errors(),
              "allocation %d failing: %d left", n, kalloc_live() - live);
        int fd = file_open("0:/blank.elf", FILE_MODE_READ);
        CHECK(fd == 1, "after it, the first handle is free (%d)", fd);
        if (fd > 0)
        {
            fclose(fd);
        }
        if (!failed)
        {
            break;
        }
    }
    CHECK(n == 2, "the extended open allocates once before it is refused (%d)", n);
}

static void c_fail_closed(void)
{
    CHECK(fimg_mount() == 0, "mount");
    struct disk *d = disk_get(0);
    int live = kalloc_live();
    fimg_reset_counts();
    const char *paths[] = {"0:/blank.elf", "0:/BLANK.ELF", "0:/shell.elf", "0:/long name.txt", "0:/NEW.TXT",
                           "0:/new file.txt", "0:/dir/new file.txt", "0:/blank.elf/x"};
    for (unsigned int i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        for (int mode = FILE_MODE_READ; mode <= FILE_MODE_APPEND; mode++)
        {
            int res = file_open_policy(paths[i], mode, FILE_OPEN_EXTENDED);
            CHECK(res == -EUNIMP, "extended open of %s, mode %d: %d, want -EUNIMP", paths[i], mode, res);
        }
    }
    struct path_part part = {"blank.elf", 0};
    for (int mode = FILE_MODE_READ; mode <= FILE_MODE_APPEND; mode++)
    {
        void *p = d->filesystem->open_policy(0, &part, mode, FILE_OPEN_EXTENDED);
        CHECK(ERROR_I(p) == -EUNIMP, "FAT open_policy with no disk, mode %d: %d", mode, ERROR_I(p));
    }
    CHECK(ERROR_I(d->filesystem->open_policy(d, &part, FILE_MODE_READ, 2)) == -EINVARG, "FAT open_policy, policy 2");
    CHECK(ERROR_I(d->filesystem->open_policy(0, &part, FILE_MODE_READ, -1)) == -EINVARG, "FAT open_policy, policy -1");

    // Refused before the filesystem: bad paths, modes and policies.
    static char long_path[300];
    CHECK(build(long_path, 1, 0, 257) == 260, "260 bytes");
    char *g = guarded(long_path, 260);
    const char *bad_paths[] = {0, "", "0:/", "0:/a/", "0:/a//b", "0:/.", "0:/a/..", "0:/a ", "0:/a.", "1:/a", "0:/a|b", g};
    for (unsigned int i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); i++)
    {
        for (int mode = FILE_MODE_READ; mode <= FILE_MODE_APPEND; mode++)
        {
            int res = file_open_policy(bad_paths[i], mode, FILE_OPEN_EXTENDED);
            CHECK(res == -EINVARG, "extended open of bad path %d, mode %d: %d", i, mode, res);
        }
    }
    kfree(g);
    CHECK(file_open_policy("0:/blank.elf", 3, FILE_OPEN_EXTENDED) == -EINVARG, "mode 3");
    CHECK(file_open_policy("0:/blank.elf", FILE_MODE_READ, 2) == -EINVARG, "policy 2");
    CHECK(file_open_policy("0:/blank.elf", FILE_MODE_WRITE, -1) == -EINVARG, "policy -1");
    CHECK(fimg_counts.reads == 0 && fimg_counts.writes == 0 && fimg_counts.flushes == 0,
          "nothing reached the disk (%d reads, %d writes, %d flushes)", fimg_counts.reads, fimg_counts.writes,
          fimg_counts.flushes);
    CHECK(kalloc_live() == live && !kalloc_bad_frees() && !kalloc_poison_errors(),
          "nothing left (%d live, %d bad frees)", kalloc_live() - live, kalloc_bad_frees());

    // The legacy policy is file_open: the strict names, then the file.
    CHECK(file_open_policy("0:/long name.txt", FILE_MODE_READ, FILE_OPEN_LEGACY) == -EINVARG, "legacy, long name");
    CHECK(file_open_policy("0:/blank.elf", FILE_MODE_WRITE, FILE_OPEN_LEGACY) == -ERDONLY, "legacy, read-only");
    int fd = file_open_policy("0:/blank.elf", FILE_MODE_READ, FILE_OPEN_LEGACY);
    CHECK(fd == 1, "legacy open of blank.elf: %d", fd);
    if (fd > 0)
    {
        fclose(fd);
    }
    CHECK(kalloc_live() == live && !kalloc_bad_frees(), "legacy opens give everything back");
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "known answers", c_known },
    { "refusals", c_refusals },
    { "characters", c_characters },
    { "lengths", c_lengths },
    { "allocation", c_allocation },
    { "fail closed", c_fail_closed },
};

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: fs-path <image>\n");
        return 2;
    }
    fimg_open(argv[1]);
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
    printf("fs-path host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
