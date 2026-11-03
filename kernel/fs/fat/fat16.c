#include "fat16.h"
#include "config.h"
#include "string/string.h"
#include "disk/disk.h"
#include "disk/streamer.h"
#include "memory/heap/kheap.h"
#include "memory/memory.h"
#include "status.h"
#include "kernel.h"
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#define RORTHOX_FAT16_SIGNATURE 0x29
#define RORTHOX_FAT16_FAT_ENTRY_SIZE 0x02
#define RORTHOX_FAT16_BAD_SECTOR 0xFFF7
#define RORTHOX_FAT16_EOC_START 0xFFF8
#define RORTHOX_FAT16_UNUSED 0x00

// Will be internally (data structure) used
typedef unsigned int FAT_ITEM_TYPE;
#define FAT_ITEM_TYPE_DIRECTORY 0
#define FAT_ITEM_TYPE_FILE 1

// Fat directory entry attributes bitmask
#define FAT_FILE_READ_ONLY 0x01
#define FAT_FILE_HIDDEN 0x02
#define FAT_FILE_SYSTEM 0x04
#define FAT_FILE_VOLUME_LABEL 0x08
#define FAT_FILE_SUBDIRECTORY 0x10
#define FAT_FILE_ARCHIVED 0x20
#define FAT_FILE_DEVICE 0x40
#define FAT_FILE_RESERVED 0x80

struct fat_header_extended
{
    uint8_t drive_number;
    uint8_t win_nt_bit;
    uint8_t signatur;
    uint32_t volume_id;
    uint8_t volume_id_string[11];
    uint8_t system_id_string[8];
} __attribute__((packed));

struct fat_header
{
    uint8_t short_jmp_ins[3];
    uint8_t oem_identifier[8];
    uint16_t byte_per_secotr;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors; // How many sectors are reserved before our first file allocation table
    uint8_t fat_copies;
    uint16_t root_dir_entries;
    uint16_t number_of_sectors;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t number_of_heads;
    uint32_t hidden_sectors;
    uint32_t sectors_big;
} __attribute__((packed));

// FAT header & entended header

struct fat_h
{
    struct fat_header primary_header;
    union fat_h_e
    {
        struct fat_header_extended extended_header; // is optional
    } shared;
};

// represents a file or a subdirectory
struct fat_directory_item
{
    uint8_t filename[8];
    uint8_t ext[3];    // Extension
    uint8_t attribute; // represents fat_file flags
    uint8_t reserved;
    uint8_t creation_time_tenths_of_a_sec;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access;
    uint16_t high_16_bits_first_cluster; // First cluster of the data for this directory item.
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint16_t low_16_bits_first_cluster;
    uint32_t filesize;
} __attribute__((packed));

_Static_assert(sizeof(struct fat_header) == 36, "FAT header size");
_Static_assert(sizeof(struct fat_header_extended) == 26, "FAT extended header size");
_Static_assert(sizeof(struct fat_directory_item) == 32, "FAT directory entry size");
_Static_assert(offsetof(struct fat_directory_item, attribute) == 11, "FAT attributes");
_Static_assert(offsetof(struct fat_directory_item, low_16_bits_first_cluster) == 26, "FAT cluster");
_Static_assert(offsetof(struct fat_directory_item, filesize) == 28, "FAT size");

struct fat_directory
{
    struct fat_directory_item *item;
    int first_cluster;     // zero for the fixed root directory
    int total;             // raw entry slots represented in item, including deleted slots
    int sector_pos;        // first sector where this fat directory data is
    int ending_sector_pos; // last secotr of where this fat directory is storing other directories and files
};

struct fat_item
{
    union
    {
        struct fat_directory_item *item;
        struct fat_directory *directory;
    };

    FAT_ITEM_TYPE type;
    int entry_pos;
};

struct fat_file_descriptor
{
    struct fat_item *item;
    uint32_t pos; // current position in file
    FILE_MODE mode;
    struct disk *disk;
    struct fat_file_descriptor *next;
};

struct fat_private
{
    struct fat_h header;
    struct fat_directory root_directory;

    // Used to stream data clusters
    struct disk_stream *cluster_read_stream;
    // Used to stream the file allocation table
    struct disk_stream *fat_read_stream;

    // Used in situations where we stream the directory
    struct disk_stream *directory_stream;
    struct fat_file_descriptor *descriptors;
    int write_ready;
    int mutation_disabled;
    int max_writable;
};

struct fat_directory_cursor
{
    struct disk *disk;
    int cluster;
    int sector;
    int slots;
    int index;
    int clusters;
    int loaded_sector;
    char buffer[RORTHOX_SECTOR_SIZE];
};

static int fat16_get_fat_entry(struct disk *disk, int cluster);
static int fat16_cluster_to_sector(struct fat_private *private, int cluster);
static int fat16_get_cluster_for_offset(struct disk *disk, int cluster, int offset);
static int fat16_directory_start(struct disk *disk, int cluster, struct fat_directory_cursor *cursor);
static int fat16_directory_next(struct fat_directory_cursor *cursor, struct fat_directory_item *item, int *pos);
static int fat16_prepare_write(struct disk *disk);
static int fat16_truncate(struct disk *disk, struct fat_item *item);
static struct fat_item *fat16_create(struct disk *disk, struct path_part *path);
static int fat16_check_file_chain(struct disk *disk, struct fat_directory_item *item);
static int fat16_read_bytes(struct disk *disk, void *private, void *out, uint32_t count);
static int fat16_write_bytes(struct disk *disk, void *private, const void *in, uint32_t count);
static int fat16_seek_signed(void *private, int offset, FILE_SEEK_MODE whence);

int fat16_resolve(struct disk *disk);
void *fat16_open(struct disk *disk, struct path_part *path, FILE_MODE mode);
int fat16_read(struct disk *disk, void *descriptor, uint32_t size, uint32_t nmemb, char *out_ptr);
int fat16_seek(void *private, uint32_t offset, FILE_SEEK_MODE seek_mode);
int fat16_stat(struct disk *disk, void *private, struct file_stat *stat);
int fat16_close(void *private);

struct filesystem fat16_fs =
    {
        .resolve = fat16_resolve,
        .open = fat16_open,
        .read = fat16_read,
        .seek = fat16_seek,
        .stat = fat16_stat,
        .close = fat16_close,
        .read_bytes = fat16_read_bytes,
        .write_bytes = fat16_write_bytes,
        .seek_signed = fat16_seek_signed
    };

