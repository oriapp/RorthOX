#ifndef RORTHOX_FILE_H
#define RORTHOX_FILE_H

#include <stdint.h>
#include <stddef.h>
#include "status.h"

#define RORTHOX_FILE_READ 0
#define RORTHOX_FILE_WRITE 1
#define RORTHOX_FILE_APPEND 2
#define RORTHOX_SEEK_SET 0
#define RORTHOX_SEEK_CUR 1
#define RORTHOX_SEEK_END 2
#define RORTHOX_FILE_STAT_READ_ONLY 1
#define RORTHOX_FILE_MAX_OPEN 16

struct rorthox_file_stat
{
    uint32_t flags;
    uint32_t filesize;
};

_Static_assert(sizeof(struct rorthox_file_stat) == 8, "file stat must be 8 bytes");
_Static_assert(offsetof(struct rorthox_file_stat, flags) == 0, "file stat flags must be at 0");
_Static_assert(offsetof(struct rorthox_file_stat, filesize) == 4, "file stat size must be at 4");

// Absolute 0:/ paths. WRITE truncates or creates; APPEND creates or writes at the current EOF.
int rorthox_open(const char *path, int mode);
// Byte counts, at most 4096 per call. Larger requests may return short counts; loop to finish.
int rorthox_read(int fd, void *buffer, unsigned int count);
int rorthox_write(int fd, const void *buffer, unsigned int count);
// Returns the new position, restricted to 0..filesize.
int rorthox_seek(int fd, int offset, int whence);
int rorthox_stat(int fd, struct rorthox_file_stat *out);
int rorthox_close(int fd);

#endif
