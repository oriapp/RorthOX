#ifndef DISK_H
#define DISK_H

#include "fs/file.h"

typedef unsigned int POTONGOS_DISK_TYPE;

// Represents a real physical hard disk
#define POTONGOS_DISK_TYPE_REAL 0
struct disk
{
    POTONGOS_DISK_TYPE type;
    int sector_size;

    // The ID of the disk
    int id;

    struct filesystem *filesystem;

    // The private data of our filesystem
    void *fs_private;
};

void disk_seRORTH_and_init();
struct disk *disk_get(int index);
int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf);

#endif // !DISK_h