struct filesystem *fat16_init()
{
    strcpy(fat16_fs.name, "FAT16");
    return &fat16_fs;
}

static int fat16_init_private(struct disk *disk, struct fat_private *private)
{
    memset(private, 0, sizeof(struct fat_private));
    private->cluster_read_stream = diskstreamer_new(disk->id);
    if (!private->cluster_read_stream)
    {
        return -ENOMEM;
    }
    private->fat_read_stream = diskstreamer_new(disk->id);
    if (!private->fat_read_stream)
    {
        return -ENOMEM;
    }
    private->directory_stream = diskstreamer_new(disk->id);
    if (!private->directory_stream)
    {
        return -ENOMEM;
    }
    return 0;
}

int fat16_sector_to_absolute(struct disk *disk, int sector)
{
    return sector * disk->sector_size;
}

static int fat16_cluster_limit(struct disk *disk)
{
    struct fat_private *private = disk->fs_private;
    struct fat_header *h = &private->header.primary_header;
    uint32_t sectors = h->number_of_sectors ? h->number_of_sectors : h->sectors_big;
    uint32_t data = private->root_directory.ending_sector_pos;
    if (!h->sectors_per_cluster || sectors <= data)
    {
        return 0;
    }
    uint32_t count = (sectors - data) / h->sectors_per_cluster;
    uint32_t entries = (uint32_t) h->sectors_per_fat * disk->sector_size / 2;
    if (count < 4085 || count >= 65525 || count + 2 > entries)
    {
        return 0;
    }
    return count + 1;
}

static int fat16_valid_cluster(struct disk *disk, int cluster)
{
    return cluster >= 2 && cluster <= fat16_cluster_limit(disk) &&
           cluster != RORTHOX_FAT16_BAD_SECTOR && cluster < RORTHOX_FAT16_EOC_START;
}

static int fat16_directory_start(struct disk *disk, int cluster, struct fat_directory_cursor *cursor)
{
    struct fat_private *private = disk->fs_private;
    memset(cursor, 0, sizeof(*cursor));
    cursor->disk = disk;
    cursor->cluster = cluster;
    cursor->loaded_sector = -1;
    if (cluster)
    {
        if (!fat16_valid_cluster(disk, cluster))
        {
            return -EIO;
        }
        cursor->sector = fat16_cluster_to_sector(private, cluster);
        cursor->slots = private->header.primary_header.sectors_per_cluster * disk->sector_size / 32;
        cursor->clusters = 1;
    }
    else
    {
        cursor->sector = private->root_directory.sector_pos;
        cursor->slots = private->header.primary_header.root_dir_entries;
    }
    return 0;
}

// Returns one raw slot, zero at the allocated end, or a negative error.
static int fat16_directory_next(struct fat_directory_cursor *cursor, struct fat_directory_item *item, int *pos)
{
    struct disk *disk = cursor->disk;
    struct fat_private *private = disk->fs_private;
    if (cursor->index == cursor->slots)
    {
        if (!cursor->cluster)
        {
            return 0;
        }
        int next = fat16_get_fat_entry(disk, cursor->cluster);
        if (next < 0)
        {
            return next;
        }
        if (next >= RORTHOX_FAT16_EOC_START)
        {
            return 0;
        }
        if (!fat16_valid_cluster(disk, next) || ++cursor->clusters > fat16_cluster_limit(disk) - 1)
        {
            return -EIO;
        }
        cursor->cluster = next;
        cursor->sector = fat16_cluster_to_sector(private, next);
        cursor->index = 0;
    }
    uint64_t location = (uint64_t) cursor->sector * disk->sector_size + cursor->index * 32;
    if (location > INT_MAX - 32)
    {
        return -EIO;
    }
    *pos = location;
    if (!cursor->cluster)
    {
        memcpy(item, &private->root_directory.item[cursor->index], sizeof(*item));
    }
    else
    {
        int sector = *pos / disk->sector_size;
        if (cursor->loaded_sector != sector)
        {
            int res = diskstreamer_seek(private->directory_stream, sector * disk->sector_size);
            if (res < 0)
            {
                return res;
            }
            res = diskstreamer_read(private->directory_stream, cursor->buffer, disk->sector_size);
            if (res < 0)
            {
                return res;
            }
            cursor->loaded_sector = sector;
        }
        memcpy(item, cursor->buffer + *pos % disk->sector_size, sizeof(*item));
    }
    cursor->index++;
    return 1;
}

int fat16_get_total_items_for_directory(struct disk *disk, uint32_t directory_start_sector)
{
    struct fat_private *private = disk->fs_private;
    uint32_t data = private->root_directory.ending_sector_pos;
    int sectors = private->header.primary_header.sectors_per_cluster;
    if (!sectors || directory_start_sector < data || (directory_start_sector - data) % sectors)
    {
        return -EIO;
    }
    uint32_t cluster = 2 + (directory_start_sector - data) / sectors;
    if (!fat16_valid_cluster(disk, cluster))
    {
        return -EIO;
    }
    struct fat_directory_cursor cursor;
    int res = fat16_directory_start(disk, cluster, &cursor);
    if (res < 0)
    {
        return res;
    }
    int total = 0;
    int pos;
    struct fat_directory_item item;
    while ((res = fat16_directory_next(&cursor, &item, &pos)) > 0)
    {
        if (!item.filename[0])
        {
            break;
        }
        if (total == INT_MAX / 32)
        {
            return -EIO;
        }
        total++;
    }
    return res < 0 ? res : total;
}

