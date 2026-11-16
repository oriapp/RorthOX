/*
 * Writable image disk for the FAT host checks: see fat-image.h.
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "disk/disk.h"
#include "fat-image.h"

void *malloc(unsigned long size);
void free(void *ptr);
void *memset(void *ptr, int c, unsigned long size);
void *memcpy(void *dest, const void *src, unsigned long n);
long lseek(int fd, long offset, int whence);
long write(int fd, const void *buf, unsigned long count);

#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define ATA_MAX_SECTORS 256
#define ATA_LBA28_LIMIT 0x10000000u    // as in kernel/disk/disk.c
#define ATA_LBA28_SECTORS 0x0FFFFFFFu

struct fimg_counts fimg_counts;

static struct disk the_disk;
static unsigned char *durable;      // fimg_sectors() whole sectors
static unsigned char *base;         // durable layer when the ledger started
static long sectors, file_bytes;
static unsigned char **pend;        // pending contents per sector, NULL if none
static long *pend_list;
static long pend_count;
static int *read_counts;
static unsigned int capacity;
static int capacity_error;
static int fail_read_n, fail_write_n, fail_flush_n;
static int fail_read_error, fail_write_error, fail_flush_error;
static struct fimg_record *ledger;
static int ledger_count, ledger_size;
static const unsigned char **views[3];  // live, durable, cut
static unsigned char *snaps[2];         // the sector copies of the live and durable views

static void *must_alloc(unsigned long size)
{
    void *p = malloc(size ? size : 1);
    if (!p)
    {
        printf("fat-image: out of host memory\n");
        exit(2);
    }
    return p;
}

static void ledger_add(int kind, int status, long lba, const void *data)
{
    if (ledger_count == ledger_size)
    {
        ledger_size = ledger_size ? 2 * ledger_size : 4096;
        struct fimg_record *bigger = must_alloc(ledger_size * sizeof(*bigger));
        if (ledger_count)
        {
            memcpy(bigger, ledger, ledger_count * sizeof(*bigger));
        }
        free(ledger);
        ledger = bigger;
    }
    struct fimg_record *r = &ledger[ledger_count++];
    r->kind = kind;
    r->status = status;
    r->lba = lba;
    r->data = 0;
    if (data)
    {
        unsigned char *copy = must_alloc(FIMG_SECTOR);
        memcpy(copy, data, FIMG_SECTOR);
        r->data = copy;
    }
}

static void ledger_clear(void)
{
    for (int i = 0; i < ledger_count; i++)
    {
        free((void *)ledger[i].data);
    }
    ledger_count = 0;
}

static void pend_clear(void)
{
    for (long i = 0; i < pend_count; i++)
    {
        free(pend[pend_list[i]]);
        pend[pend_list[i]] = 0;
    }
    pend_count = 0;
}

void fimg_ledger_reset(void)
{
    ledger_clear();
    memcpy(base, durable, sectors * FIMG_SECTOR);
    for (long i = 0; i < pend_count; i++)
    {
        ledger_add(FIMG_WRITE, 0, pend_list[i], pend[pend_list[i]]);
    }
}

void fimg_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    long n = fd < 0 ? -1 : lseek(fd, 0, 2);
    if (n <= 0)
    {
        printf("fat-image: cannot read %s\n", path);
        exit(2);
    }
    if (durable)
    {
        pend_clear();
        ledger_clear();
        free(durable);
        free(base);
        free(pend);
        free(pend_list);
        free(read_counts);
        for (int k = 0; k < 3; k++)
        {
            free(views[k]);
        }
        free(snaps[0]);
        free(snaps[1]);
    }
    file_bytes = n;
    sectors = (n + FIMG_SECTOR - 1) / FIMG_SECTOR;
    durable = must_alloc(sectors * FIMG_SECTOR);
    memset(durable, 0, sectors * FIMG_SECTOR);
    if (pread(fd, durable, n, 0) != n)
    {
        printf("fat-image: cannot read %s\n", path);
        exit(2);
    }
    close(fd);
    base = must_alloc(sectors * FIMG_SECTOR);
    pend = must_alloc(sectors * sizeof(*pend));
    memset(pend, 0, sectors * sizeof(*pend));
    pend_list = must_alloc(sectors * sizeof(*pend_list));
    read_counts = must_alloc(sectors * sizeof(*read_counts));
    for (int k = 0; k < 3; k++)
    {
        views[k] = must_alloc(sectors * sizeof(*views[k]));
    }
    snaps[0] = must_alloc(sectors * FIMG_SECTOR);
    snaps[1] = must_alloc(sectors * FIMG_SECTOR);
    capacity = (unsigned int)sectors;
    capacity_error = 0;
    fail_read_n = fail_write_n = fail_flush_n = 0;
    fimg_reset_counts();
    fimg_ledger_reset();
}

long fimg_sectors(void) { return sectors; }
long fimg_file_bytes(void) { return file_bytes; }
unsigned char *fimg_durable(void) { return durable; }

int fimg_mount(void)
{
    kalloc_init();
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.type = RORTHOX_DISK_TYPE_REAL;
    the_disk.sector_size = RORTHOX_SECTOR_SIZE;
    fs_init();
    the_disk.filesystem = fs_resolve(&the_disk);
    return the_disk.filesystem ? 0 : -EIO;
}

void fimg_set_capacity(unsigned int n) { capacity = n; }
void fimg_set_capacity_error(int error) { capacity_error = error; }

void fimg_fail_read(int n, int error) { fail_read_n = n ? fimg_counts.reads + n : 0; fail_read_error = error; }
void fimg_fail_write(int n, int error) { fail_write_n = n ? fimg_counts.writes + n : 0; fail_write_error = error; }
void fimg_fail_flush(int n, int error) { fail_flush_n = n ? fimg_counts.flushes + n : 0; fail_flush_error = error; }
void fimg_fail_alloc(int n) { kalloc_fail_at(n ? kalloc_calls() + n : 0); }

void fimg_reset_counts(void)
{
    // The failure points count from the calls made so far: move them with the counters.
    fail_read_n = fail_read_n ? fail_read_n - fimg_counts.reads : 0;
    fail_write_n = fail_write_n ? fail_write_n - fimg_counts.writes : 0;
    fail_flush_n = fail_flush_n ? fail_flush_n - fimg_counts.flushes : 0;
    memset(&fimg_counts, 0, sizeof(fimg_counts));
    memset(read_counts, 0, sectors * sizeof(*read_counts));
}

int fimg_read_count(long lba) { return lba >= 0 && lba < sectors ? read_counts[lba] : 0; }

// file.c reports each filesystem it inserts; the host checks don't need the text.
void print(const char *str) {}

// ---------------------------------------------------------------------------------------------
// The kernel's disk interface (kernel/disk/disk.h)

struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

static const unsigned char *live_sector(long lba)
{
    return pend[lba] ? pend[lba] : durable + lba * FIMG_SECTOR;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (!buf || total < 1 || total > ATA_MAX_SECTORS)
    {
        return -EINVARG;
    }
    if (lba >= ATA_LBA28_LIMIT || (unsigned int)(total - 1) > ATA_LBA28_LIMIT - 1 - lba)
    {
        return -EINVARG;
    }
    fimg_counts.reads++;
    int status = (long)lba + total > sectors ? -EIO : 0;
    if (fimg_counts.reads == fail_read_n)
    {
        status = fail_read_error;
    }
    if (status)
    {
        fimg_counts.failed++;
        ledger_add(FIMG_READ, status, lba, 0);
        return status;
    }
    for (int i = 0; i < total; i++)
    {
        long s = (long)lba + i;
        memcpy((unsigned char *)buf + i * FIMG_SECTOR, live_sector(s), FIMG_SECTOR);
        read_counts[s]++;
        fimg_counts.read_sectors++;
        ledger_add(FIMG_READ, 0, s, 0);
    }
    return 0;
}

// The drive is checked once per mount and the answer kept, like disk.c's IDENTIFY (which also
// refuses a drive that reports 0 sectors and caps the count at what LBA28 reaches).
static int check_writable(struct disk *idisk)
{
    if (!idisk->write_checked)
    {
        idisk->write_status = capacity_error ? capacity_error : capacity == 0 ? -ERDONLY : 0;
        idisk->write_sectors = idisk->write_status ? 0 : capacity > ATA_LBA28_SECTORS ? ATA_LBA28_SECTORS : capacity;
        idisk->write_checked = 1;
    }
    return idisk->write_status;
}

int disk_write_capacity(struct disk *idisk, unsigned int *n)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (!n)
    {
        return -EINVARG;
    }
    int res = check_writable(idisk);
    if (res < 0)
    {
        return res;
    }
    *n = idisk->write_sectors;
    return 0;
}

int disk_write_block(struct disk *idisk, unsigned int lba, int total, const void *buf)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    if (!buf || total < 1 || total > ATA_MAX_SECTORS)
    {
        return -EINVARG;
    }
    if (lba >= ATA_LBA28_LIMIT || (unsigned int)(total - 1) > ATA_LBA28_LIMIT - 1 - lba)
    {
        return -EINVARG;
    }
    int res = check_writable(idisk);
    if (res < 0)
    {
        return res;
    }
    if (lba >= idisk->write_sectors || (unsigned int)total > idisk->write_sectors - lba)
    {
        return -EINVARG;
    }
    fimg_counts.writes++;
    int status = 0;
    if ((long)lba + total > sectors)
    {
        fimg_counts.beyond++;
        status = -EIO;
    }
    if (fimg_counts.writes == fail_write_n)
    {
        status = fail_write_error;
    }
    if (status)
    {
        fimg_counts.failed++;
        ledger_add(FIMG_WRITE, status, lba, 0);
        return status;
    }
    for (int i = 0; i < total; i++)
    {
        long s = (long)lba + i;
        const unsigned char *data = (const unsigned char *)buf + i * FIMG_SECTOR;
        if (!pend[s])
        {
            pend[s] = must_alloc(FIMG_SECTOR);
            pend_list[pend_count++] = s;
        }
        memcpy(pend[s], data, FIMG_SECTOR);
        fimg_counts.write_sectors++;
        ledger_add(FIMG_WRITE, 0, s, data);
    }
    return 0;
}

int disk_flush(struct disk *idisk)
{
    if (idisk != &the_disk)
    {
        return -EIO;
    }
    int res = check_writable(idisk);
    if (res < 0)
    {
        return res;
    }
    fimg_counts.flushes++;
    if (fimg_counts.flushes == fail_flush_n)
    {
        fimg_counts.failed++;
        ledger_add(FIMG_FLUSH, fail_flush_error, -1, 0);
        return fail_flush_error;
    }
    for (long i = 0; i < pend_count; i++)
    {
        memcpy(durable + pend_list[i] * FIMG_SECTOR, pend[pend_list[i]], FIMG_SECTOR);
    }
    pend_clear();
    ledger_add(FIMG_FLUSH, 0, -1, 0);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Ledger, views and crash cuts

int fimg_ledger_count(void) { return ledger_count; }
const struct fimg_record *fimg_ledger(int i) { return i >= 0 && i < ledger_count ? &ledger[i] : 0; }

static int ok_write(int i) { return ledger[i].kind == FIMG_WRITE && ledger[i].status == 0; }
static int ok_flush(int i) { return ledger[i].kind == FIMG_FLUSH && ledger[i].status == 0; }

// Ledger index of the first write still pending after <at> records.
static int pending_start(int at)
{
    int start = 0;
    for (int i = 0; i < at && i < ledger_count; i++)
    {
        if (ok_flush(i))
        {
            start = i + 1;
        }
    }
    return start;
}

int fimg_cut_pending(int at)
{
    int n = 0;
    for (int i = pending_start(at); i < at && i < ledger_count; i++)
    {
        n += ok_write(i);
    }
    return n;
}

// A copy of every sector, so later writes and flushes don't change the view (or free its sectors).
static const unsigned char **snapshot(int k, int live)
{
    for (long s = 0; s < sectors; s++)
    {
        unsigned char *copy = snaps[k] + s * FIMG_SECTOR;
        memcpy(copy, live ? live_sector(s) : durable + s * FIMG_SECTOR, FIMG_SECTOR);
        views[k][s] = copy;
    }
    return views[k];
}

const unsigned char **fimg_view_live(void) { return snapshot(0, 1); }
const unsigned char **fimg_view_durable(void) { return snapshot(1, 0); }

const unsigned char **fimg_cut(int at, const unsigned char *keep)
{
    const unsigned char **v = views[2];
    if (at > ledger_count)
    {
        at = ledger_count;
    }
    for (long s = 0; s < sectors; s++)
    {
        v[s] = base + s * FIMG_SECTOR;
    }
    int start = pending_start(at), k = 0;
    for (int i = 0; i < at; i++)
    {
        if (ok_write(i) && (i < start || (keep && keep[k++])))
        {
            v[ledger[i].lba] = ledger[i].data;
        }
    }
    return v;
}

const unsigned char **fimg_cut_prefix(int at, int prefix)
{
    // One flag per pending write record: a sector written many times has many.
    int n = fimg_cut_pending(at);
    unsigned char *keep = must_alloc(n);
    for (int i = 0; i < n; i++)
    {
        keep[i] = i < prefix;
    }
    const unsigned char **v = fimg_cut(at, keep);
    free(keep);
    return v;
}

int fimg_export(const unsigned char **view, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long done = 0;
    while (fd >= 0 && done < file_bytes)
    {
        long n = file_bytes - done < FIMG_SECTOR ? file_bytes - done : FIMG_SECTOR;
        if (write(fd, view[done / FIMG_SECTOR], n) != n)
        {
            break;
        }
        done += n;
    }
    if (fd >= 0)
    {
        close(fd);
    }
    return fd >= 0 && done == file_bytes ? 0 : -1;
}

void fimg_load_view(const unsigned char **view)
{
    unsigned char *next = must_alloc(sectors * FIMG_SECTOR);
    for (long s = 0; s < sectors; s++)
    {
        memcpy(next + s * FIMG_SECTOR, view[s], FIMG_SECTOR);
    }
    pend_clear();
    free(durable);
    durable = next;
    fimg_ledger_reset();
}
