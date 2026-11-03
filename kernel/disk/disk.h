#ifndef DISK_H
#define DISK_H

#include "fs/file.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int RORTHOX_DISK_TYPE;

// Represents a real physical hard disk
#define RORTHOX_DISK_TYPE_REAL 0
struct disk
{
    RORTHOX_DISK_TYPE type;
    int sector_size;

    // The ID of the disk
    int id;

    struct filesystem *filesystem;

    // The private data of our filesystem
    void *fs_private;

    // Filled in by the first write, flush or capacity query (IDENTIFY DEVICE), kept until reboot
    int write_checked;
    // 0 when the disk can be written, else the negative status of the check
    int write_status;
    // Sectors the drive reports for 28-bit commands (0 when it can't be written)
    unsigned int write_sectors;
};

void disk_search_and_init();
struct disk *disk_get(int index);
int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf);
// 0 and the number of sectors when the disk can be written, else -ERDONLY (not an ATA disk with
// LBA, FLUSH CACHE and 512-byte sectors) or -EIO.
int disk_write_capacity(struct disk *idisk, unsigned int *sectors);
// Write 1 to 256 sectors. Returns once the drive has taken them; call disk_flush to make them stay.
int disk_write_block(struct disk *idisk, unsigned int lba, int total, const void *buf);
// FLUSH CACHE: every sector written before it is on the media when this returns 0.
int disk_flush(struct disk *idisk);

#ifdef __cplusplus
}
#endif

#endif // !DISK_h