int fat16_get_root_directory(struct disk *disk, struct fat_private *fat_private, struct fat_directory *directory)
{
    int res = 0;
    struct fat_directory_item *dir = 0x00;

    // calculate root directory position
    struct fat_header *primary_header = &fat_private->header.primary_header;
    int root_dir_sector_pos = (primary_header->fat_copies * primary_header->sectors_per_fat) + primary_header->reserved_sectors;
    int root_dir_entries = fat_private->header.primary_header.root_dir_entries;
    int root_dir_size = (root_dir_entries * sizeof(struct fat_directory_item));
    int total_sectors = root_dir_size / disk->sector_size;
    if (root_dir_size % disk->sector_size)
    {
        total_sectors += 1;
    }

    dir = kzalloc(root_dir_size);
    if (!dir)
    {
        res = -ENOMEM;
        goto out;
    }

    struct disk_stream *stream = fat_private->directory_stream;
    if (diskstreamer_seek(stream, fat16_sector_to_absolute(disk, root_dir_sector_pos)) != RORTHOX_ALL_OK)
    {
        res = -EIO;
        goto out;
    }

    if (diskstreamer_read(stream, dir, root_dir_size) != RORTHOX_ALL_OK)
    {
        res = -EIO;
        goto out;
    }

    int total_items = 0;
    while (total_items < root_dir_entries && dir[total_items].filename[0] != 0x00)
    {
        total_items++;
    }

    directory->item = dir;
    directory->total = total_items;
    directory->sector_pos = root_dir_sector_pos;
    directory->ending_sector_pos = root_dir_sector_pos + total_sectors;

out:
    if (res < 0)
        if (dir)
            kfree(dir);

    return res;
}

int fat16_resolve(struct disk *disk)
{
    int res = 0;
    struct disk_stream *stream = 0;
    struct fat_private *fat_private = kzalloc(sizeof(struct fat_private));
    if (!fat_private)
    {
        res = -ENOMEM;
        goto out;
    }
    res = fat16_init_private(disk, fat_private);
    if (res < 0)
    {
        goto out;
    }

    disk->fs_private = fat_private;

    stream = diskstreamer_new(disk->id);
    if (!stream)
    {
        res = -ENOMEM;
        goto out;
    }

    if (diskstreamer_read(stream, &fat_private->header, sizeof(fat_private->header)) != RORTHOX_ALL_OK)
    {
        res = -EIO;
        goto out;
    }

    // Does it have a valid fat16 signature
    if (fat_private->header.shared.extended_header.signatur != 0x29)
    {
        res = -EFSNOTUS; // we are not the one responsible for handeling whatever filesystem is on this disk
        goto out;
    }

    if (fat16_get_root_directory(disk, fat_private, &fat_private->root_directory) != RORTHOX_ALL_OK)
    {
        res = -EIO;
        goto out;
    }

    disk->filesystem = &fat16_fs;

out:
    if (stream)
    {
        diskstreamer_close(stream);
    }

    if (res < 0)
    {
        if (fat_private)
        {
            kfree(fat_private->root_directory.item);
            if (fat_private->directory_stream)
            {
                diskstreamer_close(fat_private->directory_stream);
            }
            if (fat_private->fat_read_stream)
            {
                diskstreamer_close(fat_private->fat_read_stream);
            }
            if (fat_private->cluster_read_stream)
            {
                diskstreamer_close(fat_private->cluster_read_stream);
            }
            kfree(fat_private);
        }
        disk->fs_private = 0;
        disk->filesystem = 0;
    }

    return res;
}

void fat16_to_proper_string(char **out, const char *in, size_t size)
{
    int i = 0;
    // Remember unsed bytes will be spaces
    // Look for space of a null terminator
    while (*in != 0x00 && *in != 0x20)
    {
        **out = *in;
        *out += 1;
        in += 1;

        // We cant process anymore since we have exceeded the input buffer size
        if (i >= size - 1)
        {
            break;
        }
        i++;
    }

    // replace spaces thingy with null terminator
    **out = 0x00;
}

void fat16_get_full_relative_filename(struct fat_directory_item *item, char *out, int max_len)
{
    memset(out, 0x00, max_len);
    char *out_tmp = out;
    fat16_to_proper_string(&out_tmp, (const char *)item->filename, sizeof(item->filename));
    if (item->ext[0] != 0x00 && item->ext[0] != 0x20)
    {
        *out_tmp++ = '.';
        fat16_to_proper_string(&out_tmp, (const char *)item->ext, sizeof(item->ext));
    }
}

struct fat_directory_item *fat_16_clone_directory_item(struct fat_directory_item *item, int size)
{
    struct fat_directory_item *item_copy = 0;
    if (size < sizeof(struct fat_directory_item))
    {
        return item_copy;
    }
    
    item_copy = kzalloc(size);
    if (!item_copy)
    {
        return 0;
    }

    memcpy(item_copy, item, size);
    return item_copy;
}

static uint32_t fat16_get_first_cluster(struct fat_directory_item *item)
{
    // FAT16 uses only the low word; the high cluster word belongs to FAT32.
    return item->low_16_bits_first_cluster;
}

static int fat16_cluster_to_sector(struct fat_private *private, int cluster)
{
    return private->root_directory.ending_sector_pos + ((cluster - 2) * private->header.primary_header.sectors_per_cluster);
}

static uint32_t fat16_get_first_fat_sector(struct fat_private *private)
{
    //  Very first file allocation table comes directly after the reserved sectors
    return private->header.primary_header.reserved_sectors;
}

static int fat16_get_fat_entry(struct disk *disk, int cluster)  
{
    int res = -1;
    struct fat_private *private = disk->fs_private;
    struct disk_stream *stream = private->fat_read_stream;
    if (!stream)
    {
        goto out;
    }

    uint32_t fat_table_position = fat16_get_first_fat_sector(private) * disk->sector_size;
    res = diskstreamer_seek(stream, fat_table_position + (cluster * RORTHOX_FAT16_FAT_ENTRY_SIZE));
    if (res < 0)
    {
        goto out;
    }

    uint16_t result = 0;
    res = diskstreamer_read(stream, &result, sizeof(result));
    if (res < 0)
    {
        goto out;
    }

    res = result;

out:
    return res;
}

/*
    Get the correct cluster to use based on the starting cluster and the offset
*/
static int fat16_get_cluster_for_offset(struct disk *disk, int starting_cluster, int offset)
{
    struct fat_private *private = disk->fs_private;
    int bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    if (offset < 0 || !bytes || !fat16_valid_cluster(disk, starting_cluster))
    {
        return -EIO;
    }
    int ahead = offset / bytes;
    if (ahead >= fat16_cluster_limit(disk) - 1)
    {
        return -EIO;
    }
    int cluster = starting_cluster;
    for (int i = 0; i < ahead; i++)
    {
        int next = fat16_get_fat_entry(disk, cluster);
        if (next < 0)
        {
            return next;
        }
        if (!fat16_valid_cluster(disk, next))
        {
            return -EIO;
        }
        cluster = next;
    }
    return cluster;
}

