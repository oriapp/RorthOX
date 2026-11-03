#include "file.h"
#include "config.h"
#include "memory/memory.h"
#include "memory/heap/kheap.h"
#include "string/string.h"
#include "disk/disk.h"
#include "fat/fat16.h"
#include "status.h"
#include "kernel.h"

// Filesystem functionality

struct filesystem *filesystems[RORTHOX_MAX_FILESYSTEMS]; // An array of fs pointers
struct file_descriptor *file_descriptors[RORTHOX_MAX_FILE_DESCRIPTORS];

static struct filesystem **fs_get_free_filesystem()
{
    int i;
    for (i = 0; i < RORTHOX_MAX_FILESYSTEMS; i++)
    {
        if (filesystems[i] == 0)
        {
            return &filesystems[i];
        }
    }

    return 0; // No free fs
}

void fs_insert_filesystem(struct filesystem *filesystem)
{
    // Check if fs == 0, panic system
    struct filesystem **fs;
    fs = fs_get_free_filesystem();
    if (!fs)
    {
        // Panic the system.
        print("Problem inserting filesystem\n");
        while(1) {}
    }

    *fs = filesystem;
}

static void fs_static_load()
{
    fs_insert_filesystem(fat16_init());
}

void fs_load()
{
    memset(filesystems, 0, sizeof(filesystems));
    fs_static_load();
}

void fs_init()
{
    memset(file_descriptors, 0, sizeof(file_descriptors));
    fs_load();
}

static void file_free_descriptor(struct file_descriptor *desc)
{
    file_descriptors[desc->index - 1] = 0;
    kfree(desc);
}

static int file_new_descriptor(struct file_descriptor **desc_out)
{
    int res = -ENOMEM;
    int i;
    for (i = 0; i < RORTHOX_MAX_FILE_DESCRIPTORS; i++)
    {
        if (file_descriptors[i] == 0) // We found something to use
        {
            struct file_descriptor *desc = kzalloc(sizeof(struct file_descriptor));
            if (!desc)
            {
                break;
            }
            // Descriptors start at 1
            desc->index = i + 1;
            file_descriptors[i] = desc;
            *desc_out = desc;
            res = 0; // Everythign went O.K
            break;
        }
    }

    return res;
}

// Return the filedescriptor for the given file descriptor id
static struct file_descriptor *file_get_descriptor(int fd)
{
    if (fd <= 0 || fd > RORTHOX_MAX_FILE_DESCRIPTORS)
    {
        return 0; // invalid fs passed
    }

    // Descriptors start at 1
    int index = fd - 1;

    return file_descriptors[index];
}

struct filesystem *fs_resolve(struct disk *disk)
{
    struct filesystem *fs = 0;
    int i;
    for (i = 0; i < RORTHOX_MAX_FILESYSTEMS; i++)
    {
        if (filesystems[i] != 0 && filesystems[i]->resolve(disk) == 0)
        {
            fs = filesystems[i];
            break;
        }
    }

    return fs;
}

FILE_MODE file_get_mode_by_string(const char *str)
{
    FILE_MODE mode = FILE_MODE_INVALID;
    if (strncmp(str, "r", 1) == 0)
    {
        mode = FILE_MODE_READ;
    }
    else if (strncmp(str, "w", 1) == 0)
    {
        mode = FILE_MODE_WRITE;
    }
    else if (strncmp(str, "a", 1) == 0)
    {
        mode = FILE_MODE_APPEND;
    }
    return mode;
}

// It's the interface of opening a file.
// Its responsibale for locating the correct file system to open a file on.
int fopen_status(const char *filename, const char *mode_str)
{
    int res = 0;
    FILE_MODE mode = file_get_mode_by_string(mode_str);
    if (mode != FILE_MODE_READ && file_validate_path(filename) < 0)
    {
        return -EINVARG;
    }
    struct file_descriptor *desc = 0;
    struct path_root *root_path = pathparser_parse(filename, NULL);
    if (!root_path)
    {
        res = -EINVARG;
        goto out;
    }

    // we cannot just have a root path 0:/ other than 0:/test.txt
    if (!root_path->first)
    {
        res = -EINVARG;
        goto out;
    }

    // Ensure the disk we are reading from exists
    struct disk *disk = disk_get(root_path->drive_no);
    // 1:/test.txt we dont have drive 1 in the system
    if (!disk)
    {
        res = -EIO;
        goto out;
    }

    if (!disk->filesystem)
    {
        res = -EIO;
        goto out;
    }

    if (mode == FILE_MODE_INVALID)
    {
        res = -EINVARG;
        goto out;
    }

    if (mode != FILE_MODE_READ)
    {
        res = file_new_descriptor(&desc);
        if (res < 0)
        {
            goto out;
        }
    }
    void *descriptor_private_data = disk->filesystem->open(disk, root_path->first, mode);
    if (ISERR(descriptor_private_data))
    {
        res = ERROR_I(descriptor_private_data);
        goto out;
    }

    res = desc ? 0 : file_new_descriptor(&desc);
    if (res < 0)
    {
        disk->filesystem->close(descriptor_private_data);
        goto out;
    }

    desc->filesystem = disk->filesystem;
    desc->private = descriptor_private_data;
    desc->disk = disk;
    res = desc->index;

out:
    if (res < 0 && desc)
    {
        file_free_descriptor(desc);
    }
    pathparser_free(root_path);
    return res;
}

