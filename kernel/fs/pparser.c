#include "pparser.h"
#include "kernel.h"
#include "string/string.h"
#include "memory/heap/kheap.h"
#include "memory/memory.h"
#include "status.h"

static int pathparser_path_valid_format(const char *filename)
{
    int len = strnlen(filename, RORTHOX_MAX_PATH);
    return len >= 3 && isdigit(filename[0]) && (memcmp((void*)&filename[1], ":/", 2) == 0);
}

static int pathparser_get_drive_by_path(const char **path)
{
    if (!pathparser_path_valid_format(*path))
    {
        return -EBADPATH;
    }

    int drive_no = tonumericdigit(*path[0]);

    // Add 3 bytes to skip drive number 0:/ 1:/ 2:/
    *path += 3;

    return drive_no;
}

static struct path_root* pathparser_create_root(int drive_number)
{
    struct path_root *path_r = kzalloc(sizeof(struct path_root));
    if (!path_r)
    {
        return 0;
    }
    path_r->drive_no = drive_number;
    path_r->first = 0;
    
    return path_r;
}

static const char *pathparser_get_path_part(const char **path)
{
    if (**path == '/' || **path == 0x00)
    {
        if (**path == '/')
        {
            *path += 1;
        }
        return 0;
    }

    char *result_path_part = kzalloc(RORTHOX_MAX_PATH);
    if (!result_path_part)
    {
        return ERROR(-ENOMEM);
    }
    int i = 0;
    while(**path != '/' && **path != 0x00)
    {
        if (i >= RORTHOX_MAX_PATH - 1)
        {
            kfree(result_path_part);
            return ERROR(-EBADPATH);
        }
        result_path_part[i] = **path;
        *path += 1;
        i++;
    }

    if (**path == '/')
    {
        // Skip the forward slash to avoid problems
        *path += 1;
    }

    return result_path_part;
}

struct path_part *pathparser_parse_path_part(struct path_part *last_part, const char **path)
{
    const char *path_part_str = pathparser_get_path_part(path);
    if (ISERR(path_part_str))
    {
        return ERROR(ERROR_I(path_part_str));
    }
    if (!path_part_str)
    {
        return 0;
    }
    
    struct path_part *part = kzalloc(sizeof(struct path_part));
    if (!part)
    {
        kfree((void*) path_part_str);
        return ERROR(-ENOMEM);
    }
    part->part = path_part_str;
    part->next = 0x00;

    if (last_part)
    {
        last_part->next = part;
    }

    return part;
}

void pathparser_free(struct path_root *root)
{
    if (!root)
    {
        return;
    }
    struct path_part *part = root->first;

    while (part)
    {
        struct path_part *next_part = part->next;
        kfree((void*) part->part);
        kfree(part);
        part = next_part;
    }

    kfree(root);
}

struct path_root *pathparser_parse(const char *path, const char *current_directory_path)
{
    int res = 0;
    const char *tmp_path = path;
    struct path_root *path_root = 0;

    if (strnlen(path, RORTHOX_MAX_PATH + 1) > RORTHOX_MAX_PATH)
    {
        goto out;
    }

    res = pathparser_get_drive_by_path(&tmp_path);
    if (res < 0)
    {
        goto out;
    }

    path_root = pathparser_create_root(res);

    if (!path_root)
    {
        goto out;
    }

    struct path_part *first_part = pathparser_parse_path_part(NULL, &tmp_path);

    if (ISERR(first_part))
    {
        goto out_free;
    }
    if (!first_part)
    {
        goto out;
    }

    path_root->first = first_part;
    struct path_part *part = pathparser_parse_path_part(first_part, &tmp_path);

    while(part)
    {
        if (ISERR(part))
        {
            goto out_free;
        }
        part = pathparser_parse_path_part(part, &tmp_path);
    }
    goto out;

out_free:
    pathparser_free(path_root);
    path_root = 0;
out:
    return path_root;
}

struct path_root *pathparser_parse_extended(const char *path, int allow_root)
{
    if (!path)
    {
        return ERROR(-EINVARG);
    }
    int len = strnlen(path, RORTHOX_FS_PATH_MAX);
    if (len < 3 || len >= RORTHOX_FS_PATH_MAX || memcmp((void *)path, "0:/", 3) != 0 ||
        (len == 3 && !allow_root))
    {
        return ERROR(-EINVARG);
    }

    int count = 0;
    int start = 3;
    for (int i = 3; i < len; i++)
    {
        unsigned char c = path[i];
        if (c < 0x20 || c > 0x7E)
        {
            return ERROR(-EINVARG);
        }
        const char *bad = "\"*:<>?\\|";
        for (int j = 0; bad[j]; j++)
        {
            if (c == bad[j])
            {
                return ERROR(-EINVARG);
            }
        }
        if (c == '/' || i == len - 1)
        {
            int end = c == '/' ? i : i + 1;
            int size = end - start;
            if (size < 1 || size > RORTHOX_FS_COMPONENT_MAX || path[start] == ' ' ||
                path[end - 1] == ' ' || path[end - 1] == '.' || (c == '/' && i == len - 1))
            {
                return ERROR(-EINVARG);
            }
            // Defence in depth: 129 components already exceed the path byte limit.
            if (++count > RORTHOX_FS_COMPONENTS_MAX)
            {
                return ERROR(-EINVARG);
            }
            start = i + 1;
        }
    }

    struct path_root *root = kzalloc(sizeof(*root) + count * sizeof(struct path_part) + len + 1);
    if (!root)
    {
        return ERROR(-ENOMEM);
    }
    struct path_part *parts = (struct path_part *)(root + 1);
    char *copy = (char *)(parts + count);
    memcpy(copy, (void *)path, len + 1);
    root->drive_no = 0;
    root->first = count ? parts : 0;
    char *p = copy + 3;
    for (int i = 0; i < count; i++)
    {
        parts[i].part = p;
        parts[i].next = i + 1 < count ? &parts[i + 1] : 0;
        while (*p && *p != '/')
        {
            p++;
        }
        *p++ = 0;
    }
    return root;
}

void pathparser_free_extended(struct path_root *root)
{
    kfree(root);
}

/*
Test:
    1. gdb
    2. add-symbol-file ../build/fs/kernelfull.o 0x100000
    3. break kernel.c:104
    4. target remote | qemu-system-i386 -hda ./os.bin -S -gdb stdio

    5. c - to continue
    6. next
    7. print root_path
    7.1. print *root_path
    7.2. print *root_path->first
    7.3. print *root_path->next

Code Usage:
    struct path_root *root_path = pathparser_parse("0:/bin/shell.exe", NULL);
*/