static int fat16_read_internal_from_stream(struct disk *disk, struct disk_stream *stream, int cluster, int offset, int total, void *out)
{
    struct fat_private *private = disk->fs_private;
    int bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    if (total < 0 || offset < 0 || total > INT_MAX - offset || !bytes)
    {
        return -EIO;
    }
    int current = fat16_get_cluster_for_offset(disk, cluster, offset);
    if (current < 0)
    {
        return current;
    }
    int within = offset % bytes;
    int traversed = offset / bytes + 1;
    while (total > 0)
    {
        int part = bytes - within;
        if (part > total)
        {
            part = total;
        }
        uint64_t pos = (uint64_t) fat16_cluster_to_sector(private, current) * disk->sector_size + within;
        if (pos > INT_MAX || part > INT_MAX - pos)
        {
            return -EIO;
        }
        int res = diskstreamer_seek(stream, pos);
        if (res < 0)
        {
            return res;
        }
        res = diskstreamer_read(stream, out, part);
        if (res < 0)
        {
            return res;
        }
        total -= part;
        out = (char *) out + part;
        within = 0;
        if (total)
        {
            current = fat16_get_fat_entry(disk, current);
            if (current < 0)
            {
                return current;
            }
            if (!fat16_valid_cluster(disk, current) || ++traversed > fat16_cluster_limit(disk) - 1)
            {
                return -EIO;
            }
        }
    }
    return 0;
}

static int fat16_read_internal(struct disk *disk, int starting_cluster, int offset, int total, void *out)
{
    struct fat_private *fs_private = disk->fs_private;
    struct disk_stream *stream = fs_private->cluster_read_stream;
    return fat16_read_internal_from_stream(disk, stream, starting_cluster, offset, total, out);
}

void fat16_free_directory(struct fat_directory *directory)
{
    if (!directory)
    {
        return;
    }

    if (directory->item)
    {
        kfree(directory->item);
    }

    kfree(directory);
}

void fat16_fat_item_free(struct fat_item *item)
{
    if (item->type == FAT_ITEM_TYPE_DIRECTORY)
    {
        fat16_free_directory(item->directory);
    }
    else if (item->type == FAT_ITEM_TYPE_FILE)
    {
        kfree(item->item);
    }
    // Else panic

    kfree(item);
}

struct fat_directory  *fat16_load_fat_directory(struct disk *disk, struct fat_directory_item *item)
{
    int res = 0;
    struct fat_directory *directory = 0;
    struct fat_private *fat_private = disk->fs_private;
    if (!(item->attribute & FAT_FILE_SUBDIRECTORY))
    {
        res = -EINVARG;
        goto out;
    }

    directory = kzalloc(sizeof(struct fat_directory));
    if (!directory)
    {
        res = -ENOMEM;
        goto out;
    }

    // calculate the cluster
    int cluster = fat16_get_first_cluster(item);
    if (!fat16_valid_cluster(disk, cluster))
    {
        res = -EIO;
        goto out;
    }
    directory->first_cluster = cluster;
    int cluster_sector = fat16_cluster_to_sector(fat_private, cluster);
    int total_items = fat16_get_total_items_for_directory(disk, cluster_sector);
    if (total_items < 0)
    {
        res = total_items;
        goto out;
    }
    if (total_items > 0x7FFFFFFF / (int) sizeof(struct fat_directory_item))
    {
        res = -EIO;
        goto out;
    }
    directory->total = total_items;
    if (total_items == 0)
    {
        goto out;
    }
    int directory_size = directory->total * sizeof(struct fat_directory_item);
    directory->item = kzalloc(directory_size);
    if (!directory->item)
    {
        res = -ENOMEM;
        goto out;
    }

    struct fat_directory_cursor cursor;
    res = fat16_directory_start(disk, cluster, &cursor);
    if (res < 0)
    {
        goto out;
    }
    for (int i = 0; i < total_items; i++)
    {
        int pos;
        res = fat16_directory_next(&cursor, &directory->item[i], &pos);
        if (res <= 0)
        {
            res = -EIO;
            goto out;
        }
    }
    res = 0;

out:
    if (res != RORTHOX_ALL_OK)
    {
        fat16_free_directory(directory);
        directory = 0;
    }
    return directory;
}

struct fat_item *fat16_new_fat_item_for_directory_item(struct disk *disk, struct fat_directory_item *item)
{
    struct fat_item *f_item = kzalloc(sizeof(struct fat_item)); // either a directory or a file
    if (!f_item)
    {
        return 0;
    }

    if (item->attribute & FAT_FILE_SUBDIRECTORY)
    {
        f_item->directory = fat16_load_fat_directory(disk, item);
        if (!f_item->directory)
        {
            kfree(f_item);
            return 0;
        }
        f_item->type = FAT_ITEM_TYPE_DIRECTORY;
        return f_item;
    }

    f_item->type = FAT_ITEM_TYPE_FILE;
    f_item->item = fat_16_clone_directory_item(item, sizeof(struct fat_directory_item));
    if (!f_item->item)
    {
        kfree(f_item);
        return 0;
    }

    return f_item;
}

struct fat_item *fat16_find_item_in_directory(struct disk *disk, struct fat_directory *directory, const char *name)
{
    struct fat_item *f_item = ERROR(-ENOENT);
    char tmp_filename[RORTHOX_MAX_PATH];
    for (int i = 0; i < directory->total; i++)
    {
        if (directory->item[i].filename[0] == 0x00)
        {
            break;
        }
        if (directory->item[i].filename[0] == 0xE5 ||
            (directory->item[i].attribute & FAT_FILE_VOLUME_LABEL))
        {
            continue;
        }
        // resolve the full relative filename for us
        fat16_get_full_relative_filename(&directory->item[i], tmp_filename, sizeof(tmp_filename));
        if (istrncmp(tmp_filename, name, sizeof(tmp_filename)) == 0)
        {
            // Found it let's create a new fat_item
            int pos;
            if (directory->first_cluster)
            {
                struct fat_private *private = disk->fs_private;
                int bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
                int cluster = fat16_get_cluster_for_offset(disk, directory->first_cluster, i * 32);
                if (cluster < 0)
                {
                    return ERROR(cluster);
                }
                uint64_t location = (uint64_t) fat16_cluster_to_sector(private, cluster) * disk->sector_size + (i * 32) % bytes;
                if (location > INT_MAX - 32)
                {
                    return ERROR(-EIO);
                }
                pos = location;
            }
            else
            {
                pos = directory->sector_pos * disk->sector_size + i * 32;
            }
            f_item = fat16_new_fat_item_for_directory_item(disk, &directory->item[i]);
            if (f_item)
            {
                f_item->entry_pos = pos;
            }
            return f_item ? f_item : ERROR(-EIO);
        }
    }