int file_validate_path(const char *filename)
{
    if (!filename || strnlen(filename, RORTHOX_MAX_PATH) >= RORTHOX_MAX_PATH ||
        strncmp(filename, "0:/", 3) != 0)
    {
        return -EINVARG;
    }
    const char *p = filename + 3;
    do
    {
        int base = 0;
        int ext = 0;
        int dot = 0;
        while (*p && *p != '/')
        {
            unsigned char c = *p++;
            if (c == '.')
            {
                if (dot || !base)
                {
                    return -EINVARG;
                }
                dot = 1;
                continue;
            }
            if (c <= 0x20 || c >= 0x7F)
            {
                return -EINVARG;
            }
            const char *bad = "\"*+,:;<=>?[\\]|";
            for (int i = 0; bad[i]; i++)
            {
                if (c == bad[i])
                {
                    return -EINVARG;
                }
            }
            if ((!dot && ++base > 8) || (dot && ++ext > 3))
            {
                return -EINVARG;
            }
        }
        if (!base || (dot && !ext))
        {
            return -EINVARG;
        }
        if (!*p)
        {
            return 0;
        }
        p++;
    } while (*p);
    return -EINVARG;
}

int file_open(const char *filename, FILE_MODE mode)
{
    if (mode > FILE_MODE_APPEND || file_validate_path(filename) < 0)
    {
        return -EINVARG;
    }
    const char *modes[] = {"r", "w", "a"};
    int fd = fopen_status(filename, modes[mode]);
    if (fd > 0)
    {
        struct file_stat stat;
        int res = fstat(fd, &stat);
        if (res < 0)
        {
            fclose(fd);
            return res;
        }
    }
    return fd;
}

int file_read(int fd, void *buffer, uint32_t count)
{
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc || !desc->filesystem->read_bytes)
    {
        return -EBADF;
    }
    if (count > 0x7FFFFFFF || (count && !buffer))
    {
        return -EINVARG;
    }
    return desc->filesystem->read_bytes(desc->disk, desc->private, buffer, count);
}

int file_write(int fd, const void *buffer, uint32_t count)
{
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc || !desc->filesystem->write_bytes)
    {
        return -EBADF;
    }
    if (count > 0x7FFFFFFF || (count && !buffer))
    {
        return -EINVARG;
    }
    return desc->filesystem->write_bytes(desc->disk, desc->private, buffer, count);
}

int file_seek(int fd, int offset, FILE_SEEK_MODE whence)
{
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc || !desc->filesystem->seek_signed)
    {
        return -EBADF;
    }
    return desc->filesystem->seek_signed(desc->private, offset, whence);
}

int fopen(const char *filename, const char *mode_str)
{
    int res = fopen_status(filename, mode_str);
    // fopen shouldnt return negative values
    if (res < 0)
        res = 0;
    
    return res;
}

int fstat(int fd, struct file_stat *stat)
{
    int res = 0;
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc)
    {
        res = -EIO;
        goto out;
    }

    res = desc->filesystem->stat(desc->disk, desc->private, stat);

out:
    return res;
}

int fclose(int fd)
{
    int res = 0;
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc)
    {
        res = -EIO;
        goto out;
    }

    res = desc->filesystem->close(desc->private);
    if (res == RORTHOX_ALL_OK)
    {
        file_free_descriptor(desc);
    }

out:
    return res;
}

int fseek(int fd, int offset, FILE_SEEK_MODE whence)
{
    int res = 0;
    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc)
    {
        res = -EIO;
        goto out;
    }

    res = desc->filesystem->seek(desc->private, offset, whence);
    
out:
    return res;
}

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd)
{
    int res = 0;
    if (size == 0 || nmemb == 0 || fd < 1)
    {
        res = -EINVARG;
        goto out;
    }

    struct file_descriptor *desc = file_get_descriptor(fd);
    if (!desc)
    {
        res = -EINVARG;
        goto out;
    }

    res = desc->filesystem->read(desc->disk, desc->private, size, nmemb, (char*) ptr);
out:
    return res;
}



/*
Test


Test GDB:
    1. add-symbol-file ../build/kernelfull.o 0x0100000
    2. target remote | qemu-system-i386 -hda ./os.bin -S -gdb stdio
    3. break fat16_resolve
    4. c
*/