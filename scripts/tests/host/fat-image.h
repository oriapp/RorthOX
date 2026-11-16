/*
 * A writable disk image for host checks that run the real kernel FAT code (kernel/fs/file.c,
 * pparser.c, fat/fat16.c, disk/streamer.c, built with the host C compiler). It takes the place
 * of the kernel's disk driver: link fat-image.c and kalloc.c instead of kernel/disk/disk.c, and
 * disk 0 is an image file held in memory. (fat-host.c is the read-only model with FAT entry
 * changes and seek logs; fat-write.c keeps its own copy of this model with its ordering oracle.)
 *
 * Two layers, like a drive with a write cache:
 *   durable  what is on the media: the image file at open, plus every flushed write;
 *   pending  sectors written since the last successful flush (the latest contents per sector).
 * Reads see pending over durable. disk_flush() makes every pending sector durable, in one step.
 * A failed write or flush changes nothing, and crash cuts never include a failed write (a real
 * drive may have stored part of one: these states are not covered).
 *
 * The ledger records every call that reaches the drive, in order: a successful read or write as
 * one record per sector (a write with the bytes written), a failed read or write as one record
 * for the whole call, and each flush, each with the status it returned. A crash
 * cut rebuilds the media as it would be after a crash at a ledger position: the durable layer
 * at that point plus a chosen part (none, all, a prefix, any subset) of the writes still
 * pending there. Writes since the last flush may reach the media in any order, so every subset
 * is a state a real drive could leave.
 *
 * A view is an array of fimg_sectors() pointers, one per sector, to that sector's 512 bytes.
 * Live and durable views are copies: later writes and flushes don't change them. Each view
 * function reuses its own buffer, so a view stays valid until the next call of the same function
 * (fimg_cut_prefix counts as fimg_cut), fimg_ledger_reset(), fimg_load_view() or fimg_open().
 * The drive refuses requests past LBA28, like disk.c.
 *
 * Not thread-safe, one image per process; use fork() per case (the image and ledger are copied).
 */
#ifndef HOST_FAT_IMAGE_H
#define HOST_FAT_IMAGE_H

#define FIMG_SECTOR 512

// Load an image file as the durable layer (rounded up to whole sectors with zeroes). The drive
// reports that many sectors as its capacity. Clears pending writes, ledger, counters and
// failures. Exits with status 2 if the file can't be read.
void fimg_open(const char *path);
long fimg_sectors(void);
long fimg_file_bytes(void);  // length of the file that was opened (exports have this length)
// The durable bytes, writable: fixtures change them before a mount (no ledger record).
unsigned char *fimg_durable(void);

// A fresh kernel heap (kalloc_init), a fresh descriptor table (fs_init) and disk 0 resolved by
// the filesystems the kernel has. 0, or -EIO if no filesystem takes the disk. The drive's
// write check (IDENTIFY) runs again at the next write, flush or capacity query. The fresh heap
// drops a pending fimg_fail_alloc(): ask for it after the mount.
int fimg_mount(void);

// What disk_write_capacity() reports (default: fimg_sectors(); 0 means the drive is refused with
// -ERDONLY and more than LBA28 reaches is capped, as disk.c's IDENTIFY does). Writes that reach
// past it are refused with -EINVARG, as the ATA driver does. A capacity above fimg_sectors() is allowed, but
// a write past the model's sectors fails with -EIO and is counted in fimg_counts.beyond.
// Nonzero error: disk_write_capacity(), writes and flushes return it (not an ATA drive, IDENTIFY
// failed). Like disk.c's IDENTIFY, both are taken at the first write, flush or capacity query
// after a mount and kept until the next mount: set them before that.
void fimg_set_capacity(unsigned int sectors);
void fimg_set_capacity_error(int error);

// Failure injection: the n-th call from now (n counts from 1) fails with <error>, 0 = none.
// Counted per disk_read_block/disk_write_block/disk_flush call that reaches the drive.
void fimg_fail_read(int n, int error);
void fimg_fail_write(int n, int error);
void fimg_fail_flush(int n, int error);
// The n-th allocation (kzalloc/kmalloc) from now returns NULL (n counts from 1, 0 = none).
void fimg_fail_alloc(int n);

struct fimg_counts
{
    int reads, read_sectors;     // calls (failed ones too) and sectors read by successful calls
    int writes, write_sectors;   // the same for writes
    int flushes;
    int failed;                  // calls that returned an error after reaching the drive
    int beyond;                  // writes inside the capacity but past the model's sectors
};
extern struct fimg_counts fimg_counts;
// Zero the counters (the ledger stays).
void fimg_reset_counts(void);
// Times sector <lba> was read since the last fimg_reset_counts() (or open).
int fimg_read_count(long lba);

#define FIMG_READ 0
#define FIMG_WRITE 1
#define FIMG_FLUSH 2
struct fimg_record
{
    int kind;                    // FIMG_READ, FIMG_WRITE or FIMG_FLUSH
    int status;                  // what the call returned (0 or a negative status)
    long lba;                    // -1 for a flush
    const unsigned char *data;   // a successful write: the 512 bytes written; else NULL
};
// Start a new ledger at the current state: the durable layer becomes the base of every crash
// cut, and the writes still pending are recorded first (as successful writes), so they stay
// pending in the cuts. Only the latest contents of each pending sector are recorded, in the
// order the sectors were first written: cuts after a reset can't give an older pending version of
// a sector or the real write order of those sectors, so reset with nothing pending (after a flush)
// when that matters. Views and records from before are no longer valid.
void fimg_ledger_reset(void);
int fimg_ledger_count(void);
const struct fimg_record *fimg_ledger(int i);
// Successful writes still pending after the first <at> ledger records (a crash there loses or
// keeps any of them).
int fimg_cut_pending(int at);

// Views. Each call reuses one buffer per kind: copy it if you need two of the same kind.
const unsigned char **fimg_view_live(void);     // pending over durable, now
const unsigned char **fimg_view_durable(void);  // durable only, now
// The media after a crash after the first <at> ledger records: the durable layer at that point,
// then the pending writes i (0 = oldest) with keep[i] != 0 in their order. keep NULL = none.
const unsigned char **fimg_cut(int at, const unsigned char *keep);
// The same with the oldest <prefix> pending writes kept.
const unsigned char **fimg_cut_prefix(int at, int prefix);

// Write a view to an image file of fimg_file_bytes() bytes, for mtools and fsck.fat. 0 or -1.
int fimg_export(const unsigned char **view, const char *path);
// Make a view the durable layer (e.g. a crash cut, to mount it and read it with the kernel
// code). Clears pending writes and the ledger; counters stay.
void fimg_load_view(const unsigned char **view);

#endif
