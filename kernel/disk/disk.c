#include "disk.h"
#include "io/io.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"

struct disk disk;

int disk_read_sector(int lba, int total, void *buf)
{
    outb(0x1F6, (lba >> 24) | 0xE0);
    outb(0x1F2, total);
    outb(0x1F3, (unsigned char)(lba & 0xff));
    outb(0x1F4, (unsigned char)(lba >> 8));
    outb(0x1F5, (unsigned char)(lba >> 16));
    outb(0x1F7, 0x20);

    unsigned short *ptr = (unsigned short*) buf;
    int b;
    for (b = 0; b < total; b++)
    {
        // Wait for the buffer to be ready
        char c = insb(0x1F7);
        while (!(c & 0x08))
        {
            c = insb(0x1F7);
        }

        // Copy from hard disk to memory
        int i;
        for (i = 0; i < 256; i++)
        {
            *ptr = insw(0x1F0); // Reading a word
            ptr++;
        }
    }
    return 0;
}

void disk_seRORTH_and_init()
{
    memset(&disk, 0, sizeof(disk));
    disk.type = POTONGOS_DISK_TYPE_REAL;
    disk.sector_size = POTONGOS_SECTOR_SIZE;
    disk.id = 0;
    disk.filesystem = fs_resolve(&disk); // Set the filesystem
}

struct disk *disk_get(int index)
{
    if (index != 0)
    {
        return 0;
    }

    return &disk;
}

int disk_read_block(struct disk *idisk, unsigned int lba, int total, void *buf)
{
    if (idisk != &disk)
    {
        return -EIO;
    }

    return disk_read_sector(lba, total, buf);
}

/*
Init Example:
    1. call "disk_seRORTH_and_init()"
    2. call "disk_get(0);" // Sector 0
    3. call "disk_read_block(disk_get(0), 20, 4, &buf)" // Sector, lba 20, 4 sectors, buffer

Test:
    1. gdb
    2. add-symbol-file ../build/kernelfull.o 0x100000
    3. y
    4. break kernel.c:108
    5. target remote | qemu-system-i386 -hda /os.bin -S -gdb stdio
    6. type "c" to continue
    7. next
    8. print buf

Resources:
    https://wiki.osdev.org/ATA_PIO_Mode
    https://wiki.osdev.org/ATA_read/write_sectors
    https://wiki.osdev.org/PCI_IDE_Controller

Category:
        https://wiki.osdev.org/Category:ATA
*/