    return f_item;
}

struct fat_item *fat16_get_directory_entry(struct disk *disk, struct path_part *path)
{
    struct fat_private *fat_private = disk->fs_private;
    struct fat_item *current_item = 0;
    struct fat_item *root_item = fat16_find_item_in_directory(disk, &fat_private->root_directory, path->part);
    // 0:/test.txt -> test.txt will be the path part, and will return fat_item for test.txt
    // 0:/abc/test.txt -> fat_item would be root "abc"
    if (ISERR(root_item))
    {
        current_item = root_item;
        goto out;
    }

    struct path_part *next_part = path->next;
    current_item = root_item;
    while(next_part != 0)
    {
        if (current_item->type != FAT_ITEM_TYPE_DIRECTORY)
        {
            fat16_fat_item_free(current_item);
            current_item = ERROR(-EIO);
            break;
        }

        struct fat_item *tmp_item = fat16_find_item_in_directory(disk, current_item->directory, next_part->part);
        fat16_fat_item_free(current_item);
        current_item = tmp_item;
        if (ISERR(current_item))
        {
            break;
        }
        next_part = next_part->next;
    }

out:
    return current_item;
}

void *fat16_open(struct disk *disk, struct path_part *path, FILE_MODE mode)
{
    struct fat_file_descriptor *descriptor = 0;
    int err_code = 0;
    if (mode > FILE_MODE_APPEND)
    {
        return ERROR(-EINVARG);
    }
    if (mode != FILE_MODE_READ)
    {
        err_code = fat16_prepare_write(disk);
        if (err_code < 0)
        {
            goto err_out;
        }
    }

    descriptor = kzalloc(sizeof(struct fat_file_descriptor));
    if (!descriptor)
    {
        err_code = -ENOMEM;
        goto err_out;
    }

    descriptor->item = fat16_get_directory_entry(disk, path);
    if (descriptor->item == ERROR(-ENOENT) && mode != FILE_MODE_READ)
    {
        descriptor->item = fat16_create(disk, path);
    }
    else if (!ISERR(descriptor->item) && mode != FILE_MODE_READ)
    {
        if (descriptor->item->type != FAT_ITEM_TYPE_FILE)
        {
            err_code = -EINVARG;
            goto err_out;
        }
        if (descriptor->item->item->attribute & FAT_FILE_READ_ONLY)
        {
            err_code = -ERDONLY;
            goto err_out;
        }
        err_code = mode == FILE_MODE_WRITE ? fat16_truncate(disk, descriptor->item) :
                   fat16_check_file_chain(disk, descriptor->item->item);
        if (err_code < 0)
        {
            goto err_out;
        }
    }
    if (ISERR(descriptor->item))
    {
        err_code = ERROR_I(descriptor->item);
        goto err_out;
    }

    descriptor->pos = mode == FILE_MODE_APPEND ? descriptor->item->item->filesize : 0;
    descriptor->mode = mode;
    descriptor->disk = disk;
    struct fat_private *private = disk->fs_private;
    descriptor->next = private->descriptors;
    private->descriptors = descriptor;
    return descriptor;
err_out:
    if (descriptor)
    {
        if (descriptor->item && !ISERR(descriptor->item))
        {
            fat16_fat_item_free(descriptor->item);
        }
        kfree(descriptor);
    }
    return ERROR(err_code);
}

static void fat16_free_file_descriptor(struct fat_file_descriptor *desc)
{
    struct fat_private *private = desc->disk->fs_private;
    struct fat_file_descriptor **link = &private->descriptors;
    while (*link && *link != desc)
    {
        link = &(*link)->next;
    }
    if (*link)
    {
        *link = desc->next;
    }
    fat16_fat_item_free(desc->item);
    kfree(desc);
}

int fat16_close(void *private)
{
    fat16_free_file_descriptor((struct fat_file_descriptor*) private);

    return 0;
}

int fat16_stat(struct disk *disk, void *private, struct file_stat *stat)
{
    int res = 0;
    struct fat_file_descriptor *descriptor = (struct fat_file_descriptor*) private;
    struct fat_item *desc_item = descriptor->item;
    if (desc_item->type != FAT_ITEM_TYPE_FILE)
    {
        res = -EINVARG;
        goto out;
    }

    struct fat_directory_item *ritem = desc_item->item;
    stat->filesize = ritem->filesize;
    stat->flags = 0x00;

    if (ritem->attribute & FAT_FILE_READ_ONLY)
    {
        stat->flags |= FILE_STAT_READ_ONLY; // set the flag if is
    }
    
out:
    return res;
}

// Return the amount of nmemb's that where successfully read
int fat16_read(struct disk *disk, void *descriptor, uint32_t size, uint32_t nmemb, char *out_ptr)
{
    struct fat_file_descriptor *fat_desc = descriptor;
    if (fat_desc->item->type != FAT_ITEM_TYPE_FILE)
    {
        return -EINVARG;
    }
    struct fat_directory_item *item = fat_desc->item->item;
    if (fat_desc->pos > item->filesize)
    {
        return -EIO;
    }

    int count = 0;
    for (uint32_t i = 0; i < nmemb; i++)
    {
        uint32_t left = item->filesize - fat_desc->pos;
        if (left == 0)
        {
            break;
        }
        uint32_t bytes = size < left ? size : left;
        if (fat_desc->pos > 0x7FFFFFFF || bytes > 0x7FFFFFFF - fat_desc->pos)
        {
            return -EIO;
        }
        int res = fat16_read_internal(disk, fat16_get_first_cluster(item), fat_desc->pos, bytes, out_ptr);
        if (res < 0)
        {
            // A failed internal read may have copied a prefix; the destination is not rolled back.
            return res;
        }

        fat_desc->pos += bytes;
        out_ptr += bytes;
        if (bytes != size)
        {
            break;
        }
        count++;
    }

    return count;
}

