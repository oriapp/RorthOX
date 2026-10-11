/*
 * Model disk for the FAT16 host checks: see fat-host.h.
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "disk/disk.h"
#include "disk/streamer.h"
#include "fat-host.h"

void *malloc(unsigned long size);
void *memset(void *ptr, int c, unsigned long size);
long lseek(int fd, long offset, int whence);

struct fat_geometry geo;
long seek_log[FAT_HOST_LOG];
int seek_count;
long read_log[FAT_HOST_LOG];
int read_count;
int failed_reads;
int failed_read_index;
int failed_seek_count;
int failed_alloc_calls;
long setup_max_lba;
void (*fat_host_before_resolve)(void);

static int image_fd = -1;
static struct disk the_disk;

#define MAX_CHANGES 16
static struct
{
    long cluster;
    unsigned int value;
} entry_changes[MAX_CHANGES];
static int entry_change_count;
static struct
{
    long virt, real;
} cluster_maps[MAX_CHANGES];
static int cluster_map_count;
static int fail_lookup_at, fat_lookups, fail_next_read, fail_error;
static int fail_read_at, read_calls, fail_read_error;
static long fail_lba = -1;
static int fail_lba_error;
static long max_lba_seen;

static unsigned int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

void fat_host_open_image(const char *path)
{
    unsigned char boot[512];
    image_fd = open(path, O_RDONLY);
    if (image_fd < 0 || pread(image_fd, boot, sizeof(boot), 0) != sizeof(boot) || boot[38] != 0x29)
    {
        printf("fat-host: %s is not a FAT16 image\n", path);
        exit(2);
    }
    geo.sector_size = le16(boot + 11);
    geo.sectors_per_cluster = boot[13];
    geo.reserved_sectors = le16(boot + 14);
    geo.fat_copies = boot[16];
    geo.root_dir_entries = le16(boot + 17);
    geo.sectors_per_fat = le16(boot + 22);
    geo.root_sector = geo.reserved_sectors + geo.fat_copies * geo.sectors_per_fat;
    geo.data_sector = geo.root_sector + (geo.root_dir_entries * 32 + geo.sector_size - 1) / geo.sector_size;
    geo.cluster_bytes = geo.sectors_per_cluster * geo.sector_size;
    geo.image_bytes = lseek(image_fd, 0, 2);
    if (geo.sector_size != RORTHOX_SECTOR_SIZE)
    {
        printf("fat-host: sector size %ld, the kernel uses %d\n", geo.sector_size, RORTHOX_SECTOR_SIZE);
        exit(2);
    }
}

long fat_entry_pos(long cluster)
{
    return geo.reserved_sectors * geo.sector_size + 2 * cluster;
}

long cluster_pos(long cluster)
{
    return (geo.data_sector + (cluster - 2) * geo.sectors_per_cluster) * geo.sector_size;
}

long cluster_of_pos(long pos)
{
    if (pos < geo.data_sector * geo.sector_size)
    {
        return -1;
    }
    return 2 + (pos - geo.data_sector * geo.sector_size) / geo.cluster_bytes;
}

unsigned int fat_host_image_entry(long cluster)
{
    unsigned char b[2];
    if (pread(image_fd, b, 2, fat_entry_pos(cluster)) != 2)
    {
        return 0;
    }
    return le16(b);
}

static unsigned int entry_now(long cluster)
{
    for (int i = entry_change_count - 1; i >= 0; i--)
    {
        if (entry_changes[i].cluster == cluster)
        {
            return entry_changes[i].value;
        }
    }
    return fat_host_image_entry(cluster);
}

void fat_host_set_entry(long cluster, unsigned int value)
{
    if (entry_change_count == MAX_CHANGES)
    {
        printf("fat-host: too many FAT changes\n");
        exit(2);
    }
    entry_changes[entry_change_count].cluster = cluster;
    entry_changes[entry_change_count].value = value;
    entry_change_count++;
}

void fat_host_map_cluster(long virt, long real)
{
    if (cluster_map_count == MAX_CHANGES)
    {
        printf("fat-host: too many cluster maps\n");
        exit(2);
    }
    cluster_maps[cluster_map_count].virt = virt;
    cluster_maps[cluster_map_count].real = real;
    cluster_map_count++;
}

void fat_host_fail_fat_lookup(int n, int error)
{
    fail_error = error;
    fat_lookups = 0;
    fail_lookup_at = n;
    fail_next_read = 0;
}

void fat_host_fail_read(int n, int error)
{
    fail_read_at = n;
    fail_read_error = error;
    read_calls = 0;
}

void fat_host_fail_sector(long lba, int error)
{
    fail_lba = lba;
    fail_lba_error = error;
}

void fat_host_clear_logs(void)
{
    seek_count = 0;
    read_count = 0;
    failed_reads = 0;
}

// One sector of the model disk.
static int read_one(long lba, unsigned char *out)
{
    long pos = lba * geo.sector_size;
    long cluster = cluster_of_pos(pos);
    for (int i = 0; i < cluster_map_count && cluster >= 0; i++)
    {
        if (cluster_maps[i].virt == cluster)
        {
            pos = cluster_pos(cluster_maps[i].real) + (pos - cluster_pos(cluster));
            break;
        }
    }
    if (pos < 0 || pos >= geo.image_bytes)
    {
        return -EIO;
    }
    memset(out, 0, geo.sector_size);
    if (pread(image_fd, out, geo.sector_size, pos) <= 0)
    {
        return -EIO;
    }
    // Changed FAT entries, in every FAT copy.
    for (int i = 0; i < entry_change_count; i++)
    {
        for (long f = 0; f < geo.fat_copies; f++)
        {
            long at = (geo.reserved_sectors + f * geo.sectors_per_fat) * geo.sector_size + 2 * entry_changes[i].cluster;
            if (at >= pos && at + 1 < pos + geo.sector_size)
            {
                out[at - pos] = entry_changes[i].value & 0xFF;
                out[at - pos + 1] = entry_changes[i].value >> 8;
            }
        }
    }
    return 0;
}

struct disk *disk_get(int index)
{
    return index == 0 ? &the_disk : 0;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    if (idisk != &the_disk || total <= 0)
    {
        return -EIO;
    }
    int made_to_fail = 0;
    if (fail_next_read)
    {
        fail_next_read = 0;
        made_to_fail = fail_error;
    }
    else if (fail_read_at && ++read_calls == fail_read_at)
    {
        made_to_fail = fail_read_error;
    }
    else if (fail_lba >= 0 && fail_lba >= lba && fail_lba < (long)lba + total)
    {
        made_to_fail = fail_lba_error;
    }
    if (made_to_fail)
    {
        failed_alloc_calls = kalloc_calls();
        failed_reads++;
        failed_read_index = read_count;
        failed_seek_count = seek_count;
        if (read_count < FAT_HOST_LOG)
        {
            read_log[read_count++] = lba;
        }
        return made_to_fail;
    }
    for (int i = 0; i < total; i++)
    {
        if (read_count < FAT_HOST_LOG)
        {
            read_log[read_count++] = (long)lba + i;
        }
        if ((long)lba + i > max_lba_seen)
        {
            max_lba_seen = (long)lba + i;
        }
        if (read_one((long)lba + i, (unsigned char *)buf + (long)i * geo.sector_size) < 0)
        {
            failed_alloc_calls = kalloc_calls();
            return -EIO;
        }
    }
    return 0;
}

int __real_diskstreamer_seek(struct disk_stream *stream, int pos);
int __wrap_diskstreamer_seek(struct disk_stream *stream, int pos)
{
    if (seek_count < FAT_HOST_LOG)
    {
        seek_log[seek_count++] = pos;
    }
    long fat_start = geo.reserved_sectors * geo.sector_size;
    if (pos >= fat_start && pos < fat_start + geo.sectors_per_fat * geo.sector_size &&
        fail_lookup_at && ++fat_lookups == fail_lookup_at)
    {
        fail_next_read = 1;
    }
    return __real_diskstreamer_seek(stream, pos);
}

void print(const char *str) {}

void fat_host_setup(void)
{
    kalloc_init();
    entry_change_count = 0;
    cluster_map_count = 0;
    fail_lookup_at = 0;
    fail_next_read = 0;
    fail_read_at = 0;
    fail_lba = -1;
    failed_alloc_calls = -1;
    max_lba_seen = -1;
    memset(&the_disk, 0, sizeof(the_disk));
    the_disk.type = RORTHOX_DISK_TYPE_REAL;
    the_disk.sector_size = RORTHOX_SECTOR_SIZE;
    fs_init();
    if (fat_host_before_resolve)
    {
        fat_host_before_resolve();
    }
    the_disk.filesystem = fs_resolve(&the_disk);
    if (!the_disk.filesystem)
    {
        printf("fat-host: the test image did not resolve as FAT16\n");
        exit(2);
    }
    setup_max_lba = max_lba_seen;
    fat_host_clear_logs();
    failed_alloc_calls = -1;
}

int fat_host_root_entry(const char *name83, long *first_cluster, long *size)
{
    unsigned char e[32];
    for (long i = 0; i < geo.root_dir_entries; i++)
    {
        if (pread(image_fd, e, 32, geo.root_sector * geo.sector_size + 32 * i) != 32 || e[0] == 0)
        {
            break;
        }
        int same = e[0] != 0xE5;
        for (int k = 0; k < 11 && same; k++)
        {
            same = e[k] == (unsigned char)name83[k];
        }
        if (same)
        {
            *first_cluster = le16(e + 26);
            *size = e[28] | (e[29] << 8) | (e[30] << 16) | ((long)e[31] << 24);
            return 0;
        }
    }
    return -1;
}

int fat_host_chain(long first, long *chain, int max)
{
    int n = 0;
    long cluster = first;
    while (1)
    {
        if (cluster < 2 || cluster >= 0xFFF0 || n == max)
        {
            return -1;
        }
        chain[n++] = cluster;
        unsigned int next = entry_now(cluster);
        if (next >= 0xFFF8)
        {
            return n;
        }
        cluster = next;
    }
}

unsigned char *fat_host_read_file(const char *path, long *size)
{
    int fd = open(path, O_RDONLY);
    long n = fd < 0 ? -1 : lseek(fd, 0, 2);
    unsigned char *data = n < 0 ? 0 : malloc(n + 1);
    if (!data || pread(fd, data, n, 0) != n)
    {
        printf("fat-host: cannot read %s\n", path);
        exit(2);
    }
    close(fd);
    *size = n;
    return data;
}
