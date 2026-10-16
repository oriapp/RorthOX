/*
 * Stand-ins for the host checks of loading a process. See process-host.h.
 */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "memory/memory.h"
#include "fs/file.h"
#include "memory/paging/paging.h"
#include "task/task.h"
#include "task/process.h"


#define FD 3
#define MAX_FILE (3 * 1024 * 1024)
#define MAX_MAPPINGS 256
#define MAX_BASES 4
#define MAX_DIRECTORIES 64

int host_fail_open_at;
int host_fail_stat_at;
int host_fail_read_at;
int host_fail_map_at;
int host_open_calls;
int host_stat_calls;
int host_read_calls;
int host_map_calls;
int host_failed;
int host_open_files;
int host_paging_problems;
char host_paging_problem[160];
int host_switch_count;
struct paging_4gb_chunk *host_switches[64];
int host_task_return_armed;
jmp_buf host_task_return_jump;
struct registers *host_task_return_regs;
int host_terminated_prints;
struct host_map_record host_map_log[HOST_MAX_MAP_LOG];
int host_map_log_count;

static unsigned char content[MAX_FILE];
static int content_size;
static uint32_t advertised_size;
static uint32_t file_position;
int host_short_read_at;
int host_read_error;
uint32_t host_read_bytes;

void host_file_size(uint32_t size) { advertised_size = size; }

// One mapping made with paging_map_to(): the live allocations it covered at the time.
struct mapping
{
    struct paging_4gb_chunk *directory;
    void *virt;
    void *bases[MAX_BASES];
    int base_count;
};
static struct mapping mappings[MAX_MAPPINGS];
static int mapping_count;
// Directories made by paging_new_4gb() and not freed yet.
static struct paging_4gb_chunk *directories[MAX_DIRECTORIES];
static int directory_count;

void host_file_serve(const void *data, int size)
{
    if (size > MAX_FILE)
    {
        printf("host_file_serve: file too big\n");
        exit(2);
    }
    memcpy(content, (void *)data, size);
    content_size = size;
    advertised_size = size;
}

void host_reset(void)
{
    host_fail_open_at = host_fail_stat_at = host_fail_read_at = host_fail_map_at = 0;
    host_open_calls = host_stat_calls = host_read_calls = host_map_calls = 0;
    host_failed = 0;
    host_short_read_at = 0;
    host_read_error = -EIO;
    host_read_bytes = 0;
    host_map_log_count = 0;
    host_switch_count = 0;
    host_task_return_armed = 0;
    host_task_return_regs = 0;
}

static void problem(const char *text)
{
    if (!host_paging_problems++)
    {
        host_snprintf(host_paging_problem, sizeof(host_paging_problem), "%s", text);
    }
}

// The file layer: one file, served from content[].
int fopen(const char *filename, const char *mode_str)
{
    if (++host_open_calls == host_fail_open_at)
    {
        host_failed = 1;
        return 0;
    }
    host_open_files++;
    file_position = 0;
    return FD;
}

int fstat(int fd, struct file_stat *stat)
{
    if (fd != FD || host_open_files <= 0)
    {
        return -EIO;
    }
    if (++host_stat_calls == host_fail_stat_at)
    {
        host_failed = 1;
        return -EIO;
    }
    stat->filesize = advertised_size;
    stat->flags = 0;
    return 0;
}

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd)
{
    if (fd != FD || host_open_files <= 0)
    {
        return -EIO;
    }
    if (++host_read_calls == host_fail_read_at)
    {
        host_failed = 1;
        return host_read_error;
    }
    if (host_read_calls == host_short_read_at) { host_failed = 1; return 0; }
    if (!size || nmemb > ((uint32_t)content_size - file_position) / size) return 0;
    memcpy(ptr, content + file_position, size * nmemb);
    file_position += size * nmemb;
    host_read_bytes += size * nmemb;
    return nmemb;
}

int fseek(int fd, int offset, FILE_SEEK_MODE mode)
{
    if (fd != FD || host_open_files <= 0 || mode != SEEK_SET || offset < 0 || offset > content_size)
        return -EINVARG;
    file_position = offset;
    return 0;
}