int fat16_seek(void *private, uint32_t offset, FILE_SEEK_MODE seek_mode)
{
    int res = 0;
    struct fat_file_descriptor *desc = private;
    struct fat_item *desc_item = desc->item;
    if (desc->item->type != FAT_ITEM_TYPE_FILE)
    {
        res = -EINVARG;
        goto out;
    }

    struct fat_directory_item *ritem = desc_item->item;
    switch (seek_mode)
    {
        case SEEK_SET:
            if (offset > ritem->filesize)
            {
                res = -EIO;
                goto out;
            }
            desc->pos = offset;
        break;

        case SEEK_END:
            res = -EUNIMP; // un-implemented
        break;

        case SEEK_CUR:
            if (desc->pos > ritem->filesize || offset > ritem->filesize - desc->pos)
            {
                res = -EIO;
                goto out;
            }
            desc->pos += offset;
        break;

        default:
            res = -EINVARG;
        break;
    }

out:
    return res;
}

static int fat16_prepare_write(struct disk *disk)
{
    struct fat_private *private = disk->fs_private;
    if (private->mutation_disabled)
    {
        return -EIO;
    }
    if (private->write_ready)
    {
        return 0;
    }
    unsigned int capacity;
    int res = disk_write_capacity(disk, &capacity);
    if (res < 0)
    {
        return res;
    }
    struct fat_header *h = &private->header.primary_header;
    uint64_t root = (uint64_t) h->reserved_sectors + (uint64_t) h->fat_copies * h->sectors_per_fat;
    uint64_t data = root + ((uint32_t) h->root_dir_entries * 32 + 511) / 512;
    int limit = fat16_cluster_limit(disk);
    if (disk->sector_size != 512 || h->byte_per_secotr != 512 ||
        !h->sectors_per_cluster || (h->sectors_per_cluster & (h->sectors_per_cluster - 1)) ||
        h->fat_copies != 2 || !h->reserved_sectors || !h->sectors_per_fat ||
        !h->root_dir_entries || h->hidden_sectors || !limit ||
        data != private->root_directory.ending_sector_pos || data > capacity || data * 512 > INT_MAX)
    {
        return -EIO;
    }
    uint32_t sectors = capacity;
    if (sectors > (uint32_t) INT_MAX / 512)
    {
        sectors = INT_MAX / 512;
    }
    if (sectors > 0x10000000)
    {
        sectors = 0x10000000;
    }
    uint32_t available = sectors > data ? (sectors - (uint32_t) data) / h->sectors_per_cluster : 0;
    private->max_writable = available + 1 < (uint32_t) limit ? available + 1 : limit;
    char first[512];
    char second[512];
    for (int i = 0; i < h->sectors_per_fat; i++)
    {
        res = disk_read_block(disk, h->reserved_sectors + i, 1, first);
        if (res < 0)
        {
            return res;
        }
        res = disk_read_block(disk, h->reserved_sectors + h->sectors_per_fat + i, 1, second);
        if (res < 0)
        {
            return res;
        }
        if (memcmp(first, second, sizeof(first)) != 0)
        {
            return -EIO;
        }
    }
    private->write_ready = 1;
    return 0;
}

static int fat16_write_at(struct disk *disk, int pos, const void *data, int bytes)
{
    struct fat_private *private = disk->fs_private;
    int res = diskstreamer_seek(private->directory_stream, pos);
    if (res == 0)
    {
        res = diskstreamer_write(private->directory_stream, data, bytes);
    }
    if (res < 0)
    {
        // -EINVARG with the position unmoved: the request was refused before any sector went to the drive
        if (res != -EINVARG || private->directory_stream->pos != pos)
        {
            private->mutation_disabled = 1;
        }
        return -EIO;
    }
    return 0;
}

static int fat16_flush(struct disk *disk)
{
    struct fat_private *private = disk->fs_private;
    if (disk_flush(disk) < 0)
    {
        private->mutation_disabled = 1;
        return -EIO;
    }
    return 0;
}

static int fat16_set_entry(struct disk *disk, int copy, int cluster, uint16_t value)
{
    struct fat_private *private = disk->fs_private;
    struct fat_header *h = &private->header.primary_header;
    int pos = (h->reserved_sectors + copy * h->sectors_per_fat) * disk->sector_size + cluster * 2;
    return fat16_write_at(disk, pos, &value, sizeof(value));
}

static void fat16_publish(struct disk *disk, int pos, struct fat_directory_item *item)
{
    struct fat_private *private = disk->fs_private;
    struct fat_directory *root = &private->root_directory;
    int start = root->sector_pos * disk->sector_size;
    int offset = pos - start;
    if (offset >= 0 && offset < private->header.primary_header.root_dir_entries * 32 && offset % 32 == 0)
    {
        int slot = offset / 32;
        root->item[slot] = *item;
        if (root->total <= slot)
        {
            root->total = slot + 1;
        }
    }
    for (struct fat_file_descriptor *desc = private->descriptors; desc; desc = desc->next)
    {
        if (desc->item->type == FAT_ITEM_TYPE_FILE && desc->item->entry_pos == pos)
        {
            *desc->item->item = *item;
        }
    }
}

static int fat16_commit_item(struct disk *disk, int pos, struct fat_directory_item *item)
{
    int res = fat16_write_at(disk, pos, item, sizeof(*item));
    if (res < 0)
    {
        return res;
    }
    res = fat16_flush(disk);
    if (res == 0)
    {
        fat16_publish(disk, pos, item);
    }
    return res;
}

static void fat16_modified(struct fat_directory_item *item)
{
    item->attribute |= FAT_FILE_ARCHIVED;
    item->last_mod_date = 0x0021;
    item->last_mod_time = 0;
    item->last_access = 0x0021;
}

