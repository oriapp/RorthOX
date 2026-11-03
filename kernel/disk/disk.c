#include "disk.h"
#include "io/io.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"

// Primary ATA channel, legacy ports
#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1 // read
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
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY 0x80

// Error register: the drive aborted the command
#define ATA_ER_ABRT 0x04

#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_IDENTIFY 0xEC
// Bits 7 and 5 are always 1, bit 6 selects LBA, bit 4 = 0 is the master drive
#define ATA_DRIVE_MASTER_LBA 0xE0
#define ATA_LBA28_LIMIT 0x10000000u
// A sector count register of 0 means 256 sectors
#define ATA_MAX_SECTORS 256
// Status reads per wait before giving up. A count, not a time: interrupts are off here and
// the PIT may not be set up yet.
#define ATA_POLL_LIMIT 1000000
// FLUSH CACHE may take much longer than a sector (ATA allows tens of seconds, and under QEMU it
// waits for the host to sync the image file). About 100 s at 1 us per status read, a few seconds
// under QEMU without KVM; it only runs out on a broken drive.
#define ATA_FLUSH_POLL_LIMIT 100000000
// Highest sector count a 28-bit command can reach (IDENTIFY words 60-61 never report more)
#define ATA_LBA28_SECTORS 0x0FFFFFFFu

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

