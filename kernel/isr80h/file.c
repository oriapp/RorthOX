#include "file.h"
#include "kernel.h"
#include "fs/file.h"
#include "task/task.h"
#include "task/process.h"
#include "memory/heap/kheap.h"
#include "status.h"

#define FILE_TRANSFER_SIZE 4096

_Static_assert(RORTHOX_FILE_READ == FILE_MODE_READ && RORTHOX_FILE_WRITE == FILE_MODE_WRITE &&
    RORTHOX_FILE_APPEND == FILE_MODE_APPEND, "file modes must match the public interface");
_Static_assert(RORTHOX_SEEK_SET == SEEK_SET && RORTHOX_SEEK_CUR == SEEK_CUR &&
    RORTHOX_SEEK_END == SEEK_END, "seek origins must match the public interface");
_Static_assert(RORTHOX_FILE_STAT_READ_ONLY == FILE_STAT_READ_ONLY, "file stat flags must match");

void *isr80h_command_file_open(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t path_address = 0;
    uint32_t mode = 0;
    if (task_get_stack_item(task, 0, &path_address) < 0 ||
        task_get_stack_item(task, 1, &mode) < 0 || mode > RORTHOX_FILE_APPEND)
    {
        return ERROR(-EINVARG);
    }

    char path[RORTHOX_MAX_PATH];
    int res = task_copy_path_from_user(task, path_address, path, sizeof(path));
    if (res < 0)
    {
        return ERROR(res);
    }
    return ERROR(process_open_file(task->process, path, mode));
}

// IF=0 and kernel CR3 keep the checked user span stable through the transfer.
static int isr80h_file_transfer(int write)
{
    struct task *task = task_current();
    uint32_t handle = 0;
    uint32_t address = 0;
    uint32_t count = 0;
    if (task_get_stack_item(task, 0, &handle) < 0 ||
        task_get_stack_item(task, 1, &address) < 0 ||
        task_get_stack_item(task, 2, &count) < 0 || count > INT32_MAX)
    {
        return -EINVARG;
    }
    int fd = process_file_descriptor(task->process, (int)handle);
    if (fd < 0)
    {
        return fd;
    }

    // A zero-byte operation checks access mode without touching the buffer or disk.
    int res = write ? file_write(fd, 0, 0) : file_read(fd, 0, 0);
    if (res < 0 || !count)
    {
        return res;
    }
    res = task_validate_user_span(task, address, count, !write);
    if (res < 0)
    {
        return res;
    }

    void *buffer = kmalloc(FILE_TRANSFER_SIZE);
    if (!buffer)
    {
        return -ENOMEM;
    }
    uint32_t chunk = count > FILE_TRANSFER_SIZE ? FILE_TRANSFER_SIZE : count;
    if (write)
    {
        res = task_copy_from_user(task, buffer, address, chunk);
        if (res == 0)
        {
            res = file_write(fd, buffer, chunk);
        }
    }
    else
    {
        res = file_read(fd, buffer, chunk);
        if (res > 0)
        {
            int copied = task_copy_to_user(task, address, buffer, res);
            if (copied < 0)
            {
                res = copied;
            }
        }
    }
    kfree(buffer);
    return res;
}

void *isr80h_command_file_read(struct interrupt_frame *frame)
{
    return ERROR(isr80h_file_transfer(0));
}

void *isr80h_command_file_write(struct interrupt_frame *frame)
{
    return ERROR(isr80h_file_transfer(1));
}

void *isr80h_command_file_seek(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t handle = 0;
    uint32_t offset = 0;
    uint32_t whence = 0;
    if (task_get_stack_item(task, 0, &handle) < 0 ||
        task_get_stack_item(task, 1, &offset) < 0 ||
        task_get_stack_item(task, 2, &whence) < 0)
    {
        return ERROR(-EINVARG);
    }
    int fd = process_file_descriptor(task->process, (int)handle);
    if (fd < 0)
    {
        return ERROR(fd);
    }
    return ERROR(file_seek(fd, (int)offset, whence));
}

void *isr80h_command_file_stat(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t handle = 0;
    uint32_t address = 0;
    if (task_get_stack_item(task, 0, &handle) < 0 ||
        task_get_stack_item(task, 1, &address) < 0)
    {
        return ERROR(-EINVARG);
    }
    int fd = process_file_descriptor(task->process, (int)handle);
    if (fd < 0)
    {
        return ERROR(fd);
    }
    struct rorthox_file_stat out;
    int res = task_validate_user_span(task, address, sizeof(out), 1);
    if (res < 0)
    {
        return ERROR(res);
    }
    struct file_stat stat;
    res = fstat(fd, &stat);
    if (res < 0)
    {
        return ERROR(res);
    }
    out.flags = stat.flags;
    out.filesize = stat.filesize;
    return ERROR(task_copy_to_user(task, address, &out, sizeof(out)));
}

void *isr80h_command_file_close(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t handle = 0;
    if (task_get_stack_item(task, 0, &handle) < 0)
    {
        return ERROR(-EINVARG);
    }
    return ERROR(process_close_file(task->process, (int)handle));
}