// Reserve the full list before changing any sector. The bitmap detects loops before reclaim.
static int fat16_collect_chain(struct disk *disk, struct fat_directory_item *item, uint16_t **out, int *count)
{
    struct fat_private *private = disk->fs_private;
    *out = 0;
    *count = 0;
    if (item->high_16_bits_first_cluster)
    {
        return -EIO;
    }
    if (item->filesize > INT_MAX)
    {
        return -EFBIG;
    }
    int capacity = private->max_writable - 1;
    uint16_t *chain = 0;
    unsigned char *seen = 0;
    if (capacity > 0)
    {
        chain = kzalloc(capacity * sizeof(uint16_t) + (private->max_writable + 8) / 8);
        if (!chain)
        {
            return -ENOMEM;
        }
        seen = (unsigned char *) (chain + capacity);
    }
    int current = item->low_16_bits_first_cluster;
    int res = 0;
    while (current)
    {
        if (!fat16_valid_cluster(disk, current) || current > private->max_writable ||
            *count >= capacity || (seen[current / 8] & (1 << (current % 8))))
        {
            res = -EIO;
            goto out;
        }
        seen[current / 8] |= 1 << (current % 8);
        chain[(*count)++] = current;
        int next = fat16_get_fat_entry(disk, current);
        if (next < 0)
        {
            res = next;
            goto out;
        }
        if (next >= RORTHOX_FAT16_EOC_START)
        {
            break;
        }
        if (!fat16_valid_cluster(disk, next))
        {
            res = -EIO;
            goto out;
        }
        current = next;
    }
    uint32_t bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    uint32_t needed = item->filesize / bytes + (item->filesize % bytes != 0);
    if ((uint32_t) *count < needed)
    {
        res = -EIO;
        goto out;
    }
    *out = chain;
    return 0;
out:
    kfree(chain);
    return res;
}

static int fat16_check_file_chain(struct disk *disk, struct fat_directory_item *item)
{
    struct fat_private *private = disk->fs_private;
    uint16_t *chain;
    int count;
    int res = fat16_collect_chain(disk, item, &chain, &count);
    if (res < 0)
    {
        return res;
    }
    uint32_t bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    if ((uint32_t) count != item->filesize / bytes + (item->filesize % bytes != 0))
    {
        res = -EIO;
    }
    kfree(chain);
    return res;
}

static int fat16_truncate(struct disk *disk, struct fat_item *item)
{
    uint16_t *chain;
    int count;
    int res = fat16_collect_chain(disk, item->item, &chain, &count);
    if (res < 0)
    {
        return res;
    }
    struct fat_directory_item empty = *item->item;
    empty.low_16_bits_first_cluster = 0;
    empty.filesize = 0;
    fat16_modified(&empty);
    res = fat16_commit_item(disk, item->entry_pos, &empty);
    if (res < 0)
    {
        goto out;
    }
    *item->item = empty;
    for (int copy = 0; count && copy < 2; copy++)
    {
        for (int i = 0; i < count; i++)
        {
            res = fat16_set_entry(disk, copy, chain[i], 0);
            if (res < 0)
            {
                goto out;
            }
        }
        res = fat16_flush(disk);
        if (res < 0)
        {
            goto out;
        }
    }
out:
    kfree(chain);
    return res;
}

static int fat16_is_lfn(struct fat_directory_item *item)
{
    return item->filename[0] && item->filename[0] != 0xE5 && (item->attribute & 0x3F) == 0x0F;
}

static struct fat_item *fat16_create(struct disk *disk, struct path_part *path)
{
    struct fat_private *private = disk->fs_private;
    struct fat_directory *directory = &private->root_directory;
    struct fat_item *parent = 0;
    struct fat_item *result = 0;
    int res = 0;
    while (path->next)
    {
        struct fat_item *next = fat16_find_item_in_directory(disk, directory, path->part);
        if (parent)
        {
            fat16_fat_item_free(parent);
            parent = 0;
        }
        if (ISERR(next))
        {
            res = ERROR_I(next);
            goto out;
        }
        parent = next;
        if (parent->type != FAT_ITEM_TYPE_DIRECTORY)
        {
            res = -EINVARG;
            goto out;
        }
        directory = parent->directory;
        path = path->next;
    }
    struct fat_directory_cursor cursor;
    res = fat16_directory_start(disk, directory->first_cluster, &cursor);
    if (res < 0)
    {
        goto out;
    }
    struct fat_directory_item entry;
    int pos;
    int slot = -1;
    int previous_lfn = 0;
    while ((res = fat16_directory_next(&cursor, &entry, &pos)) > 0)
    {
        if (!entry.filename[0])
        {
            if (slot < 0 && previous_lfn)
            {
                // Long-name entries with no short entry after them: broken directory
                res = -EIO;
                goto out;
            }
            if (slot < 0)
            {
                slot = pos;
                res = fat16_directory_next(&cursor, &entry, &pos);
                if (res < 0 || (res > 0 && entry.filename[0]))
                {
                    res = -EIO;
                    goto out;
                }
            }
            break;
        }
        if (slot < 0 && entry.filename[0] == 0xE5 && entry.attribute != 0x0F && !previous_lfn)
        {
            slot = pos;
        }
        previous_lfn = fat16_is_lfn(&entry);
    }
    if (res < 0)
    {
        goto out;
    }
    if (slot < 0)
    {
        res = -ENOSPC;
        goto out;
    }
    memset(&entry, 0, sizeof(entry));
    memset(entry.filename, ' ', sizeof(entry.filename));
    memset(entry.ext, ' ', sizeof(entry.ext));
    int base = 0;
    int ext = 0;
    int dot = 0;
    for (const char *p = path->part; *p; p++)
    {
        if (*p == '.')
        {
            dot = 1;
            continue;
        }
        char c = *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
        if ((!dot && base >= 8) || (dot && ext >= 3))
        {
            res = -EINVARG;
            goto out;
        }
        if (dot)
        {
            entry.ext[ext++] = c;
        }
        else
        {
            entry.filename[base++] = c;
        }
    }
    entry.creation_date = 0x0021;
    fat16_modified(&entry);
    result = fat16_new_fat_item_for_directory_item(disk, &entry);
    if (!result)
    {
        res = -ENOMEM;
        goto out;
    }
    result->entry_pos = slot;
    res = fat16_commit_item(disk, slot, &entry);
out:
    if (parent)
    {
        fat16_fat_item_free(parent);
    }
    if (res < 0)
    {
        if (result)
        {
            fat16_fat_item_free(result);
        }
        return ERROR(res);
    }
    return result;
}

static int fat16_allocate(struct disk *disk, uint16_t *chain, int old_count, int needed)
{
    struct fat_private *private = disk->fs_private;
    struct fat_header *h = &private->header.primary_header;
    if (needed > private->max_writable - 1)
    {
        return -ENOSPC;
    }
    uint16_t first[256];
    uint16_t second[256];
    int loaded = -1;
    int count = old_count;
    for (int cluster = 2; count < needed && cluster <= private->max_writable; cluster++)
    {
        int sector = cluster / 256;
        if (sector != loaded)
        {
            int res = disk_read_block(disk, h->reserved_sectors + sector, 1, first);
            if (res < 0)
            {
                return res;
            }
            res = disk_read_block(disk, h->reserved_sectors + h->sectors_per_fat + sector, 1, second);
            if (res < 0)
            {
                return res;
            }
            loaded = sector;
        }
        if (!first[cluster % 256] && !second[cluster % 256])
        {
            chain[count++] = cluster;
        }
    }
    return count == needed ? 0 : -ENOSPC;
}

