/*
 * Host check for scripts/tests/fat-image-model.sh: the writable image disk of fat-image.c on its
 * own, with sector values worked out by hand, and then under the real kernel/fs code.
 *
 * Three sectors at the end of the image (A, B, C, free space in the data area) get known
 * contents: each case checks the live and durable layers, the counters, the ledger and the crash
 * cuts against what the sequence of writes and flushes must give, sector by sector, and that
 * every other sector still holds the image file's bytes. Some cuts are also exported and read
 * back from the file. Failures: the n-th read, write, flush and allocation; capacity limits.
 * One case writes a sector more times than the image has sectors before a flush; one checks that
 * views don't change after later writes and flushes.
 * The kernel case creates a file through file.c and fat16.c, exports the image before and after
 * (and one with the second FAT changed) for the script's mtools and fsck.fat checks, and mounts
 * the crash cut at the end again with fimg_load_view().
 *
 * Usage: fat-image-model <image> <outdir>. Prints one line per failed check and
 * "fat-image host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "disk/disk.h"
#include "fat-image.h"

void *malloc(unsigned long size);
void *memset(void *ptr, int c, unsigned long size);
void *memcpy(void *dest, const void *src, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define fmt(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)
long lseek(int fd, long offset, int whence);
long write(int fd, const void *buf, unsigned long count);
unsigned int alarm(unsigned int seconds);

#define S FIMG_SECTOR
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static const char *out_dir;
static unsigned char *orig;      // the image file as opened
static long A, B, C;             // three sectors near the end of the image
static int checks, bad;
static struct disk *d;

// One sector filled with <tag>.
static unsigned char *sec(int tag)
{
    static unsigned char b[8][S];
    static int next;
    unsigned char *p = b[next++ % 8];
    memset(p, tag, S);
    return p;
}

static int wr(long lba, int tags, int a, int b) // <tags> sectors (1 or 2), filled with a then b
{
    unsigned char two[2 * S];
    memset(two, a, S);
    memset(two + S, b, S);
    return disk_write_block(d, lba, tags, two);
}

// Sector <lba> of a view holds the byte <tag> everywhere; tag < 0: the image file's bytes.
static int is(const unsigned char **v, long lba, int tag)
{
    return memcmp(v[lba], tag < 0 ? orig + lba * S : sec(tag), S) == 0;
}

// The whole view: A, B and C as given, every other sector as in the image file.
static int view_is(const unsigned char **v, int a, int b, int c)
{
    for (long s = 0; s < fimg_sectors(); s++)
    {
        if (s != A && s != B && s != C && !is(v, s, -1))
        {
            printf("    sector %ld changed\n", s);
            return 0;
        }
    }
    return is(v, A, a) && is(v, B, b) && is(v, C, c);
}

// Export the view, read the file back, compare with the image file changed by hand.
static int export_is(const unsigned char **v, const char *name, int a, int b, int c)
{
    char path[512];
    fmt(path, sizeof(path), "%s/%s", out_dir, name);
    long n = fimg_file_bytes();
    unsigned char *want = malloc(fimg_sectors() * S), *got = malloc(n);
    memcpy(want, orig, n);
    if (a >= 0) memset(want + A * S, a, S);
    if (b >= 0) memset(want + B * S, b, S);
    if (c >= 0) memset(want + C * S, c, S);
    int fd = fimg_export(v, path) == 0 ? open(path, O_RDONLY) : -1;
    int ok = fd >= 0 && lseek(fd, 0, 2) == n && pread(fd, got, n, 0) == n && memcmp(got, want, n) == 0;
    if (fd >= 0) close(fd);
    return ok;
}

static int rec(int i, int kind, long lba, int status, int tag)
{
    const struct fimg_record *r = fimg_ledger(i);
    return r && r->kind == kind && r->lba == lba && r->status == status &&
           (tag < 0 ? r->data == 0 : r->data && memcmp(r->data, sec(tag), S) == 0);
}

// ---------------------------------------------------------------------------------------------

// Two writes (one of two sectors), a flush, two more writes, a read.
static void c_layers(void)
{
    CHECK(wr(A, 1, 0x11, 0) == 0 && wr(B, 2, 0x22, 0x33) == 0, "first writes");
    CHECK(view_is(fimg_view_live(), 0x11, 0x22, 0x33), "live after the first writes");
    CHECK(view_is(fimg_view_durable(), -1, -1, -1), "unflushed writes are durable");
    CHECK(disk_flush(d) == 0, "flush");
    CHECK(wr(A, 1, 0x44, 0) == 0 && wr(C, 1, 0x55, 0) == 0, "second writes");
    unsigned char r[S];
    CHECK(disk_read_block(d, A, 1, r) == 0 && memcmp(r, sec(0x44), S) == 0, "a read sees the pending write");
    CHECK(view_is(fimg_view_live(), 0x44, 0x22, 0x55), "live: 44 22 55");
    CHECK(view_is(fimg_view_durable(), 0x11, 0x22, 0x33), "durable: 11 22 33");
    CHECK(fimg_counts.writes == 4 && fimg_counts.write_sectors == 5 && fimg_counts.flushes == 1 &&
          fimg_counts.reads == 1 && fimg_counts.read_sectors == 1 && fimg_counts.failed == 0, "counters");
    CHECK(fimg_read_count(A) == 1 && fimg_read_count(B) == 0, "read counts");
    CHECK(fimg_ledger_count() == 7 && rec(0, FIMG_WRITE, A, 0, 0x11) && rec(1, FIMG_WRITE, B, 0, 0x22) &&
          rec(2, FIMG_WRITE, C, 0, 0x33) && rec(3, FIMG_FLUSH, -1, 0, -1) && rec(4, FIMG_WRITE, A, 0, 0x44) &&
          rec(5, FIMG_WRITE, C, 0, 0x55) && rec(6, FIMG_READ, A, 0, -1), "ledger");
    int end = fimg_ledger_count();
    CHECK(fimg_cut_pending(end) == 2 && fimg_cut_pending(3) == 3 && fimg_cut_pending(0) == 0 &&
          fimg_cut_pending(4) == 0, "pending writes at the cuts");
    CHECK(view_is(fimg_cut_prefix(end, 0), 0x11, 0x22, 0x33), "crash at the end, none kept");
    CHECK(view_is(fimg_cut_prefix(end, 1), 0x44, 0x22, 0x33), "crash at the end, first kept");
    CHECK(view_is(fimg_cut_prefix(end, 2), 0x44, 0x22, 0x55), "crash at the end, both kept");
    unsigned char second[] = { 0, 1 };
    CHECK(view_is(fimg_cut(end, second), 0x11, 0x22, 0x55), "crash at the end, second kept");
    CHECK(view_is(fimg_cut(end, 0), 0x11, 0x22, 0x33), "crash at the end, keep NULL");
    CHECK(view_is(fimg_cut_prefix(3, 0), -1, -1, -1), "crash before the flush, none kept");
    CHECK(view_is(fimg_cut_prefix(2, 2), 0x11, 0x22, -1), "crash after the second write, both kept");
    unsigned char middle[] = { 0, 1, 0 };
    CHECK(view_is(fimg_cut(3, middle), -1, 0x22, -1), "crash before the flush, B kept");
    CHECK(view_is(fimg_cut_prefix(4, 3), 0x11, 0x22, 0x33), "crash right after the flush");
    CHECK(view_is(fimg_cut_prefix(0, 5), -1, -1, -1), "crash before anything");
    CHECK(export_is(fimg_cut_prefix(end, 1), "cut-end-1.img", 0x44, 0x22, 0x33), "export of a cut");
    CHECK(export_is(fimg_cut(3, middle), "cut-3-b.img", -1, 0x22, -1), "export of a cut before the flush");
    CHECK(export_is(fimg_view_live(), "live.img", 0x44, 0x22, 0x55), "export of the live view");
    // A new ledger starts from the durable layer and keeps the pending writes pending.
    fimg_ledger_reset();
    CHECK(fimg_ledger_count() == 2 && rec(0, FIMG_WRITE, A, 0, 0x44) && rec(1, FIMG_WRITE, C, 0, 0x55), "reset ledger");
    CHECK(view_is(fimg_cut_prefix(2, 0), 0x11, 0x22, 0x33) && view_is(fimg_cut_prefix(2, 2), 0x44, 0x22, 0x55),
          "cuts after a reset");
    // A cut made the media: the durable layer, nothing pending, an empty ledger.
    fimg_load_view(fimg_cut(2, second));
    CHECK(view_is(fimg_view_durable(), 0x11, 0x22, 0x55) && view_is(fimg_view_live(), 0x11, 0x22, 0x55) &&
          fimg_ledger_count() == 0 && fimg_cut_pending(0) == 0, "a cut loaded as the media");
}

// The same sector twice before a flush: every order a drive may keep.
static void c_rewrite(void)
{
    CHECK(wr(A, 1, 1, 0) == 0 && wr(A, 1, 2, 0) == 0, "writes");
    CHECK(view_is(fimg_view_live(), 2, -1, -1) && view_is(fimg_view_durable(), -1, -1, -1), "layers");
    unsigned char first[] = { 1, 0 }, second[] = { 0, 1 };
    CHECK(view_is(fimg_cut(2, first), 1, -1, -1) && view_is(fimg_cut(2, second), 2, -1, -1) &&
          view_is(fimg_cut_prefix(2, 2), 2, -1, -1), "cuts");
    CHECK(disk_flush(d) == 0 && view_is(fimg_view_durable(), 2, -1, -1), "flushed");
}

// One sector written more times than the image has sectors before a flush (fat16.c rewrites a
// FAT sector once per cluster it links): one pending write per call, each can be cut.
static void c_many_writes(void)
{
    long n = fimg_sectors() + 8, bad_writes = 0;
    for (long i = 0; i < n; i++)
    {
        bad_writes += wr(A, 1, (int)(i % 251) + 1, 0) != 0;
    }
    CHECK(bad_writes == 0 && fimg_cut_pending(fimg_ledger_count()) == n, "%ld pending writes", n);
    int end = fimg_ledger_count();
    CHECK(view_is(fimg_cut_prefix(end, 0), -1, -1, -1), "none kept");
    CHECK(view_is(fimg_cut_prefix(end, n - 1), (int)((n - 2) % 251) + 1, -1, -1), "all but the last kept");
    CHECK(view_is(fimg_cut_prefix(end, n), (int)((n - 1) % 251) + 1, -1, -1), "all kept");
}

// Live and durable views are copies: a later flush or write changes neither.
static void c_views(void)
{
    CHECK(wr(A, 1, 0x11, 0) == 0, "write");
    const unsigned char **live = fimg_view_live(), **dur = fimg_view_durable();
    CHECK(disk_flush(d) == 0 && wr(A, 1, 0x22, 0) == 0 && wr(B, 1, 0x33, 0) == 0, "flush and write again");
    CHECK(view_is(live, 0x11, -1, -1), "the live view kept its sectors");
    CHECK(view_is(dur, -1, -1, -1), "the durable view kept its sectors");
    CHECK(view_is(fimg_view_live(), 0x22, 0x33, -1) && view_is(fimg_view_durable(), 0x11, -1, -1), "new views");
}

static void c_read_failure(void)
{
    unsigned char r[S];
    CHECK(wr(B, 1, 0x66, 0) == 0, "write");
    fimg_fail_read(2, -EIO);
    CHECK(disk_read_block(d, A, 1, r) == 0, "first read");
    CHECK(disk_read_block(d, B, 1, r) == -EIO, "second read fails");
    CHECK(disk_read_block(d, B, 1, r) == 0 && memcmp(r, sec(0x66), S) == 0, "third read");
    CHECK(disk_read_block(d, fimg_sectors() - 1, 2, r) == -EIO, "read past the image");
    CHECK(disk_read_block(d, A, 0, r) == -EINVARG && disk_read_block(d, A, 1, 0) == -EINVARG, "bad reads");
    CHECK(disk_read_block(d, 0x0FFFFFFF, 2, r) == -EINVARG && disk_read_block(d, 0x10000000, 1, r) == -EINVARG,
          "reads past LBA28");
    CHECK(fimg_counts.reads == 4 && fimg_counts.read_sectors == 2 && fimg_counts.failed == 2, "counters");
    CHECK(rec(1, FIMG_READ, A, 0, -1) && rec(2, FIMG_READ, B, -EIO, -1) && rec(3, FIMG_READ, B, 0, -1) &&
          rec(4, FIMG_READ, fimg_sectors() - 1, -EIO, -1) && fimg_ledger_count() == 5, "ledger");
    CHECK(view_is(fimg_view_live(), -1, 0x66, -1), "a failed read changes nothing");
}

static void c_write_failure(void)
{
    fimg_fail_write(2, -EIO);
    CHECK(wr(A, 1, 0x11, 0) == 0, "first write");
    CHECK(wr(B, 2, 0x22, 0x33) == -EIO, "second write fails");
    CHECK(wr(C, 1, 0x55, 0) == 0, "third write");
    CHECK(view_is(fimg_view_live(), 0x11, -1, 0x55), "live: the failed write left nothing");
    CHECK(fimg_cut_pending(3) == 2 && view_is(fimg_cut_prefix(3, 2), 0x11, -1, 0x55), "cut");
    CHECK(rec(1, FIMG_WRITE, B, -EIO, -1) && fimg_counts.writes == 3 && fimg_counts.write_sectors == 2 &&
          fimg_counts.failed == 1, "ledger and counters");
}

static void c_flush_failure(void)
{
    CHECK(wr(A, 1, 0x11, 0) == 0, "write");
    fimg_fail_flush(1, -EIO);
    CHECK(disk_flush(d) == -EIO, "flush fails");
    CHECK(view_is(fimg_view_durable(), -1, -1, -1) && view_is(fimg_view_live(), 0x11, -1, -1), "still pending");
    CHECK(fimg_cut_pending(2) == 1 && view_is(fimg_cut_prefix(2, 0), -1, -1, -1), "a failed flush makes nothing durable");
    CHECK(disk_flush(d) == 0 && view_is(fimg_view_durable(), 0x11, -1, -1), "the next flush works");
    CHECK(fimg_cut_pending(3) == 0 && view_is(fimg_cut_prefix(3, 0), 0x11, -1, -1), "durable in the cut");
    CHECK(rec(1, FIMG_FLUSH, -1, -EIO, -1) && rec(2, FIMG_FLUSH, -1, 0, -1) && fimg_counts.flushes == 2, "ledger");
}

static void c_capacity(void)
{
    unsigned int n = 0;
    CHECK(disk_write_capacity(d, &n) == 0 && n == fimg_sectors(), "default capacity %u", n);
    fimg_set_capacity(B);
    CHECK(fimg_mount() == 0, "mount");
    CHECK(disk_write_capacity(d, &n) == 0 && n == B, "capacity %u", n);
    fimg_set_capacity(B + 9);   // too late: kept until the next mount, like IDENTIFY
    CHECK(disk_write_capacity(d, &n) == 0 && n == B, "capacity %u after a change", n);
    CHECK(wr(A, 1, 0x11, 0) == 0, "last sector inside");
    CHECK(wr(B, 1, 0x22, 0) == -EINVARG && wr(A, 2, 0x11, 0x22) == -EINVARG, "past the capacity");
    CHECK(fimg_counts.writes == 1 && view_is(fimg_view_live(), 0x11, -1, -1), "refused writes reach nothing");
    fimg_set_capacity(fimg_sectors() + 8);
    CHECK(fimg_mount() == 0, "mount");
    CHECK(wr(C, 1, 0x33, 0) == 0 && wr(fimg_sectors(), 1, 1, 0) == -EIO && fimg_counts.beyond == 1, "past the model");
    fimg_set_capacity_error(-ERDONLY);
    CHECK(fimg_mount() == 0, "mount");
    CHECK(disk_write_capacity(d, &n) == -ERDONLY && wr(A, 1, 1, 0) == -ERDONLY && disk_flush(d) == -ERDONLY,
          "not writable");
    CHECK(view_is(fimg_view_live(), 0x11, -1, 0x33), "nothing written");
    fimg_set_capacity_error(0);
    fimg_set_capacity(0);
    CHECK(fimg_mount() == 0 && disk_write_capacity(d, &n) == -ERDONLY && wr(A, 1, 1, 0) == -ERDONLY &&
          wr(0x10000000, 1, 1, 0) == -EINVARG, "a drive of 0 sectors is not writable; past LBA28 is a bad request");
    fimg_set_capacity(0xFFFFFFFFu);
    CHECK(fimg_mount() == 0 && disk_write_capacity(d, &n) == 0 && n == 0x0FFFFFFF, "capacity capped at LBA28: %u", n);
    CHECK(wr(0x0FFFFFFF, 1, 1, 0) == -EINVARG && fimg_counts.beyond == 1, "the last LBA28 sector is past the capacity");
}

static void c_alloc_failure(void)
{
    fimg_fail_alloc(1);
    CHECK(file_open("0:/blank.elf", FILE_MODE_READ) < 0 && kalloc_failed(), "open with no memory");
    fimg_fail_alloc(0);
    int fd = file_open("0:/blank.elf", FILE_MODE_READ);
    CHECK(fd > 0, "open");
    fclose(fd);
}

// The real kernel code creates a file; exports for the script; crash cuts mounted again.
static void c_kernel(void)
{
    static unsigned char data[5000], back[5000];
    for (int i = 0; i < 5000; i++)
    {
        data[i] = (unsigned char)(i * 7 + (i >> 8));
    }
    char path[512];
    fmt(path, sizeof(path), "%s/expect-MODEL.TXT", out_dir);
    int out = open(path, 01 | 0100 | 01000, 0644);
    CHECK(out >= 0 && write(out, data, 5000) == 5000, "save expect-MODEL.TXT");
    close(out);
    fimg_ledger_reset();
    fimg_reset_counts();
    int fd = file_open("0:/MODEL.TXT", FILE_MODE_WRITE);
    CHECK(fd > 0 && file_write(fd, data, 5000) == 5000, "write MODEL.TXT");
    fclose(fd);
    int end = fimg_ledger_count();
    CHECK(fimg_counts.writes > 0 && fimg_counts.flushes > 0 && fimg_counts.reads > 0, "counters %d %d %d",
          fimg_counts.writes, fimg_counts.flushes, fimg_counts.reads);
    CHECK(fimg_cut_pending(end) == 0, "pending writes after the close");
    fmt(path, sizeof(path), "%s/before.img", out_dir);
    CHECK(fimg_export(fimg_cut(0, 0), path) == 0, "export before.img");
    fmt(path, sizeof(path), "%s/written.img", out_dir);
    CHECK(fimg_export(fimg_view_durable(), path) == 0, "export written.img");
    // written.img with one FAT 2 entry changed: fsck.fat must see it.
    const unsigned char *b = fimg_view_durable()[0];
    long fat2 = (b[14] | (b[15] << 8)) + (b[22] | (b[23] << 8));
    static const unsigned char *bent[1 << 20];
    static unsigned char fat2_sector[S];
    memcpy(bent, fimg_view_durable(), fimg_sectors() * sizeof(*bent));
    memcpy(fat2_sector, bent[fat2], S);
    fat2_sector[2 * 3] ^= 0x5A;
    bent[fat2] = fat2_sector;
    fmt(path, sizeof(path), "%s/fat2.img", out_dir);
    CHECK(fimg_export(bent, path) == 0, "export fat2.img");
    // The crash cut at the end (every write flushed: the base and ledger records), loaded as the
    // media and mounted again, has the file; the export from before has none.
    const unsigned char **cut = fimg_cut(end, 0), **now = fimg_view_durable();
    long differ = 0;
    for (long s = 0; s < fimg_sectors(); s++)
    {
        differ += memcmp(cut[s], now[s], S) != 0;
    }
    CHECK(differ == 0, "the cut at the end is the durable layer (%ld sectors differ)", differ);
    fimg_load_view(cut);
    CHECK(fimg_mount() == 0, "mount the written state");
    fd = file_open("0:/MODEL.TXT", FILE_MODE_READ);
    CHECK(fd > 0 && file_read(fd, back, 5000) == 5000 && memcmp(back, data, 5000) == 0, "read MODEL.TXT back");
    fclose(fd);
    CHECK(fimg_ledger_count() > 0 && fimg_ledger(0)->kind == FIMG_READ, "a fresh ledger after the load");
    fmt(path, sizeof(path), "%s/before.img", out_dir);
    fimg_open(path);
    CHECK(fimg_mount() == 0 && file_open("0:/MODEL.TXT", FILE_MODE_READ) < 0, "no MODEL.TXT before");
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "layers, ledger and cuts", c_layers },
    { "one sector written twice", c_rewrite },
    { "one sector written many times", c_many_writes },
    { "views are copies", c_views },
    { "read failures", c_read_failure },
    { "write failures", c_write_failure },
    { "flush failures", c_flush_failure },
    { "capacity", c_capacity },
    { "allocation failures", c_alloc_failure },
    { "kernel FAT code on the model", c_kernel },
};

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: fat-image-model <image> <outdir>\n");
        return 2;
    }
    out_dir = argv[2];
    fimg_open(argv[1]);
    orig = malloc(fimg_sectors() * S);
    memcpy(orig, fimg_durable(), fimg_sectors() * S);
    A = fimg_sectors() - 3;
    B = fimg_sectors() - 2;
    C = fimg_sectors() - 1;
    // The hand-computed views below assume A, B and C are free (zero) data sectors.
    if (fimg_sectors() > (1 << 20) || !is(fimg_view_live(), A, 0) || !is(fimg_view_live(), B, 0) ||
        !is(fimg_view_live(), C, 0))
    {
        printf("fat-image-model: the last three sectors of the image are not zero\n");
        return 2;
    }
    d = disk_get(0);
    int failed = 0, total = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
            if (fimg_mount() != 0)
            {
                printf("    the image did not mount\n");
                bad++;
            }
            fimg_ledger_reset();
            fimg_reset_counts();
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
    printf("fat-image host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