// After the last sector (or a command without data) the command must end without an error and
// without more data.
static int ata_wait_end(int limit)
{
    for (int i = 0; i < limit; i++)
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

static int ata_wait_done(void)
{
    return ata_wait_end(ATA_POLL_LIMIT);
}

// WRITE SECTORS, FLUSH CACHE and IDENTIFY DEVICE also need DRDY before the command is written.
static int ata_wait_ready(void)
{
    for (int i = 0; i < ATA_POLL_LIMIT; i++)
    {
        unsigned char status = insb(ATA_STATUS);
        if (!(status & (ATA_SR_BSY | ATA_SR_DRQ)) && (status & ATA_SR_DRDY))
        {
            return 0;
        }
    }
    return -EIO;
}

// Select the master drive for a write, flush or identify command and wait until it takes one.
static int ata_select(unsigned int lba)
{
    int res = ata_wait_idle();
    if (res < 0)
    {
        return res;
    }

    outb(ATA_DRIVE_HEAD, ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F));
    ata_pause();
    return ata_wait_ready();
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

// IDENTIFY DEVICE on the primary master. Returns 0 and the number of sectors when the drive can be
// written by this driver: an ATA (not ATAPI) disk with LBA, FLUSH CACHE and 512-byte sectors.
// -ERDONLY when it can't, -EIO when the drive doesn't answer properly.
static int ata_identify(unsigned int *sectors_out)
{
    int res = ata_wait_idle();
    if (res < 0)
    {
        return res;
    }

    outb(ATA_DRIVE_HEAD, ATA_DRIVE_MASTER_LBA);
    ata_pause();

    // No ATA disk: QEMU reads 0 when the channel has no drive (an ATAPI drive also shows 0 after a reset),
    // and a bus without drives floats to 0xFF (that usually already timed out in the idle wait above: -EIO).
    unsigned char status = insb(ATA_STATUS);
    if (status == 0x00 || status == 0xFF)
    {
        return -ERDONLY;
    }

    res = ata_wait_ready();
    if (res < 0)
    {
        return res;
    }

    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_pause();

    res = ata_wait_data();
    if (res < 0)
    {
        // An ATAPI drive (or anything else that isn't an ATA disk) aborts IDENTIFY DEVICE.
        status = insb(ATA_ALT_STATUS);
        if ((status & (ATA_SR_BSY | ATA_SR_DF | ATA_SR_ERR)) == ATA_SR_ERR && (insb(ATA_ERROR) & ATA_ER_ABRT))
        {
            return -ERDONLY;
        }
        return res;
    }

    // All 256 words must be read, or DRQ stays set and the drive ignores the next command.
    unsigned short general = 0, capabilities = 0, lba_low = 0, lba_high = 0;
    unsigned short command_set = 0, sector_size = 0, words_low = 0, words_high = 0;
    for (int i = 0; i < RORTHOX_SECTOR_SIZE / 2; i++)
    {
        unsigned short word = insw(ATA_DATA);
        switch (i)
        {
        case 0: general = word; break;
        case 49: capabilities = word; break;
        case 60: lba_low = word; break;
        case 61: lba_high = word; break;
        case 83: command_set = word; break;
        case 106: sector_size = word; break;
        case 117: words_low = word; break;
        case 118: words_high = word; break;
        }
    }
    ata_pause();
    res = ata_wait_done();
    if (res < 0)
    {
        return res;
    }

    // Word 0 bit 15: 0 for an ATA device. Word 0 bit 2: the rest of the data is incomplete.
    // Word 49 bit 9: LBA supported.
    if ((general & 0x8004) || !(capabilities & 0x0200))
    {
        return -ERDONLY;
    }

    // Words 60-61: sectors reachable with 28-bit commands.
    unsigned int sectors = lba_low | ((unsigned int)lba_high << 16);
    if (sectors == 0)
    {
        return -ERDONLY;
    }
    if (sectors > ATA_LBA28_SECTORS)
    {
        sectors = ATA_LBA28_SECTORS;
    }

    // Word 83 is valid when bits 15:14 are 01; bit 12: FLUSH CACHE supported.
    if ((command_set & 0xC000) != 0x4000 || !(command_set & 0x1000))
    {
        return -ERDONLY;
    }

    // Word 106 is valid when bits 15:14 are 01; bit 12 set: words 117-118 give the logical
    // sector size in words. Otherwise sectors are 512 bytes.
    if ((sector_size & 0xC000) == 0x4000 && (sector_size & 0x1000) &&
        (words_low | ((unsigned int)words_high << 16)) != RORTHOX_SECTOR_SIZE / 2)
    {
        return -ERDONLY;
    }

    *sectors_out = sectors;
    return 0;
}

// Write 1 to 256 sectors to the primary master with LBA28 PIO (WRITE SECTORS). The caller has
// checked the request.
static int ata_write_sectors(unsigned int lba, int total, const void *buf)
{
    int res = ata_select(lba);
    if (res < 0)
    {
        return res;
    }

    outb(ATA_SECTOR_COUNT, (unsigned char)(total & 0xFF)); // 256 goes out as 0
    outb(ATA_LBA_LOW, (unsigned char)(lba & 0xFF));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_pause();

    const unsigned char *ptr = (const unsigned char*) buf;
    for (int b = 0; b < total; b++)
    {
        // The drive asks for each sector with DRQ
        res = ata_wait_data();
        if (res < 0)
        {
            return res;
        }

        // The first byte goes in the low half of each word, like insw reads it
        for (int i = 0; i < RORTHOX_SECTOR_SIZE / 2; i++)
        {
            outw(ATA_DATA, (unsigned short)(ptr[0] | (ptr[1] << 8)));
            ptr += 2;
        }
        ata_pause();
    }

    // The last data word doesn't mean the sector was written: wait until the drive is done.
    return ata_wait_done();
}

// IDENTIFY the drive once and keep the answer, failures too: a write that couldn't be checked
// stays refused until the next boot.
static int disk_check_writable(struct disk *idisk)
{
    if (!idisk->write_checked)
    {
        unsigned int sectors = 0;
        idisk->write_status = ata_identify(&sectors);
        idisk->write_sectors = idisk->write_status == 0 ? sectors : 0;
        idisk->write_checked = 1;
    }
    return idisk->write_status;
}

int disk_write_capacity(struct disk *idisk, unsigned int *sectors)
{
    if (idisk != &disk)
    {
        return -EIO;
    }
    if (!sectors)
    {
        return -EINVARG;
    }

    int res = disk_check_writable(idisk);
    if (res < 0)
    {
        return res;
    }

    *sectors = idisk->write_sectors;
    return 0;
}

int disk_write_block(struct disk *idisk, unsigned int lba, int total, const void *buf)
{
    if (idisk != &disk)
    {
        return -EIO;
    }

    // Refuse a bad request before touching the drive
    if (!buf || total < 1 || total > ATA_MAX_SECTORS)
    {
        return -EINVARG;
    }
    if (lba >= ATA_LBA28_LIMIT || (unsigned int)(total - 1) > ATA_LBA28_LIMIT - 1 - lba)
    {
        return -EINVARG;
    }

    int res = disk_check_writable(idisk);
    if (res < 0)
    {
        return res;
    }

    // Every sector must lie inside the drive (the last one is write_sectors - 1)
    if (lba >= idisk->write_sectors || (unsigned int)total > idisk->write_sectors - lba)
    {
        return -EINVARG;
    }

    return ata_write_sectors(lba, total, buf);
}

int disk_flush(struct disk *idisk)
{
    if (idisk != &disk)
    {
        return -EIO;
    }

    int res = disk_check_writable(idisk);
    if (res < 0)
    {
        return res;
    }

    res = ata_select(0);
    if (res < 0)
    {
        return res;
    }

    // FLUSH CACHE has no data: BSY until the cache is on the media, then ERR/DF tell how it went
    outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    ata_pause();
    return ata_wait_end(ATA_FLUSH_POLL_LIMIT);
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