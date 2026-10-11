#include "streamer.h"
#include "memory/heap/kheap.h"
#include "config.h"
#include "status.h"
#include <limits.h>

struct disk_stream *diskstreamer_new(int disk_id)
{
    struct disk *disk = disk_get(disk_id);
    if (!disk)
    {
        return 0;
    }

    struct disk_stream *streamer = kzalloc(sizeof(struct disk_stream));
    if (!streamer)
    {
        return 0;
    }
    streamer->pos = 0;
    streamer->disk = disk;
    return streamer;
}

int diskstreamer_seek(struct disk_stream *stream, int pos)
{
    if (pos < 0)
    {
        return -EINVARG;
    }

    stream->pos = pos;
    return 0;
}

int diskstreamer_read(struct disk_stream *stream, void *out, int total)
{
    if (total < 0 || stream->pos < 0 || total > INT_MAX - stream->pos)
    {
        return -EINVARG;
    }

    char buf[RORTHOX_SECTOR_SIZE];
    while (total > 0)
    {
        int sector = stream->pos / RORTHOX_SECTOR_SIZE;
        int offset = stream->pos % RORTHOX_SECTOR_SIZE;
        int total_to_read = RORTHOX_SECTOR_SIZE - offset;
        if (total_to_read > total)
        {
            total_to_read = total;
        }

        int res = disk_read_block(stream->disk, sector, 1, buf);
        if (res < 0)
        {
            return res;
        }

        for (int i = 0; i < total_to_read; i++)
        {
            *(char*)out++ = buf[offset+i];
        }

        stream->pos += total_to_read;
        total -= total_to_read;
    }

    return 0;
}

void diskstreamer_close(struct disk_stream *stream)
{
    kfree(stream);
}


/*

Tets:
    1. gdb
    2. add-symbol-file ../build/kernelfull.o 0x100000
    3. break kernel.c:106
    4. targer remote | qemu-system-i386 -hda ./os.bin -S -gdb stdio
    5. type - c
    6. next
    7. print stream
    8. next
    9. next
    10. next
    11. print c - res = 184

Usage:
struct disk_stream *stream = diskstreamer_new(0);
diskstreamer_seek(stream, 0x201);
unsigned char c = 0;
diskstreamer_read(stream, &c, 1);
*/