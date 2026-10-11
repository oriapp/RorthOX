#include "disk.h"
#include "io/io.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"

// Primary ATA channel, legacy ports
#define ATA_DATA 0x1F0
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS 0x1F7 // read
#define ATA_COMMAND 0x1F7 // write
#define ATA_ALT_STATUS 0x3F6 // read, doesn't clear a pending drive interrupt

// Status register bits
#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF 0x20
#define ATA_SR_BSY 0x80

#define ATA_CMD_READ_SECTORS 0x20
// Bits 7 and 5 are always 1, bit 6 selects LBA, bit 4 = 0 is the master drive
#define ATA_DRIVE_MASTER_LBA 0xE0
#define ATA_LBA28_LIMIT 0x10000000u
// A sector count register of 0 means 256 sectors
#define ATA_MAX_SECTORS 256
// Status reads per wait before giving up. A count, not a time: interrupts are off here and
// the PIT may not be set up yet.
#define ATA_POLL_LIMIT 1000000

struct disk disk;

// Status isn't valid for about 400 ns after a drive select, a command or a data block.
// Four alternate status reads is the usual way to wait that long.
static void ata_pause(void)
{
    for (int i = 0; i < 4; i++)
    {
        insb(ATA_ALT_STATUS);
    }
}

// The task file may only be written while BSY and DRQ are both clear. ERR left over from the
// last command doesn't matter here, a new command clears it.
static int ata_wait_idle(void)
{
    for (int i = 0; i < ATA_POLL_LIMIT; i++)
    {
        unsigned char status = insb(ATA_STATUS);
        if (!(status & (ATA_SR_BSY | ATA_SR_DRQ)))
        {
            return 0;
        }
    }
    return -EIO;
}

// Wait for the next sector. While BSY is set the other bits mean nothing.
static int ata_wait_data(void)
{
    for (int i = 0; i < ATA_POLL_LIMIT; i++)
    {
        unsigned char status = insb(ATA_STATUS);
        if (status & ATA_SR_BSY)
        {
            continue;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF))
        {
            return -EIO;
        }
        if (status & ATA_SR_DRQ)
        {
            return 0;
        }
    }
    return -EIO;
}

// After the last sector the command must end without an error and without more data.
static int ata_wait_done(void)
{
    for (int i = 0; i < ATA_POLL_LIMIT; i++)
    {
        unsigned char status = insb(ATA_STATUS);
        if (status & ATA_SR_BSY)
        {
            continue;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF | ATA_SR_DRQ))
        {
            return -EIO;
        }
        return 0;
    }
    return -EIO;
}

// Read 1 to 256 sectors from the primary master with LBA28 PIO (READ SECTORS).
int disk_read_sector(unsigned int lba, int total, void *buf)
{
    if (!buf || total < 1 || total > ATA_MAX_SECTORS)
    {
        return -EINVARG;
    }

    // The last sector must fit in 28 bits too
    if (lba >= ATA_LBA28_LIMIT || (unsigned int)(total - 1) > ATA_LBA28_LIMIT - 1 - lba)
    {
        return -EINVARG;
    }

    int res = ata_wait_idle();
    if (res < 0)
    {
        return res;
    }

    outb(ATA_DRIVE_HEAD, ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F));
    ata_pause();
    res = ata_wait_idle();
    if (res < 0)
    {
        return res;
    }

    outb(ATA_SECTOR_COUNT, (unsigned char)(total & 0xFF)); // 256 goes out as 0
    outb(ATA_LBA_LOW, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    ata_pause();

    unsigned short *ptr = (unsigned short*) buf;
    for (int b = 0; b < total; b++)
    {
        res = ata_wait_data();
        if (res < 0)
        {
            return res;
        }

        // Copy from hard disk to memory
        for (int i = 0; i < RORTHOX_SECTOR_SIZE / 2; i++)
        {
            *ptr = insw(ATA_DATA); // Reading a word
            ptr++;
        }
        ata_pause();
    }

    return ata_wait_done();
}

void disk_search_and_init()
{
    memset(&disk, 0, sizeof(disk));
    disk.type = RORTHOX_DISK_TYPE_REAL;
    disk.sector_size = RORTHOX_SECTOR_SIZE;
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
    1. call "disk_search_and_init()"
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