int fclose(int fd)
{
    if (fd != FD || host_open_files <= 0)
    {
        return -EIO;
    }
    host_open_files--;
    return 0;
}

static int known_directory(struct paging_4gb_chunk *directory)
{
    for (int i = 0; i < directory_count; i++)
    {
        if (directories[i] == directory)
        {
            return 1;
        }
    }
    return 0;
}

// The paging layer. One allocation per directory (the real one makes 1026).
struct paging_4gb_chunk *paging_new_4gb(uint8_t flags)
{
    struct paging_4gb_chunk *chunk = kzalloc(sizeof(struct paging_4gb_chunk));
    if (!chunk)
    {
        return 0;
    }
    if (directory_count == MAX_DIRECTORIES)
    {
        printf("paging_new_4gb: too many directories\n");
        exit(2);
    }
    directories[directory_count++] = chunk;
    return chunk;
}

void paging_free_4gb(struct paging_4gb_chunk *chunk)
{
    if (!known_directory(chunk))
    {
        problem("paging_free_4gb() of something that is not a live page directory");
        return;
    }
    for (int i = 0; i < mapping_count; i++)
    {
        if (mappings[i].directory != chunk)
        {
            continue;
        }
        for (int j = 0; j < mappings[i].base_count; j++)
        {
            if (!kalloc_is_live(mappings[i].bases[j]))
            {
                problem("a page directory was freed after memory it maps (free the task first)");
            }
        }
        mappings[i] = mappings[--mapping_count];
        i--;
    }
    for (int i = 0; i < directory_count; i++)
    {
        if (directories[i] == chunk)
        {
            directories[i] = directories[--directory_count];
            break;
        }
    }
    kfree(chunk);
}

void paging_switch(struct paging_4gb_chunk *directory)
{
    if (directory && !known_directory(directory))
    {
        problem("paging_switch() to a page directory that is not live");
    }
    if (host_switch_count < 64)
    {
        host_switches[host_switch_count] = directory;
    }
    host_switch_count++;
}

int paging_map_to(struct paging_4gb_chunk *directory, void *virt, void *phys, void *phys_end, int flags)
{
    if (!known_directory(directory))
    {
        problem("paging_map_to() on a page directory that is not live");
        return -EINVARG;
    }
    if (flags == 0)
    {
        // Unmapping (process_free): forget the mapping, never fails.
        for (int i = 0; i < mapping_count; i++)
        {
            if (mappings[i].directory == directory && mappings[i].virt == virt)
            {
                mappings[i] = mappings[--mapping_count];
                i--;
            }
        }
        return 0;
    }
    if (++host_map_calls == host_fail_map_at)
    {
        host_failed = 1;
        return -EINVARG;
    }
    if (mapping_count == MAX_MAPPINGS)
    {
        printf("paging_map_to: too many mappings\n");
        exit(2);
    }
    if (host_map_log_count < HOST_MAX_MAP_LOG)
    {
        struct host_map_record *r = &host_map_log[host_map_log_count];
        r->directory = directory;
        r->virt = virt;
        r->phys = phys;
        r->phys_end = phys_end;
        r->flags = flags;
    }
    host_map_log_count++;
    struct mapping *m = &mappings[mapping_count++];
    m->directory = directory;
    m->virt = virt;
    m->base_count = 0;
    // Remember the live allocations the mapped range touches.
    for (int i = 0; m->base_count < MAX_BASES; i++)
    {
        char *base = kalloc_live_at(i);
        if (!base)
        {
            break;
        }
        char *end = base + kalloc_size(base);
        if (base < (char *)phys_end && end > (char *)phys)
        {
            m->bases[m->base_count++] = base;
        }
    }
    return 0;
}

int paging_map(struct paging_4gb_chunk *directory, void *virt, void *phys, int flags)
{
    return paging_map_to(directory, virt, phys, (char *)phys + PAGING_PAGE_SIZE, flags);
}

int paging_set(uint32_t *directory, void *virt, uint32_t val) { return 0; }
uint32_t paging_get(uint32_t *directory, void *virt) { return 0; }
void *paging_get_physical_address(uint32_t *directory, void *virt) { return virt; }

