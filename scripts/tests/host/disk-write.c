/*
 * Host check for scripts/tests/disk-read.sh: WRITE SECTORS (0x30), FLUSH CACHE (0xE7) and IDENTIFY
 * DEVICE (0xEC) in kernel/disk/disk.c against a model of the primary IDE channel. Builds the real
 * disk.c with the host C compiler; insb, insw, outb and outw are the model's ports. Each case runs
 * in its own process.
 *
 * The model records rule breaks: a task file write while BSY or DRQ is set, a command written
 * without DRDY, data words without DRQ (or IDENTIFY words outside its data phase), any command
 * other than the three, and it stops a case that keeps polling after HARD_CAP status reads.
 *
 * IDENTIFY: QEMU's words give the sector count from words 60-61; a drive without FLUSH CACHE
 * (word 83 bit 12), with an invalid word 83, an ATAPI word 0, no LBA, 0 sectors or 4 KiB logical
 * sectors is refused with -ERDONLY, as is an absent drive (status 0x00) and an aborted IDENTIFY;
 * DF, no DRQ, a stuck BSY and ERR after the data give -EIO; counts above 28 bits are clamped; all
 * 256 words are read; the result (also a refusal) is kept, so IDENTIFY goes out once per boot.
 * WRITE: 1, 3, 256 sectors (count register 0), LBA bits 24-27, the last 28-bit sectors, slow
 * sectors, late DRDY; ERR, DF, ERR with DRQ before a sector, ERR/DF/DRQ/BSY after the last one,
 * a stuck BSY and a DRQ that never comes (-EIO, no data after the failure, bounded); requests
 * refused before any port (0, -1, 257 sectors, NULL, past 28 bits, another disk) and past the
 * drive's sector count (after IDENTIFY, before WRITE). FLUSH: ok, slow (past the normal poll
 * limit), ERR, DF, DRQ, BSY forever (gives up within its own limit). A write after a failed one.
 *
 * Prints one line per failing case and "disk-write host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "disk/disk.h"

void *memset(void *ptr, int c, unsigned long size);
int snprintf(char *s, unsigned long n, const char *fmt, ...);
void *malloc(unsigned long size);
unsigned int alarm(unsigned int seconds);

#define HARD_CAP 400000000L
#define NORMAL_LIMIT 2000000L     // a failing command (not FLUSH) must give up within this
#define FLUSH_LIMIT 200000000L    // a stuck FLUSH must give up within this
#define WORDS 256

enum phase { IDLE, BUSY, DRQ_W, DRQ_ID, DONE, FAILED, FLUSHING };

static struct
{
    long pre_reads;            // status reads showing pre_status before the drive is ready
    unsigned char pre_status;
    unsigned char idle_status; // 0x50 (DRDY|DSC)
    unsigned short id[WORDS];
    int id_abort;              // IDENTIFY ends in ERR with ABRT
    unsigned char id_fail;     // nonzero: status after IDENTIFY instead of DRQ
    unsigned char id_end;      // nonzero: status after the 256 IDENTIFY words (0x50 otherwise)
    long busy_reads;           // BSY reads after a command and after each block
    int fail_sector;           // write: failure instead of DRQ for this sector (-1 none)
    unsigned char fail_status;
    unsigned char end_status;  // after the last block (0x50)
    long flush_reads;          // BSY reads for FLUSH (-1: never ends)
    unsigned char flush_end;   // status after FLUSH (0x50)

    enum phase phase;
    long countdown;
    int cmd, sector, word, count;
    unsigned int lba;
    unsigned char regs[8];
    int ncmd;
    int cmds[300];
    long status_reads, port_ops, violations, words_out, words_in;
    long event_reads;
    unsigned short *data;
    char first_violation[160];
} dev;

static void violation(const char *fmt, unsigned int a, unsigned int b)
{
    if (!dev.violations++)
    {
        snprintf(dev.first_violation, sizeof(dev.first_violation), fmt, a, b);
    }
}

static void qemu_identify(void)
{
    memset(dev.id, 0, sizeof(dev.id));
    dev.id[0] = 0x0040;
    dev.id[49] = 0x0B00;
    dev.id[60] = 0x8077;
    dev.id[61] = 0x0000;
    dev.id[82] = 0x4021;
    dev.id[83] = 0x7400;
    dev.id[85] = 0x4021;
    dev.id[86] = 0x3400;
    dev.id[106] = 0x6000;
}

static void drive_reset(void)
{
    unsigned short *data = dev.data;
    memset(&dev, 0, sizeof(dev));
    dev.data = data;
    dev.idle_status = 0x50;
    dev.fail_sector = -1;
    dev.end_status = 0x50;
    dev.flush_end = 0x50;
    dev.phase = IDLE;
    qemu_identify();
}

static unsigned char status_now(void)
{
    switch (dev.phase)
    {
    case IDLE: return dev.pre_reads > 0 ? dev.pre_status : dev.idle_status;
    case BUSY: return 0x80;
    case FLUSHING: return 0x80;
    case DRQ_W: case DRQ_ID: return 0x58;
    case DONE: return dev.cmd == 0x30 ? dev.end_status : dev.cmd == 0xE7 ? dev.flush_end : dev.id_end ? dev.id_end : 0x50;
    case FAILED: return dev.fail_status;
    }
    return 0;
}

static void busy_done(void)
{
    dev.event_reads = dev.status_reads;
    if (dev.cmd == 0x30)
    {
        if (dev.sector == dev.count)
        {
            dev.phase = DONE;
        }
        else if (dev.sector == dev.fail_sector)
        {
            dev.phase = FAILED;
        }
        else
        {
            dev.phase = DRQ_W;
        }
    }
    else if (dev.cmd == 0xEC)
    {
        if (dev.id_fail)
        {
            dev.phase = FAILED;
            dev.fail_status = dev.id_fail;
        }
        else if (dev.id_abort)
        {
            dev.phase = FAILED;
            dev.fail_status = 0x41;
        }
        else
        {
            dev.phase = DRQ_ID;
        }
    }
}

static unsigned char status_read(void)
{
    if (++dev.status_reads > HARD_CAP)
    {
        printf("    still polling after %ld status reads (status %02x)\n", HARD_CAP, status_now());
        fflush(0);
        _exit(3);
    }
    if (dev.phase == IDLE && dev.pre_reads > 0)
    {
        unsigned char s = dev.pre_status;
        dev.pre_reads--;
        return s;
    }
    if (dev.phase == BUSY)
    {
        if (dev.countdown > 0)
        {
            dev.countdown--;
        }
        else
        {
            busy_done();
        }
    }
    else if (dev.phase == FLUSHING)
    {
        if (dev.flush_reads >= 0)
        {
            if (dev.countdown > 0)
            {
                dev.countdown--;
            }
            else
            {
                dev.phase = DONE;
                dev.event_reads = dev.status_reads;
            }
        }
    }
    return status_now();
}

unsigned char insb(unsigned short port)
{
    dev.port_ops++;
    if (port == 0x1F7 || port == 0x3F6)
    {
        return status_read();
    }
    if (port == 0x1F1)
    {
        return (dev.phase == FAILED && dev.fail_status == 0x41) ? 0x04 : 0x00;
    }
    if (port >= 0x1F2 && port <= 0x1F6)
    {
        return dev.regs[port - 0x1F0];
    }
    violation("insb from port %x", port, 0);
    return 0xFF;
}

unsigned short insw(unsigned short port)
{
    dev.port_ops++;
    if (port != 0x1F0 || dev.phase != DRQ_ID)
    {
        violation("insw from %x in phase %u", port, dev.phase);
        return 0xFFFF;
    }
    unsigned short w = dev.id[dev.word];
    dev.words_in++;
    if (++dev.word == WORDS)
    {
        dev.phase = DONE;
    }
    return w;
}

void outw(unsigned short port, unsigned short val)
{
    dev.port_ops++;
    if (port != 0x1F0 || dev.phase != DRQ_W)
    {
        violation("outw to %x without DRQ (status %02x)", port, status_now());
        return;
    }
    dev.data[dev.sector * WORDS + dev.word] = val;
    dev.words_out++;
    if (++dev.word == WORDS)
    {
        dev.word = 0;
        dev.sector++;
        dev.phase = BUSY;
        dev.countdown = dev.busy_reads;
    }
}

void outb(unsigned short port, unsigned char val)
{
    dev.port_ops++;
    if (port == 0x3F6)
    {
        return;
    }
    if (port < 0x1F1 || port > 0x1F7)
    {
        violation("outb to port %x value %02x", port, val);
        return;
    }
    unsigned char status = status_now();
    if (status & 0x88)
    {
        violation("outb to port %x while status is %02x", port, status);
        return;
    }
    dev.regs[port - 0x1F0] = val;
    if (port != 0x1F7)
    {
        return;
    }
    if (!(status & 0x40))
    {
        violation("command %02x written without DRDY (status %02x)", val, status);
    }
    if (dev.ncmd < 300)
    {
        dev.cmds[dev.ncmd] = val;
    }
    dev.ncmd++;
    dev.cmd = val;
    dev.word = 0;
    dev.sector = 0;
    if (val == 0x30)
    {
        if ((dev.regs[6] & 0xF0) != 0xE0)
        {
            violation("drive/head %02x not master+LBA", dev.regs[6], 0);
        }
        dev.lba = dev.regs[3] | (dev.regs[4] << 8) | (dev.regs[5] << 16) | ((dev.regs[6] & 0x0F) << 24);
        dev.count = dev.regs[2] ? dev.regs[2] : 256;
        dev.phase = BUSY;
        dev.countdown = dev.busy_reads;
    }
    else if (val == 0xEC)
    {
        dev.phase = BUSY;
        dev.countdown = dev.busy_reads;
    }
    else if (val == 0xE7)
    {
        dev.phase = FLUSHING;
        dev.countdown = dev.flush_reads;
    }
    else
    {
        violation("unexpected command %02x", val, 0);
    }
}

struct filesystem *fs_resolve(struct disk *disk)
{
    return 0;
}

static unsigned char *buf;

static unsigned char pat(unsigned int lba, int i)
{
    return (unsigned char)(lba * 13u + i * 7u + 0x5A);
}

static void fill(unsigned int lba, int total)
{
    for (int s = 0; s < total; s++)
        for (int i = 0; i < 512; i++)
            buf[s * 512 + i] = pat(lba + s, i);
}

static int fail(const char *what, const char *why)
{
    printf("    %s: %s\n", what, why);
    return 1;
}

static int clean(const char *what)
{
    if (dev.violations)
    {
        printf("    %s: %s\n", what, dev.first_violation);
        return 1;
    }
    return 0;
}

static int data_ok(unsigned int lba, int total)
{
    unsigned char *d = (unsigned char *)dev.data;
    for (int s = 0; s < total; s++)
        for (int i = 0; i < 512; i++)
            if (d[s * 512 + i] != pat(lba + s, i))
                return 0;
    return 1;
}

static int count_cmd(int c)
{
    int n = 0;
    for (int i = 0; i < dev.ncmd && i < 300; i++)
        n += dev.cmds[i] == c;
    return n;
}

// IDENTIFY with the given words: expected status and sectors.
static int ident(const char *what, int expected, unsigned int sectors_expected)
{
    unsigned int sectors = 12345;
    int res = disk_write_capacity(disk_get(0), &sectors);
    if (res != expected)
    {
        char m[80];
        snprintf(m, sizeof(m), "returned %d, expected %d", res, expected);
        return fail(what, m);
    }
    if (res == 0 && sectors != sectors_expected)
    {
        char m[80];
        snprintf(m, sizeof(m), "sectors %u, expected %u", sectors, sectors_expected);
        return fail(what, m);
    }
    if (res != 0 && sectors != 12345)
        return fail(what, "wrote *sectors on failure");
    if (dev.status_reads > NORMAL_LIMIT)
        return fail(what, "too many status reads");
    return clean(what);
}

static int c_id_qemu(void) { return ident("QEMU words", 0, 0x8077); }
static int c_id_noflush(void) { dev.id[83] = 0x6400; return ident("w83 without FLUSH bit 12", -ERDONLY, 0); }
static int c_id_w83_invalid(void) { dev.id[83] = 0xF400; return ident("w83 bits 15:14 = 11", -ERDONLY, 0); }
static int c_id_w83_zero(void) { dev.id[83] = 0x1000; return ident("w83 bits 15:14 = 00", -ERDONLY, 0); }
static int c_id_atapi_w0(void) { dev.id[0] = 0x85C0; return ident("w0 bit 15 (ATAPI)", -ERDONLY, 0); }
static int c_id_incomplete_w0(void) { dev.id[0] = 0x0044; return ident("w0 bit 2 (incomplete)", -ERDONLY, 0); }
static int c_id_nolba(void) { dev.id[49] = 0x0800; return ident("w49 without LBA", -ERDONLY, 0); }
static int c_id_zero(void) { dev.id[60] = 0; dev.id[61] = 0; return ident("0 sectors", -ERDONLY, 0); }
static int c_id_clamp(void) { dev.id[60] = 0x0000; dev.id[61] = 0x1000; return ident("w60-61 = 0x10000000", 0, 0x0FFFFFFF); }
static int c_id_max(void) { dev.id[60] = 0xFFFF; dev.id[61] = 0x0FFF; return ident("w60-61 = 0x0fffffff", 0, 0x0FFFFFFF); }
static int c_id_4k(void) { dev.id[106] = 0x5000; dev.id[117] = 2048; return ident("4096-byte logical sectors", -ERDONLY, 0); }
static int c_id_512long(void) { dev.id[106] = 0x5000; dev.id[117] = 256; return ident("w106 bit 12 with 256 words", 0, 0x8077); }
static int c_id_106_invalid(void) { dev.id[106] = 0x9000; dev.id[117] = 2048; return ident("w106 not valid (bit 15)", 0, 0x8077); }
static int c_id_absent(void)
{
    dev.idle_status = 0x00;
    int r = ident("status 0x00 (no drive)", -ERDONLY, 0);
    if (!r && dev.ncmd)
        r = fail("status 0x00", "a command was sent");
    return r;
}
static int c_id_float(void)
{
    dev.idle_status = 0xFF;
    // 0xFF has BSY set, so the first wait runs out: -EIO within the normal limit, no command.
    unsigned int s;
    int res = disk_write_capacity(disk_get(0), &s);
    if (res != -EIO && res != -ERDONLY)
        return fail("status 0xFF", "not refused");
    if (dev.ncmd)
        return fail("status 0xFF", "a command was sent");
    if (dev.status_reads > NORMAL_LIMIT)
        return fail("status 0xFF", "too many status reads");
    return 0;
}
static int c_id_abort(void) { dev.id_abort = 1; return ident("IDENTIFY aborted (ERR, ABRT)", -ERDONLY, 0); }
static int c_id_df(void) { dev.id_fail = 0x60; return ident("IDENTIFY ends in DF", -EIO, 0); }
static int c_id_nodrq(void) { dev.id_fail = 0x50; return ident("IDENTIFY without DRQ", -EIO, 0); }
static int c_id_stuck(void) { dev.id_fail = 0x80; return ident("IDENTIFY BSY forever", -EIO, 0); }
static int c_id_end_err(void) { dev.id_end = 0x41; return ident("ERR after the IDENTIFY data", -EIO, 0); }
static int c_id_nodrdy(void)
{
    dev.pre_reads = 50000;
    dev.pre_status = 0x10; // no BSY, no DRQ, no DRDY yet
    return ident("DRDY late after the select", 0, 0x8077);
}
static int c_id_words(void)
{
    int r = ident("all 256 words read", 0, 0x8077);
    if (!r && dev.words_in != WORDS)
        r = fail("all 256 words read", "not 256 words");
    return r;
}
static int c_id_cached(void)
{
    unsigned int s;
    disk_write_capacity(disk_get(0), &s);
    disk_write_capacity(disk_get(0), &s);
    fill(100, 1);
    disk_write_block(disk_get(0), 100, 1, buf);
    disk_flush(disk_get(0));
    if (count_cmd(0xEC) != 1)
        return fail("IDENTIFY once", "IDENTIFY sent more than once");
    return clean("IDENTIFY once");
}
static int c_id_fail_cached(void)
{
    dev.id[83] = 0x6400;
    unsigned int s;
    fill(100, 1);
    int a = disk_write_block(disk_get(0), 100, 1, buf);
    int b = disk_flush(disk_get(0));
    int c = disk_write_capacity(disk_get(0), &s);
    if (a != -ERDONLY || b != -ERDONLY || c != -ERDONLY)
        return fail("cached refusal", "not -ERDONLY three times");
    if (count_cmd(0xEC) != 1 || count_cmd(0x30) || count_cmd(0xE7))
        return fail("cached refusal", "wrong commands");
    return clean("cached refusal");
}

static int write_ok(const char *what, unsigned int lba, int total)
{
    fill(lba, total);
    int res = disk_write_block(disk_get(0), lba, total, buf);
    if (res != 0)
    {
        char m[80];
        snprintf(m, sizeof(m), "returned %d", res);
        return fail(what, m);
    }
    if (count_cmd(0x30) != 1 || dev.lba != lba || dev.count != total)
        return fail(what, "wrong WRITE command");
    if (dev.words_out != (long)total * WORDS || !data_ok(lba, total))
        return fail(what, "wrong data");
    return clean(what);
}

static int c_w1(void) { return write_ok("1 sector", 1000, 1); }
static int c_w3(void) { dev.id[60] = 0xFFFF; dev.id[61] = 0x0FFF; return write_ok("3 sectors, LBA bits 24-27", 0x5ABCDEF, 3); }
static int c_w256(void) { return write_ok("256 sectors (count 0)", 2000, 256); }
static int c_w_slow(void) { dev.busy_reads = 50000; return write_ok("slow sectors", 3000, 3); }
static int c_w_high(void)
{
    dev.id[60] = 0xFFFF; dev.id[61] = 0x0FFF;
    return write_ok("last 28-bit sectors", 0x0FFFFFFE - 255, 256);
}
static int c_w_drdy(void)
{
    unsigned int s;
    disk_write_capacity(disk_get(0), &s);
    dev.pre_reads = 50000;
    dev.pre_status = 0x10;
    return write_ok("DRDY late before WRITE", 100, 2);
}

static int write_fails(const char *what, unsigned int lba, int total, long words_expected, int prompt)
{
    fill(lba, total);
    int res = disk_write_block(disk_get(0), lba, total, buf);
    char m[100];
    if (res != -EIO)
    {
        snprintf(m, sizeof(m), "returned %d, expected -EIO", res);
        return fail(what, m);
    }
    if (dev.words_out != words_expected)
    {
        snprintf(m, sizeof(m), "%ld words sent, expected %ld", dev.words_out, words_expected);
        return fail(what, m);
    }
    if (dev.status_reads > NORMAL_LIMIT + 10000)
        return fail(what, "too many status reads");
    if (prompt && dev.status_reads - dev.event_reads > 100)
        return fail(what, "kept polling after the failure showed");
    return clean(what);
}

static int c_w_err2(void) { dev.fail_sector = 1; dev.fail_status = 0x41; return write_fails("ERR after sector 1 of 3", 500, 3, WORDS, 1); }
static int c_w_err0(void) { dev.fail_sector = 0; dev.fail_status = 0x41; return write_fails("ERR before sector 1", 500, 2, 0, 1); }
static int c_w_df(void) { dev.fail_sector = 2; dev.fail_status = 0x60; return write_fails("DF before sector 3", 500, 3, 2 * WORDS, 1); }
static int c_w_errdrq(void) { dev.fail_sector = 1; dev.fail_status = 0x49; return write_fails("ERR with DRQ", 500, 3, WORDS, 1); }
static int c_w_end_err(void) { dev.end_status = 0x41; return write_fails("ERR at the end", 500, 2, 2 * WORDS, 1); }
static int c_w_end_df(void) { dev.end_status = 0x60; return write_fails("DF at the end", 500, 2, 2 * WORDS, 1); }
static int c_w_end_drq(void) { dev.end_status = 0x58; return write_fails("DRQ at the end", 500, 1, WORDS, 1); }
static int c_w_end_bsy(void) { dev.end_status = 0x80; return write_fails("BSY forever at the end", 500, 1, WORDS, 0); }
static int c_w_bsy(void) { dev.fail_sector = 1; dev.fail_status = 0x80; return write_fails("BSY forever after sector 1", 500, 2, WORDS, 0); }
static int c_w_nodrq(void) { dev.fail_sector = 0; dev.fail_status = 0x50; return write_fails("DRQ never comes", 500, 1, 0, 0); }

static int c_w_refused(void)
{
    struct disk *d = disk_get(0);
    fill(0, 2);
    int bad = 0;
    // Before IDENTIFY: no port at all
    bad |= disk_write_block(d, 100, 0, buf) != -EINVARG;
    bad |= disk_write_block(d, 100, -1, buf) != -EINVARG;
    bad |= disk_write_block(d, 100, 257, buf) != -EINVARG;
    bad |= disk_write_block(d, 100, 1, 0) != -EINVARG;
    bad |= disk_write_block(d, 0x10000000, 1, buf) != -EINVARG;
    bad |= disk_write_block(d, 0x0FFFFFFF, 2, buf) != -EINVARG;
    bad |= disk_write_block(d, 0xFFFFFFFF, 1, buf) != -EINVARG;
    struct disk other;
    memset(&other, 0, sizeof(other));
    bad |= disk_write_block(&other, 100, 1, buf) != -EIO;
    bad |= disk_flush(&other) != -EIO;
    unsigned int s;
    bad |= disk_write_capacity(&other, &s) != -EIO;
    bad |= disk_write_capacity(d, 0) != -EINVARG;
    if (bad)
        return fail("refused requests", "wrong result");
    if (dev.port_ops)
        return fail("refused requests", "touched the ports");
    // Past the drive's end (0x8077 sectors): IDENTIFY only, no WRITE
    bad |= disk_write_block(d, 0x8077, 1, buf) != -EINVARG;
    bad |= disk_write_block(d, 0x8076, 2, buf) != -EINVARG;
    bad |= disk_write_block(d, 0x8077 - 256, 256, buf) != 0;
    if (bad)
        return fail("past the end", "not -EINVARG");
    if (count_cmd(0x30) != 1 || dev.lba != 0x8077 - 256)
        return fail("past the end", "WRITE sent for a refused request");
    return clean("refused requests");
}

static int c_flush_ok(void)
{
    dev.flush_reads = 1000;
    int res = disk_flush(disk_get(0));
    if (res != 0 || count_cmd(0xE7) != 1)
        return fail("flush", "not 0 with one FLUSH");
    return clean("flush");
}
static int c_flush_slow(void)
{
    dev.flush_reads = 5000000; // longer than the normal 1,000,000-read limit
    int res = disk_flush(disk_get(0));
    if (res != 0)
        return fail("slow flush (5M reads)", "gave up");
    return clean("slow flush");
}
static int c_flush_err(void)
{
    dev.flush_reads = 10;
    dev.flush_end = 0x41;
    int res = disk_flush(disk_get(0));
    if (res != -EIO)
        return fail("flush ERR", "not -EIO");
    if (dev.status_reads - dev.event_reads > 100)
        return fail("flush ERR", "kept polling");
    return clean("flush ERR");
}
static int c_flush_df(void) { dev.flush_reads = 10; dev.flush_end = 0x60; return disk_flush(disk_get(0)) == -EIO ? clean("flush DF") : fail("flush DF", "not -EIO"); }
static int c_flush_drq(void) { dev.flush_reads = 10; dev.flush_end = 0x58; return disk_flush(disk_get(0)) == -EIO ? clean("flush DRQ") : fail("flush DRQ", "not -EIO"); }
static int c_flush_stuck(void)
{
    dev.flush_reads = -1;
    int res = disk_flush(disk_get(0));
    if (res != -EIO)
        return fail("flush BSY forever", "not -EIO");
    if (dev.status_reads > FLUSH_LIMIT)
        return fail("flush BSY forever", "too many status reads");
    printf("    (flush BSY forever: gave up after %ld status reads)\n", dev.status_reads);
    return clean("flush BSY forever");
}
static int c_write_then_read_ok(void)
{
    // After a failed write the next command is accepted (ERR from the last command is not a block).
    dev.fail_sector = 0;
    dev.fail_status = 0x41;
    fill(10, 1);
    disk_write_block(disk_get(0), 10, 1, buf);
    dev.fail_sector = -1;
    dev.phase = IDLE;
    dev.idle_status = 0x41; // DRDY|ERR left from the failed command
    dev.words_out = 0;
    fill(20, 1);
    int res = disk_write_block(disk_get(0), 20, 1, buf);
    if (res != 0 || !data_ok(20, 1))
        return fail("write after a failed one", "failed");
    return clean("write after a failed one");
}

struct test_case { const char *name; int (*run)(void); };
static const struct test_case cases[] = {
    { "id qemu", c_id_qemu }, { "id no flush", c_id_noflush }, { "id w83 11", c_id_w83_invalid },
    { "id w83 00", c_id_w83_zero }, { "id w0 atapi", c_id_atapi_w0 }, { "id w0 incomplete", c_id_incomplete_w0 },
    { "id no lba", c_id_nolba },
    { "id 0 sectors", c_id_zero }, { "id clamp", c_id_clamp }, { "id max", c_id_max }, { "id 4k", c_id_4k },
    { "id 512 long", c_id_512long }, { "id w106 invalid", c_id_106_invalid }, { "id absent", c_id_absent },
    { "id float", c_id_float }, { "id abort", c_id_abort }, { "id DF", c_id_df }, { "id no DRQ", c_id_nodrq },
    { "id stuck", c_id_stuck }, { "id end ERR", c_id_end_err }, { "id DRDY late", c_id_nodrdy }, { "id words", c_id_words },
    { "id cached", c_id_cached }, { "id fail cached", c_id_fail_cached },
    { "w 1", c_w1 }, { "w 3", c_w3 }, { "w 256", c_w256 }, { "w slow", c_w_slow }, { "w high", c_w_high },
    { "w DRDY late", c_w_drdy }, { "w ERR 2", c_w_err2 }, { "w ERR 0", c_w_err0 }, { "w DF", c_w_df },
    { "w ERR|DRQ", c_w_errdrq }, { "w end ERR", c_w_end_err }, { "w end DF", c_w_end_df },
    { "w end DRQ", c_w_end_drq }, { "w end BSY", c_w_end_bsy }, { "w BSY", c_w_bsy }, { "w no DRQ", c_w_nodrq },
    { "w refused", c_w_refused }, { "flush", c_flush_ok }, { "flush slow", c_flush_slow },
    { "flush ERR", c_flush_err }, { "flush DF", c_flush_df }, { "flush DRQ", c_flush_drq },
    { "flush stuck", c_flush_stuck }, { "write after failure", c_write_then_read_ok },
};

int main(void)
{
    int total = sizeof(cases) / sizeof(cases[0]);
    int failed = 0;
    buf = malloc(257 * 512);
    dev.data = malloc(257 * WORDS * 2);
    disk_search_and_init();
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(120);
            drive_reset();
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
    printf("disk-write host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
