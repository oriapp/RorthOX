/*
 * Host check for scripts/tests/disk-read.sh: the ATA PIO read in kernel/disk/disk.c against a
 * model of the primary IDE channel. Builds the real disk.c with the host C compiler; insb, insw,
 * outb and outw are the model's ports. Each case runs in its own process.
 *
 * The model is one master drive with a status register that changes as it is read (every read of
 * 0x1F7 or 0x3F6 is one step). It records rule breaks:
 *   - a write to a task file register (0x1F1-0x1F7) while BSY or DRQ is set
 *   - a data read (0x1F0) without DRQ, or past the sectors asked for
 *   - a command with a wrong drive/LBA/count register
 *   - any other port
 * and it stops a case that keeps polling after HARD_CAP status reads (a read that never gives up).
 *
 * Cases: reads of 1, 3, 255 and 256 sectors (256 is count register 0), the last LBA28 sectors,
 * slow sectors (BSY for 50000 reads, with DRQ still set from the last sector), BSY or DRQ before
 * the command, ERR left over from the last command; ERR, ERR with DRQ, DF, DF with DRQ, BSY that
 * never ends, DRQ that never comes, on the first or a later sector or at the end of the command
 * (-EIO, nothing read from the failing sector, done within POLL_LIMIT status reads, and within
 * PROMPT_READS once the drive shows ERR, DF or a DRQ after the last sector); a good read after a
 * failed one; and requests that are refused before any port is touched (0, -1, 257 sectors, LBAs
 * past 28 bits, a NULL buffer, another disk).
 *
 * Prints one line per failing case and "disk-ata host check: <failed> of <cases> cases failed".
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

#define SECTOR_WORDS (RORTHOX_SECTOR_SIZE / 2)
// A wait that never ends is stopped here and reported.
#define HARD_CAP 20000000L
// A failing read must give up within this many status reads.
#define POLL_LIMIT 2000000L
// A slow drive that the driver must wait for.
#define SLOW_READS 50000
// ERR, DF or DRQ left after the last sector must be seen within this many status reads.
#define PROMPT_READS 100

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_DRDY 0x40
#define ST_BSY 0x80

enum phase { PRE, IDLE, BUSY, DATA, DONE, FAILED };

static struct
{
    // How the drive behaves.
    long pre_reads;              // status reads before the drive is ready for a command (-1: never)
    unsigned char pre_status;    // status until then
    unsigned char idle_status;   // status when ready (0x50, or 0x41 after a failed command)
    long busy_reads;             // BSY reads before each sector's DRQ
    unsigned char busy_status;   // 0x80, or 0x88 (BSY with DRQ still set)
    int fail_sector;             // sector of the command that fails (-1: none)
    unsigned char fail_status;   // status from then on
    unsigned char end_status;    // status after the last sector (0x50 when all is well)

    // State.
    enum phase phase;
    long countdown;
    int sector, word, count;
    unsigned int lba;
    unsigned char regs[8];
    int commands;

    // What happened.
    long status_reads, data_words, port_ops, violations;
    long event_reads;            // status_reads when the failure or the end of the command came
    char first_violation[160];
} dev;

static int violation(const char *fmt, unsigned int a, unsigned int b)
{
    if (!dev.violations++)
    {
        snprintf(dev.first_violation, sizeof(dev.first_violation), fmt, a, b);
    }
    return 0;
}

static void drive_reset(void)
{
    memset(&dev, 0, sizeof(dev));
    dev.idle_status = 0x50;
    dev.busy_status = ST_BSY;
    dev.fail_sector = -1;
    dev.end_status = 0x50;
    dev.phase = IDLE;
}

static unsigned short pattern(unsigned int lba, int word)
{
    return (unsigned short)((lba ^ (lba >> 16)) * 31u + word * 7u + 0x1234u);
}

// Start sector dev.sector: busy for a while, then data (or the failure).
static void sector_start(void)
{
    dev.phase = BUSY;
    dev.countdown = dev.busy_reads;
}

static unsigned char status_now(void)
{
    switch (dev.phase)
    {
    case PRE: return dev.pre_status;
    case IDLE: return dev.idle_status;
    case BUSY: return dev.busy_status;
    case DATA: return 0x58;
    case DONE: return dev.end_status;
    case FAILED: return dev.fail_status;
    }
    return 0;
}

// One status read: the drive moves on one step.
static unsigned char status_read(void)
{
    if (++dev.status_reads > HARD_CAP)
    {
        printf("    still polling after %ld status reads (status %02x)\n", HARD_CAP, status_now());
        fflush(0);
        _exit(3);
    }
    if (dev.phase == PRE && dev.pre_reads >= 0 && dev.countdown++ >= dev.pre_reads)
    {
        dev.phase = IDLE;
    }
    else if (dev.phase == BUSY)
    {
        if (dev.countdown > 0)
        {
            dev.countdown--;
        }
        else if (dev.sector == dev.fail_sector)
        {
            dev.phase = FAILED;
            dev.event_reads = dev.status_reads;
        }
        else
        {
            dev.phase = DATA;
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
        return dev.phase == FAILED ? 0x04 : 0x00;
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
    if (port != 0x1F0)
    {
        violation("insw from port %x", port, 0);
        return 0xFFFF;
    }
    if (dev.phase != DATA)
    {
        violation("data read without DRQ (status %02x, word %u)", status_now(), (unsigned int)dev.data_words);
        return 0xFFFF;
    }
    unsigned short value = pattern(dev.lba + dev.sector, dev.word);
    dev.data_words++;
    if (++dev.word == SECTOR_WORDS)
    {
        dev.word = 0;
        if (++dev.sector == dev.count)
        {
            dev.phase = DONE;
            dev.event_reads = dev.status_reads;
        }
        else
        {
            sector_start();
        }
    }
    return value;
}

void outb(unsigned short port, unsigned char val)
{
    dev.port_ops++;
    if (port == 0x3F6)
    {
        return; // device control
    }
    if (port < 0x1F1 || port > 0x1F7)
    {
        violation("outb to port %x value %02x", port, val);
        return;
    }
    unsigned char status = status_now();
    if (status & (ST_BSY | ST_DRQ))
    {
        violation("outb to port %x while status is %02x", port, status);
        return; // the drive ignores it
    }
    dev.regs[port - 0x1F0] = val;
    if (port != 0x1F7)
    {
        return;
    }
    if (val != 0x20)
    {
        violation("command %02x instead of READ SECTORS (20)", val, 0);
        return;
    }
    // READ SECTORS: a new command clears the last command's error.
    dev.commands++;
    if ((dev.regs[6] & 0xF0) != 0xE0)
    {
        violation("drive/head register %02x: not master with LBA", dev.regs[6], 0);
    }
    dev.lba = dev.regs[3] | (dev.regs[4] << 8) | (dev.regs[5] << 16) | ((dev.regs[6] & 0x0F) << 24);
    dev.count = dev.regs[2] ? dev.regs[2] : 256;
    dev.sector = 0;
    dev.word = 0;
    sector_start();
}

void outw(unsigned short port, unsigned short val)
{
    dev.port_ops++;
    violation("outw to port %x value %04x", port, val);
}

// disk.c's other dependencies.
struct filesystem *fs_resolve(struct disk *disk)
{
    return 0;
}

static unsigned char *buffer;
#define BUFFER_SECTORS 260
#define CANARY 0xC5

static void buffer_fill(void)
{
    memset(buffer, CANARY, BUFFER_SECTORS * RORTHOX_SECTOR_SIZE);
}

static int bytes_untouched(int from)
{
    for (int i = from; i < BUFFER_SECTORS * RORTHOX_SECTOR_SIZE; i++)
    {
        if (buffer[i] != CANARY)
        {
            return 0;
        }
    }
    return 1;
}

static int report(const char *what, int ok)
{
    if (dev.violations)
    {
        printf("    %s: %s\n", what, dev.first_violation);
        return 1;
    }
    if (!ok)
    {
        return 1;
    }
    return 0;
}

// A read that must work: right result, every word, the right command, nothing past the buffer.
static int read_ok(const char *what, unsigned int lba, int count)
{
    buffer_fill();
    int res = disk_read_block(disk_get(0), lba, count, buffer);
    int ok = 1;
    if (res != 0)
    {
        printf("    %s: returned %d, expected 0\n", what, res);
        ok = 0;
    }
    else if (dev.commands != 1 || dev.lba != lba || dev.count != count)
    {
        printf("    %s: %d command(s), LBA %#x count %d, expected one for LBA %#x count %d\n",
               what, dev.commands, dev.lba, dev.count, lba, count);
        ok = 0;
    }
    else if (dev.data_words != (long)count * SECTOR_WORDS || dev.phase != DONE)
    {
        printf("    %s: read %ld words, expected %d\n", what, dev.data_words, count * SECTOR_WORDS);
        ok = 0;
    }
    else
    {
        unsigned short *words = (unsigned short *)buffer;
        for (int s = 0; s < count && ok; s++)
        {
            for (int w = 0; w < SECTOR_WORDS; w++)
            {
                if (words[s * SECTOR_WORDS + w] != pattern(lba + s, w))
                {
                    printf("    %s: sector %d word %d wrong in the buffer\n", what, s, w);
                    ok = 0;
                    break;
                }
            }
        }
        if (ok && !bytes_untouched(count * RORTHOX_SECTOR_SIZE))
        {
            printf("    %s: wrote past the sectors it read\n", what);
            ok = 0;
        }
    }
    return report(what, ok);
}

// A read that must fail with -EIO, give up in time and read nothing from the failing sector.
// prompt: the drive shows the failure (ERR, DF, DRQ after the end) without BSY, so the driver
// must stop within PROMPT_READS status reads instead of waiting for its limit.
static int read_fails(const char *what, unsigned int lba, int count, long words_expected, int prompt)
{
    buffer_fill();
    int res = disk_read_block(disk_get(0), lba, count, buffer);
    int ok = 1;
    if (res != -EIO)
    {
        printf("    %s: returned %d, expected %d\n", what, res, -EIO);
        ok = 0;
    }
    else if (dev.status_reads > POLL_LIMIT)
    {
        printf("    %s: gave up only after %ld status reads (limit %ld)\n", what, dev.status_reads, POLL_LIMIT);
        ok = 0;
    }
    else if (prompt && dev.status_reads - dev.event_reads > PROMPT_READS)
    {
        printf("    %s: %ld more status reads after the failure showed (limit %d)\n", what,
               dev.status_reads - dev.event_reads, PROMPT_READS);
        ok = 0;
    }
    else if (dev.data_words != words_expected)
    {
        printf("    %s: read %ld data words, expected %ld\n", what, dev.data_words, words_expected);
        ok = 0;
    }
    else if (!bytes_untouched(words_expected * 2))
    {
        printf("    %s: wrote to the buffer past the sectors that were read\n", what);
        ok = 0;
    }
    return report(what, ok);
}

// A request that must be refused before any port is touched.
static int refused(const char *what, unsigned int lba, int count, int null_buffer, int expected)
{
    buffer_fill();
    int res = disk_read_block(disk_get(0), lba, count, null_buffer ? 0 : buffer);
    int ok = 1;
    if (res >= 0 || (expected && res != expected))
    {
        printf("    %s: returned %d, expected %d\n", what, res, expected ? expected : -EINVARG);
        ok = 0;
    }
    else if (dev.port_ops != 0)
    {
        printf("    %s: touched the ports %ld time(s) before refusing\n", what, dev.port_ops);
        ok = 0;
    }
    else if (!bytes_untouched(0))
    {
        printf("    %s: wrote to the buffer\n", what);
        ok = 0;
    }
    return report(what, ok);
}

static int c_one(void) { return read_ok("1 sector at LBA 1000", 1000, 1); }
static int c_three(void) { return read_ok("3 sectors at LBA 0x5abcdef", 0x5ABCDEF, 3); }
static int c_255(void) { return read_ok("255 sectors", 0x123456, 255); }
static int c_256(void) { return read_ok("256 sectors ending at the last LBA28 sector", 0x0FFFFF00, 256); }
static int c_last(void) { return read_ok("the last LBA28 sector", 0x0FFFFFFF, 1); }

static int c_slow(void)
{
    dev.busy_reads = SLOW_READS;
    dev.busy_status = ST_BSY | ST_DRQ;
    return read_ok("3 slow sectors (BSY with DRQ for 50000 reads each)", 2000, 3);
}

static int c_pre_bsy(void)
{
    dev.phase = PRE;
    dev.pre_reads = SLOW_READS;
    dev.pre_status = ST_BSY | ST_DRQ;
    return read_ok("BSY and DRQ before the command", 3000, 2);
}

static int c_pre_drq(void)
{
    dev.phase = PRE;
    dev.pre_reads = 1000;
    dev.pre_status = ST_DRDY | ST_DRQ;
    return read_ok("DRQ left set before the command", 3000, 1);
}

static int c_stale_err(void)
{
    dev.idle_status = ST_DRDY | ST_ERR;
    return read_ok("ERR left from the last command", 4000, 2);
}

static int c_err(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_DRDY | ST_ERR;
    return read_fails("ERR on the first sector", 5000, 1, 0, 1);
}

static int c_err_drq(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_DRDY | ST_DRQ | ST_ERR;
    return read_fails("ERR with DRQ on the first sector", 5000, 1, 0, 1);
}

static int c_df(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_DRDY | ST_DF;
    return read_fails("DF on the first sector", 5000, 2, 0, 1);
}

static int c_df_drq(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_DRDY | ST_DF | ST_DRQ;
    return read_fails("DF with DRQ on the first sector", 5000, 1, 0, 1);
}

static int c_err_later(void)
{
    dev.fail_sector = 2;
    dev.fail_status = ST_DRDY | ST_ERR;
    return read_fails("ERR on sector 3 of 4", 6000, 4, 2 * SECTOR_WORDS, 1);
}

static int c_bsy_stuck(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_BSY;
    return read_fails("BSY that never ends", 7000, 1, 0, 0);
}

static int c_bsy_drq_stuck(void)
{
    dev.fail_sector = 1;
    dev.fail_status = ST_BSY | ST_DRQ;
    return read_fails("BSY with DRQ that never ends, on sector 2", 7000, 2, SECTOR_WORDS, 0);
}

static int c_no_drq(void)
{
    dev.fail_sector = 0;
    dev.fail_status = 0x50;
    return read_fails("DRQ that never comes", 8000, 1, 0, 0);
}

static int c_end_err(void)
{
    dev.end_status = ST_DRDY | ST_ERR;
    return read_fails("ERR after the last sector", 9000, 2, 2 * SECTOR_WORDS, 1);
}

static int c_end_df(void)
{
    dev.end_status = ST_DRDY | ST_DF;
    return read_fails("DF after the last sector", 9000, 2, 2 * SECTOR_WORDS, 1);
}

static int c_end_drq(void)
{
    dev.end_status = 0x58;
    return read_fails("DRQ still set after the last sector", 9000, 1, SECTOR_WORDS, 1);
}

static int c_end_bsy(void)
{
    dev.end_status = ST_BSY;
    return read_fails("BSY that never ends after the last sector", 9000, 1, SECTOR_WORDS, 0);
}

static int c_pre_stuck(void)
{
    dev.phase = PRE;
    dev.pre_reads = -1;
    dev.pre_status = ST_BSY;
    int res = read_fails("BSY that never ends before the command", 9500, 1, 0, 0);
    if (!res && dev.commands != 0)
    {
        printf("    BSY that never ends before the command: a command was sent\n");
        res = 1;
    }
    return res;
}

static int c_after_failure(void)
{
    dev.fail_sector = 0;
    dev.fail_status = ST_DRDY | ST_ERR;
    if (read_fails("first of two reads, ERR", 10000, 1, 0, 1))
    {
        return 1;
    }
    // The drive stays in its error state (status 0x41) until the next command.
    dev.fail_sector = -1;
    dev.commands = 0;
    dev.data_words = 0;
    dev.status_reads = 0;
    return read_ok("a good read after a failed one", 11000, 2);
}

static int c_refused(void)
{
    int failed = 0;
    failed |= refused("0 sectors", 100, 0, 0, -EINVARG);
    failed |= refused("-1 sectors", 100, -1, 0, -EINVARG);
    failed |= refused("257 sectors", 100, 257, 0, -EINVARG);
    failed |= refused("100000 sectors", 100, 100000, 0, -EINVARG);
    failed |= refused("LBA 0x10000000 (29 bits)", 0x10000000, 1, 0, 0);
    failed |= refused("2 sectors from the last LBA28 sector", 0x0FFFFFFF, 2, 0, 0);
    failed |= refused("256 sectors from LBA 0xfffff01", 0x0FFFFF01, 256, 0, 0);
    failed |= refused("LBA 0xffffffff", 0xFFFFFFFF, 1, 0, 0);
    failed |= refused("a NULL buffer", 100, 1, 1, 0);
    struct disk other;
    buffer_fill();
    if (disk_read_block(&other, 100, 1, buffer) != -EIO || dev.port_ops != 0)
    {
        printf("    another disk: not refused with -EIO before the ports\n");
        failed = 1;
    }
    return failed;
}

struct test_case
{
    const char *name;
    int (*run)(void);
};

static const struct test_case cases[] = {
    { "1 sector", c_one },
    { "3 sectors, LBA bits 24-27", c_three },
    { "255 sectors", c_255 },
    { "256 sectors", c_256 },
    { "last LBA28 sector", c_last },
    { "slow sectors", c_slow },
    { "BSY|DRQ before the command", c_pre_bsy },
    { "DRQ before the command", c_pre_drq },
    { "stale ERR", c_stale_err },
    { "ERR", c_err },
    { "ERR|DRQ", c_err_drq },
    { "DF", c_df },
    { "DF|DRQ", c_df_drq },
    { "ERR on a later sector", c_err_later },
    { "stuck BSY", c_bsy_stuck },
    { "stuck BSY|DRQ", c_bsy_drq_stuck },
    { "no DRQ", c_no_drq },
    { "ERR at the end", c_end_err },
    { "DF at the end", c_end_df },
    { "DRQ at the end", c_end_drq },
    { "BSY at the end", c_end_bsy },
    { "stuck BSY before the command", c_pre_stuck },
    { "good read after a failed one", c_after_failure },
    { "refused requests", c_refused },
};

int main(void)
{
    int total = sizeof(cases) / sizeof(cases[0]);
    int failed = 0;
    buffer = malloc(BUFFER_SECTORS * RORTHOX_SECTOR_SIZE);
    if (!buffer)
    {
        return 2;
    }
    disk_search_and_init();
    for (int i = 0; i < total; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
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
    printf("disk-ata host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