static int fat16_write_bytes(struct disk *disk, void *private_data, const void *in, uint32_t count)
{
    struct fat_file_descriptor *desc = private_data;
    struct fat_private *private = disk->fs_private;
    if (desc->mode == FILE_MODE_READ)
    {
        return -EBADF;
    }
    if (!count)
    {
        return 0;
    }
    int res = fat16_prepare_write(disk);
    if (res < 0)
    {
        return res;
    }
    struct fat_directory_item item = *desc->item->item;
    uint32_t start = desc->mode == FILE_MODE_APPEND ? item.filesize : desc->pos;
    if (start > item.filesize)
    {
        return -EINVARG;
    }
    if (start > INT_MAX || count > INT_MAX - start)
    {
        return -EFBIG;
    }
    uint32_t end = start + count;
    uint32_t size = end > item.filesize ? end : item.filesize;
    int bytes = private->header.primary_header.sectors_per_cluster * disk->sector_size;
    int needed = size / bytes + (size % bytes != 0);
    uint16_t *chain;
    int old_count;
    res = fat16_collect_chain(disk, &item, &chain, &old_count);
    if (res < 0)
    {
        return res;
    }
    if ((uint32_t) old_count != item.filesize / bytes + (item.filesize % bytes != 0))
    {
        res = -EIO;
        goto out;
    }
    res = fat16_allocate(disk, chain, old_count, needed);
    if (res < 0)
    {
        goto out;
    }
    char zero[512];
    memset(zero, 0, sizeof(zero));
    for (int i = old_count; i < needed; i++)
    {
        int pos = fat16_cluster_to_sector(private, chain[i]) * disk->sector_size;
        for (int offset = 0; offset < bytes; offset += sizeof(zero))
        {
            res = fat16_write_at(disk, pos + offset, zero, sizeof(zero));
            if (res < 0)
            {
                goto out;
            }
        }
    }
    uint32_t position = start;
    uint32_t left = count;
    while (left)
    {
        int offset = position % bytes;
        uint32_t part = bytes - offset;
        if (part > left)
        {
            part = left;
        }
        int pos = fat16_cluster_to_sector(private, chain[position / bytes]) * disk->sector_size + offset;
        res = fat16_write_at(disk, pos, in, part);
        if (res < 0)
        {
            goto out;
        }
        in = (const char *) in + part;
        position += part;
        left -= part;
    }
    res = fat16_flush(disk);
    if (res < 0)
    {
        goto out;
    }
    if (needed > old_count)
    {
        // The detached extension must be durable in both copies before attaching the old tail.
        for (int copy = 0; copy < 2; copy++)
        {
            for (int i = old_count; i < needed; i++)
            {
                res = fat16_set_entry(disk, copy, chain[i], i + 1 < needed ? chain[i + 1] : 0xFFFF);
                if (res < 0)
                {
                    goto out;
                }
            }
            res = fat16_flush(disk);
            if (res < 0)
            {
                goto out;
            }
        }
        if (old_count)
        {
            for (int copy = 0; copy < 2; copy++)
            {
                res = fat16_set_entry(disk, copy, chain[old_count - 1], chain[old_count]);
                if (res < 0)
                {
                    goto out;
                }
                res = fat16_flush(disk);
                if (res < 0)
                {
                    goto out;
                }
            }
        }
    }
    item.low_16_bits_first_cluster = chain[0];
    item.filesize = size;
    fat16_modified(&item);
    res = fat16_commit_item(disk, desc->item->entry_pos, &item);
    if (res == 0)
    {
        desc->pos = end;
        res = count;
    }
out:
    kfree(chain);
    return res;
}

static int fat16_read_bytes(struct disk *disk, void *private, void *out, uint32_t count)
{
    struct fat_file_descriptor *desc = private;
    if (desc->mode != FILE_MODE_READ)
    {
        return -EBADF;
    }
    if (desc->item->type != FAT_ITEM_TYPE_FILE)
    {
        return -EINVARG;
    }
    struct fat_directory_item *item = desc->item->item;
    if (!count || desc->pos >= item->filesize)
    {
        return 0;
    }
    uint32_t left = item->filesize - desc->pos;
    if (count > left)
    {
        count = left;
    }
    if (desc->pos > INT_MAX || count > INT_MAX - desc->pos)
    {
        return -EIO;
    }
    int res = fat16_read_internal(disk, fat16_get_first_cluster(item), desc->pos, count, out);
    if (res < 0)
    {
        return res;
    }
    desc->pos += count;
    return count;
}

static int fat16_seek_signed(void *private, int offset, FILE_SEEK_MODE whence)
{
    struct fat_file_descriptor *desc = private;
    if (desc->item->type != FAT_ITEM_TYPE_FILE)
    {
        return -EINVARG;
    }
    int64_t base;
    switch (whence)
    {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = desc->pos; break;
        case SEEK_END: base = desc->item->item->filesize; break;
        default: return -EINVARG;
    }
    int64_t pos = base + offset;
    if (pos < 0 || pos > INT_MAX || pos > desc->item->item->filesize)
    {
        return -EINVARG;
    }
    desc->pos = pos;
    return pos;
}

/*
Debug:
    1. add-symbol-file ../build/kernelfull.o 0x0100000
    2. target remote | qemu-system-i386 -hda ./os.bin -S -gdb srdio
    3. break fat16_resolve
    4. c
    5. next
    6. print *fat_private
    7. next
    8. print fat_private->header
    9. next
    10. next again. try pressing enter (??)
    11. print fat_private->root_directory
    12. next x 6 time until "return res"
    13. print res
    14. print disk->filesystem
    15. print *disk->filesystem

Resources:
    https://wiki.osdev.org/FAT
    http://www.brokenthorn.com/Resources/OSDev22.html
    http://www.maverick-os.dk/FileSystemFormats/FAT16_FileSystem.html
    https://teslabs.com/openplayer/docs/docs/specs/fat16_specs.pdf

Some more:
    File system development is kind of a branch of os development
*/