void *paging_align_address(void *ptr)
{
    unsigned long p = (unsigned long)ptr;
    return (void *)((p + PAGING_PAGE_SIZE - 1) & ~(unsigned long)(PAGING_PAGE_SIZE - 1));
}

void *paging_align_to_lower_page(void *addr)
{
    return (void *)((unsigned long)addr & ~(unsigned long)(PAGING_PAGE_SIZE - 1));
}

void kernel_page() {}
void user_registers() {}

void task_return(struct registers *regs)
{
    if (host_task_return_armed)
    {
        host_task_return_armed = 0;
        host_task_return_regs = regs;
        longjmp(host_task_return_jump, 1);
    }
    printf("task_return() called unexpectedly\n");
    exit(4);
}

void print(const char *str)
{
    static const char word[] = "terminated";
    for (const char *s = str; *s; s++)
    {
        int i = 0;
        while (word[i] && s[i] == word[i])
        {
            i++;
        }
        if (!word[i])
        {
            host_terminated_prints++;
            break;
        }
    }
}

extern struct task *current_task;
extern struct task *task_head;
extern struct task *task_tail;
extern struct process *current_process;

int host_snapshot_take(struct host_snapshot *s, char *message, int size)
{
    memset(s, 0, sizeof(*s));
    s->head = task_head;
    s->tail = task_tail;
    s->current_task = current_task;
    s->current_process = current_process;
    for (int i = 0; i < RORTHOX_MAX_PROCESSES && i < 32; i++)
    {
        s->slots[i] = process_get(i);
    }
    s->live = kalloc_live();
    s->open_files = host_open_files;
    struct task *prev = 0;
    for (struct task *t = task_head; t; t = t->next)
    {
        if (s->task_count == HOST_MAX_TASKS)
        {
            host_snprintf(message, size, "the task list has more than %d tasks (a loop?)", HOST_MAX_TASKS);
            return 0;
        }
        if (t->prev != prev)
        {
            host_snprintf(message, size, "task %d in the list has a wrong prev link", s->task_count);
            return 0;
        }
        s->tasks[s->task_count++] = t;
        prev = t;
    }
    if (prev != task_tail)
    {
        host_snprintf(message, size, "the list tail is not the last task");
        return 0;
    }
    return 1;
}

int host_snapshot_compare(const struct host_snapshot *a, const struct host_snapshot *b, char *message, int size)
{
    if (a->task_count != b->task_count)
    {
        host_snprintf(message, size, "the task list has %d tasks, expected %d", b->task_count, a->task_count);
        return 1;
    }
    for (int i = 0; i < a->task_count; i++)
    {
        if (a->tasks[i] != b->tasks[i])
        {
            host_snprintf(message, size, "task %d in the list changed", i);
            return 1;
        }
    }
    if (a->head != b->head || a->tail != b->tail)
    {
        host_snprintf(message, size, "the list head or tail changed");
        return 1;
    }
    if (a->current_task != b->current_task)
    {
        host_snprintf(message, size, "the current task changed");
        return 1;
    }
    if (a->current_process != b->current_process)
    {
        host_snprintf(message, size, "the keyboard process (current_process) changed");
        return 1;
    }
    for (int i = 0; i < RORTHOX_MAX_PROCESSES && i < 32; i++)
    {
        if (a->slots[i] != b->slots[i])
        {
            host_snprintf(message, size, "process slot %d changed", i);
            return 1;
        }
    }
    if (a->open_files != b->open_files)
    {
        host_snprintf(message, size, "%d files open, expected %d", b->open_files, a->open_files);
        return 1;
    }
    if (a->live != b->live)
    {
        host_snprintf(message, size, "%d allocations left over", b->live - a->live);
        return 1;
    }
    return 0;
}

struct process *host_load_caller(void)
{
    struct process *caller = 0;
    int res = process_load_for_slot("0:/shell.elf", &caller, 0);
    if (res < 0 || !caller || process_get(0) != caller || !caller->task)
    {
        printf("could not load the caller process (%d)\n", res);
        exit(2);
    }
    process_switch(caller);
    current_task = caller->task;
    if (task_head != caller->task || task_tail != caller->task)
    {
        printf("the caller's task is not the only task in the list\n");
        exit(2);
    }
    return caller;
}

void PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s", msg);
    exit(3);
}
