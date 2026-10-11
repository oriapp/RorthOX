/*
 * Shared part of the FAT16 host checks (fat-chain.c, fat-entry.c): disk 0 is a copy of the
 * RorthOX image, read through the real kernel/fs and kernel/disk/streamer.c code. The model disk
 * can change FAT entries on the way (in both FAT copies), serve a cluster number past the end of
 * the image file from a real cluster, and fail a chosen read. It records every sector read and,
 * through the linker option --wrap=diskstreamer_seek, every position fat16.c seeks a stream to.
 */
#ifndef HOST_FAT_HOST_H
#define HOST_FAT_HOST_H

#include <stdint.h>

struct fat_geometry
{
    long sector_size;
    long sectors_per_cluster;
    long reserved_sectors;
    long fat_copies;
    long sectors_per_fat;
    long root_dir_entries;
    long root_sector;
    long data_sector;
    long cluster_bytes;
    long image_bytes;
};

extern struct fat_geometry geo;

#define FAT_HOST_LOG 65536
extern long seek_log[FAT_HOST_LOG];
extern int seek_count;
extern long read_log[FAT_HOST_LOG];
extern int read_count;
// Reads that were made to fail; for the last one, its index in read_log and seek_count at that time.
extern int failed_reads;
extern int failed_read_index;
extern int failed_seek_count;

// Open the image and read its BPB. Exits on failure.
void fat_host_open_image(const char *path);
// A fresh heap, no changes to the disk, disk 0 resolved as FAT16. Exits on failure.
void fat_host_setup(void);
// Forget the seeks and reads seen so far.
void fat_host_clear_logs(void);

// FAT entry <cluster> reads as <value> in both FAT copies (until the next setup).
void fat_host_set_entry(long cluster, unsigned int value);
// FAT entry <cluster> as the image has it.
unsigned int fat_host_image_entry(long cluster);
// Reads of cluster <virt> get the bytes of cluster <real>.
void fat_host_map_cluster(long virt, long real);
// Fail the disk read that follows the <n>-th seek into the first FAT from now (n counts from 1),
// i.e. the <n>-th FAT entry lookup, with the status <error> (e.g. -ENOMEM).
void fat_host_fail_fat_lookup(int n, int error);

// Byte positions the FAT16 code should use.
long fat_entry_pos(long cluster);    // entry <cluster> in the first FAT
long cluster_pos(long cluster);      // first byte of data cluster <cluster>
long cluster_of_pos(long pos);       // data cluster that holds byte <pos>, -1 if outside the data area

// First cluster and size of a root directory entry with the 8.3 name <name> (e.g. "CHAIN   BIN").
// Returns 0, or -1 if there is no such entry.
int fat_host_root_entry(const char *name83, long *first_cluster, long *size);
// The chain from <first> through the image's FAT (and the changes above). Returns its length,
// or -1 if it has a bad link or is longer than max.
int fat_host_chain(long first, long *chain, int max);

// Read a whole host file into a new buffer (malloc). Exits on failure.
unsigned char *fat_host_read_file(const char *path, long *size);

#endif
