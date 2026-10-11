/*
 * Host check for scripts/tests/disk-read.sh: the byte streamer in kernel/disk/streamer.c on top of
 * a model disk. Builds the real streamer.c with the host C compiler; disk_read_block() is the
 * model, which serves a 128-sector pattern, records every sector asked for, can fail one sector,
 * and notes how deep the stack is on each call. Each case runs in its own process.
 *
 * Cases: reads of 0 bytes (no disk read, position unchanged), 1 byte, whole sectors, spans that
 * start or end inside a sector, spans that end exactly on a sector boundary (the next sector is
 * made to fail: it must not be read), 101 sectors in one call (stack no deeper than for 100
 * bytes), a failure part way (the error comes back, the bytes before it are copied and counted
 * in the position, nothing after them is written), a negative seek, a negative length and a
 * length that runs past INT_MAX (refused, no disk read, position unchanged).
 *
 * Prints one line per failing case and "disk-stream host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "disk/disk.h"
#include "disk/streamer.h"

void *memset(void *ptr, int c, unsigned long size);
unsigned int alarm(unsigned int seconds);

#define SECTORS 128
#define SIZE (SECTORS * RORTHOX_SECTOR_SIZE)
#define CANARY 0xC5
#define INT_MAX_VALUE 0x7FFFFFFF

static struct disk the_disk;
static struct
{
    int requests;
    int sectors[4096];
    int bad_total;          // a request for 0 or a negative number of sectors
    int fail_lba;           // this sector fails with -EIO (-1: none)
    unsigned long stack_low; // lowest stack address seen in disk_read_block
} rec;

static unsigned char disk_byte(long pos)
{
    return (unsigned char)(pos * 131 + (pos >> 9) * 7 + 3);
}

struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    volatile char here;
    unsigned long sp = (unsigned long)&here;
    if (!rec.stack_low || sp < rec.stack_low)
    {
        rec.stack_low = sp;
    }
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (total <= 0)
    {
        rec.bad_total++;
        return -EINVARG;
    }
    for (int i = 0; i < total; i++)
    {
        if (rec.requests < 4096)
        {
            rec.sectors[rec.requests++] = lba + i;
        }
        if ((int)(lba + i) == rec.fail_lba || lba + i >= SECTORS)
        {
            return -EIO;
        }
    }
    unsigned char *out = buf;
    for (long i = 0; i < (long)total * RORTHOX_SECTOR_SIZE; i++)
    {
        out[i] = disk_byte((long)lba * RORTHOX_SECTOR_SIZE + i);
    }
    return 0;
}

// The output area: GUARD canary bytes on each side of the bytes a read may write.
#define GUARD 64
static unsigned char area[SIZE + 2 * GUARD];
static unsigned char *const out = area + GUARD;

static struct disk_stream *stream_at(int pos)
{
    kalloc_init();
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.sector_size = RORTHOX_SECTOR_SIZE;
    memset(&rec, 0, sizeof(rec));
    rec.fail_lba = -1;
    memset(area, CANARY, sizeof(area));
    struct disk_stream *stream = diskstreamer_new(0);
    if (!stream || diskstreamer_seek(stream, pos) != 0 || stream->pos != pos)
    {
        printf("    could not make a stream at %d\n", pos);
        return 0;
    }
    return stream;
}

// out[0..copied) holds the disk bytes from pos, the rest of the area is untouched.
static int bytes_ok(const char *what, int pos, int copied)
{
    for (int i = 0; i < (int)sizeof(area); i++)
    {
        int at = i - GUARD;
        unsigned char want = (at >= 0 && at < copied) ? disk_byte((long)pos + at) : CANARY;
        if (area[i] != want)
        {
            printf("    %s: byte %d of the output is %02x, expected %02x\n", what, at, area[i], want);
            return 0;
        }
    }
    return 1;
}

// Only sectors first..last were asked for, each one at least once.
static int sectors_ok(const char *what, int first, int last)
{
    if (rec.bad_total)
    {
        printf("    %s: asked the disk for %d sectors or fewer\n", what, 0);
        return 0;
    }
    for (int i = 0; i < rec.requests; i++)
    {
        if (rec.sectors[i] < first || rec.sectors[i] > last)
        {
            printf("    %s: read sector %d, only %d..%d hold the bytes\n", what, rec.sectors[i], first, last);
            return 0;
        }
    }
    for (int s = first; s <= last; s++)
    {
        int seen = 0;
        for (int i = 0; i < rec.requests; i++)
        {
            seen |= rec.sectors[i] == s;
        }
        if (!seen)
        {
            printf("    %s: never read sector %d\n", what, s);
            return 0;
        }
    }
    return 1;
}

// A read of total bytes from pos that must work. The sector after the span fails if asked for.
static int read_ok(const char *what, int pos, int total)
{
    struct disk_stream *stream = stream_at(pos);
    if (!stream)
    {
        return 1;
    }
    int last = total ? (pos + total - 1) / RORTHOX_SECTOR_SIZE : -1;
    rec.fail_lba = total ? last + 1 : pos / RORTHOX_SECTOR_SIZE;
    int res = diskstreamer_read(stream, out, total);
    if (res != 0)
    {
        printf("    %s: returned %d, expected 0\n", what, res);
        return 1;
    }
    if (stream->pos != pos + total)
    {
        printf("    %s: position %d, expected %d\n", what, stream->pos, pos + total);
        return 1;
    }
    if (!total && rec.requests)
    {
        printf("    %s: read %d sector(s) for no bytes\n", what, rec.requests);
        return 1;
    }
    if (total && !sectors_ok(what, pos / RORTHOX_SECTOR_SIZE, last))
    {
        return 1;
    }
    return bytes_ok(what, pos, total) ? 0 : 1;
}

static int c_zero(void) { return read_ok("0 bytes at 100", 100, 0); }
static int c_zero_boundary(void) { return read_ok("0 bytes at 512", 512, 0); }
static int c_byte(void) { return read_ok("1 byte at 0", 0, 1); }
static int c_byte_end(void) { return read_ok("1 byte at 511", 511, 1); }
static int c_two_bytes(void) { return read_ok("2 bytes at 511", 511, 2); }
static int c_sector(void) { return read_ok("512 bytes at 0", 0, 512); }
static int c_sectors(void) { return read_ok("3 sectors at 1024", 1024, 3 * 512); }
static int c_unaligned(void) { return read_ok("1000 bytes at 100", 100, 1000); }
static int c_to_boundary(void) { return read_ok("724 bytes at 300 (ends at 1024)", 300, 724); }
static int c_cluster(void) { return read_ok("64 sectors at 4096 (ends on a boundary)", 4096, 64 * 512); }

// 101 sectors in one call: no deeper stack than for a few bytes (neither read ends on a boundary).
static int c_many(void)
{
    if (read_ok("100 bytes at 3", 3, 100))
    {
        return 1;
    }
    unsigned long one = rec.stack_low;
    if (read_ok("100 sectors and 5 bytes at 3", 3, 100 * 512 + 5))
    {
        return 1;
    }
    if (rec.stack_low + 256 < one)
    {
        printf("    101 sectors: stack %lu bytes deeper than for 100 bytes\n", one - rec.stack_low);
        return 1;
    }
    return 0;
}

// Sector 2 fails: the error comes back, the bytes before sector 2 are copied and counted.
static int c_fail_part_way(void)
{
    struct disk_stream *stream = stream_at(100);
    if (!stream)
    {
        return 1;
    }
    rec.fail_lba = 2;
    int res = diskstreamer_read(stream, out, 2000);
    if (res != -EIO)
    {
        printf("    failure in sector 2: returned %d, expected %d\n", res, -EIO);
        return 1;
    }
    if (stream->pos != 1024)
    {
        printf("    failure in sector 2: position %d, expected 1024\n", stream->pos);
        return 1;
    }
    if (!sectors_ok("failure in sector 2", 0, 2))
    {
        return 1;
    }
    return bytes_ok("failure in sector 2", 100, 1024 - 100) ? 0 : 1;
}

// A negative seek is refused and the position stays.
static int c_negative_seek(void)
{
    struct disk_stream *stream = stream_at(700);
    if (!stream)
    {
        return 1;
    }
    int res = diskstreamer_seek(stream, -1);
    if (res >= 0 || stream->pos != 700)
    {
        printf("    seek to -1: returned %d with position %d, expected an error and 700\n", res, stream->pos);
        return 1;
    }
    return 0;
}

// A read that can't be done is refused before the disk is touched.
static int refused(const char *what, int pos, int total)
{
    struct disk_stream *stream = stream_at(pos);
    if (!stream)
    {
        return 1;
    }
    int res = diskstreamer_read(stream, out, total);
    if (res >= 0)
    {
        printf("    %s: returned %d, expected an error\n", what, res);
        return 1;
    }
    if (rec.requests || rec.bad_total || stream->pos != pos)
    {
        printf("    %s: read %d sector(s), position %d (expected none and %d)\n", what, rec.requests, stream->pos, pos);
        return 1;
    }
    return bytes_ok(what, pos, 0) ? 0 : 1;
}

static int c_negative_total(void) { return refused("read of -1 bytes", 700, -1); }
static int c_negative_big(void) { return refused("read of INT_MIN bytes", 700, -INT_MAX_VALUE - 1); }
static int c_past_int_max(void) { return refused("read of 100 bytes at INT_MAX - 10", INT_MAX_VALUE - 10, 100); }

struct test_case
{
    const char *name;
    int (*run)(void);
};

static const struct test_case cases[] = {
    { "0 bytes", c_zero },
    { "0 bytes on a boundary", c_zero_boundary },
    { "1 byte", c_byte },
    { "last byte of a sector", c_byte_end },
    { "2 bytes over a boundary", c_two_bytes },
    { "1 sector", c_sector },
    { "3 sectors", c_sectors },
    { "unaligned span", c_unaligned },
    { "unaligned span to a boundary", c_to_boundary },
    { "64 sectors", c_cluster },
    { "101 sectors, stack", c_many },
    { "failure part way", c_fail_part_way },
    { "negative seek", c_negative_seek },
    { "negative length", c_negative_total },
    { "INT_MIN length", c_negative_big },
    { "length past INT_MAX", c_past_int_max },
};

int main(void)
{
    int total = sizeof(cases) / sizeof(cases[0]);
    int failed = 0;
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
            int res = cases[i].run();
            fflush(0);
            _exit(res ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("  FAIL %s%s\n", cases[i].name, WIFSIGNALED(status) ? " (killed)" : "");
            failed++;
        }
    }
    printf("disk-stream host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
