/*
 * Legacy path matrix for scripts/tests/fs-path.sh: what the existing path calls accept and refuse,
 * printed one line per path, so the script can compare it byte for byte with the matrix recorded
 * from the code before the extended paths came in (scripts/tests/host/fs-path-legacy.txt). Runs
 * the real kernel/fs/file.c, pparser.c, fat/fat16.c and disk/streamer.c on the image disk model
 * (fat-image.c) with a copy of bin/os.bin. It uses only calls that existed before, so it builds
 * on the older code too.
 *
 * Each path runs in its own process on a fresh mount (an open that creates a file doesn't change
 * the next line, and a crash prints "crashed" for that path only). A line is
 *
 *   <path> v=<file_validate_path> p=<pathparser_parse> fr= fw= fa= fx=<fopen_status r/w/a/x>
 *          o0= o1= o2= o3=<file_open with mode 0-3> [leak]
 *
 * where the path shows bytes outside 0x21-0x7E and the backslash as \xNN ("(empty)" for ""), p= is "-" for NULL
 * or the drive and the parts ("0[a][b]"), an open that worked prints its handle (closed right
 * after), and "leak" means something stayed allocated or was freed twice.
 *
 * Usage: fs-path-legacy <image>
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "fs/pparser.h"
#include "fat-image.h"

void *memset(void *ptr, int c, unsigned long size);
void *memcpy(void *dest, const void *src, unsigned long n);
unsigned int alarm(unsigned int seconds);

static void show_path(const char *p)
{
    if (!*p)
    {
        printf("(empty)");
    }
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
    {
        if (*c < 0x21 || *c > 0x7E || *c == '\\')
        {
            printf("\\x%02X", *c);
        }
        else
        {
            printf("%c", *c);
        }
    }
}

static void show_fd(const char *name, int fd)
{
    printf(" %s=%d", name, fd);
    if (fd > 0)
    {
        fclose(fd);
    }
}

static void row(const char *p)
{
    fimg_mount();
    int live = kalloc_live();
    show_path(p);
    printf(" v=%d p=", file_validate_path(p));
    struct path_root *root = pathparser_parse(p, 0);
    if (!root)
    {
        printf("-");
    }
    else
    {
        printf("%d", root->drive_no);
        for (struct path_part *part = root->first; part; part = part->next)
        {
            printf("[");
            show_path(part->part);
            printf("]");
        }
        pathparser_free(root);
    }
    const char *modes[] = {"r", "w", "a", "x"};
    char name[3] = "f?";
    for (int i = 0; i < 4; i++)
    {
        name[1] = modes[i][0];
        show_fd(name, fopen_status(p, modes[i]));
    }
    name[0] = 'o';
    for (int mode = 0; mode < 4; mode++)
    {
        name[1] = '0' + mode;
        show_fd(name, file_open(p, mode));
    }
    if (kalloc_live() != live || kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf(" leak");
    }
    printf("\n");
}

static void run(const char *p)
{
    fflush(0);
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(20);
        row(p);
        fflush(0);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        printf("\n");
        show_path(p);
        printf(" crashed\n");
    }
}

static const char *fixed[] = {
    "", "0", "0:", "0:/", "1:/", "0:a", "/blank.elf", "blank.elf", "a:/blank.elf", "00:/blank.elf",
    "0:\\blank.elf", "1:/blank.elf", "9:/blank.elf",
    "0:/blank.elf", "0:/BLANK.ELF", "0:/Blank.Elf", "0:/shell.elf", "0:/blank", "0:/blank.", "0:/blank.elf.",
    "0:/blank.elf/", "0:/blank.elf//", "0:/blank.elf/x", "0:/blank.elf//x", "0://blank.elf", "0:/a//b", "0:/a/",
    "0:/.", "0:/..", "0:/./blank.elf", "0:/../blank.elf", "0:/a/./b", "0:/a/../b", "0:/.a", "0:/..a",
    "0:/a.", "0:/a..b", "0:/a.b.c", "0:/a.b/c.d", "0:/ a", "0:/a ", "0:/a b", "0:/a /b", "0:/a/ b",
    "0:/long name.txt", "0:/longname.txt", "0:/12345678", "0:/123456789", "0:/12345678.123", "0:/12345678.1234",
    "0:/123456789.123", "0:/a.123", "0:/a.1234", "0:/x/y", "0:/dir/blank.elf", "0:/NEW.TXT", "0:/new.txt",
    "0:/NEW", "0:/A/B/C/D/E/F", "0:/~1", "0:/A~1.TXT", "0:/!#$%&'().-@^_`{}~", "0:/a+b", "0:/a,b", "0:/a;b",
    "0:/a=b", "0:/a[b]", "0:/a\"b", "0:/a*b", "0:/a:b", "0:/a<b", "0:/a>b", "0:/a?b", "0:/a\\b", "0:/a|b",
};

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: fs-path-legacy <image>\n");
        return 2;
    }
    fimg_open(argv[1]);
    for (unsigned int i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
    {
        run(fixed[i]);
    }
    // Every byte at the start, in the middle and at the end of a name, and in an extension.
    for (int c = 1; c < 256; c++)
    {
        char p[][10] = {"0:/Xab", "0:/aXb", "0:/abX", "0:/a.bX"};
        for (int i = 0; i < 4; i++)
        {
            for (char *x = p[i]; *x; x++)
            {
                if (*x == 'X')
                {
                    *x = c;
                }
            }
            run(p[i]);
        }
    }
    // Around RORTHOX_MAX_PATH (108 with the NUL): one long name, and valid 8.3 parts.
    for (int len = 104; len <= 112; len++)
    {
        char p[120];
        memset(p, 'a', sizeof(p));
        memcpy(p, "0:/", 3);
        p[len] = 0;
        run(p);
        memcpy(p, "0:/", 3);
        for (int i = 3; i < len; i++)
        {
            p[i] = (i - 3) % 8 == 7 ? '/' : 'A';
        }
        if (p[len - 1] == '/')
        {
            p[len - 1] = 'B';
        }
        p[len] = 0;
        run(p);
    }
    return 0;
}
