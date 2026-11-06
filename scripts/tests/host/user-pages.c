/* Whole user pages: malloc and raw program backing cleared to the page end, raw program stack zero, ownership exact. */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "memory/paging/paging.h"
#include "task/process.h"

#define PAGE PAGING_PAGE_SIZE
#define ROUND(n) (((uint64_t)(n) + PAGE - 1) / PAGE * PAGE)
// Stale bytes in every free block the next allocations can get.
#define STALE 0xC3
#define STALE_BYTES (8u * 1024 * 1024)
// The host arena is small: bigger requests fail, as on a kernel heap without such a free run.
#define HOST_HEAP_LIMIT (16u * 1024 * 1024)

void *__real_kzalloc(size_t);
void *__wrap_kzalloc(size_t size)
{
    if (size > HOST_HEAP_LIMIT) return 0;
    return __real_kzalloc(size);
}

static unsigned char blank[65536];
static int blank_size;
static unsigned char content[3 * PAGE];
static unsigned char *stale_start, *stale_end;

// Fill the next STALE_BYTES of the arena with STALE: everything allocated after this comes from
// there, as from heap blocks that held someone else's data.
static void make_stale(void)
{
    unsigned char *marker = kzalloc(1);
    unsigned char pattern = STALE;
    kalloc_fill_unused(&pattern, 1, STALE_BYTES);
    stale_start = marker + 16;
    stale_end = stale_start + STALE_BYTES;
}

// 1 (and a message) unless [ptr, ptr+size) lies in the stale area and was all STALE before
// (kalloc never reuses memory, so only this allocation wrote there).
static int not_stale_backing(const char *what, unsigned char *ptr, uint64_t size)
{
    if (ptr < stale_start || ptr + size > stale_end)
    {
        printf("%s: backing %p+%llu is not in the stale area %p-%p, nothing proved\n", what,
               (void *)ptr, (unsigned long long)size, (void *)stale_start, (void *)stale_end);
        return 1;
    }
    return 0;
}

static int first_nonzero(const unsigned char *ptr, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; i++)
        if (ptr[i]) return (int)i;
    return -1;
}

static int span(struct process *p, uint32_t a, int expected, uint32_t length, const char *what)
{
    uint32_t n = 0;
    int rc = process_user_span(p, a, 0, &n);
    if (rc != expected || (!rc && n != length))
    {
        printf("%s: span %#x rc=%d length=%u, want %d/%u\n", what, a, rc, n, expected, length);
        return 1;
    }
    return 0;
}

static struct process_allocation *entry(struct process *p, void *ptr)
{
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
        if (p->allocations[i].ptr == ptr) return &p->allocations[i];
    return 0;
}

static int used_entries(struct process *p)
{
    int n = 0;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
        n += p->allocations[i].ptr != 0;
    return n;
}

static const uint32_t good_sizes[] = {1, 19, PAGE - 1, PAGE, PAGE + 1, 2 * PAGE + 5};
#define GOOD (int)(sizeof(good_sizes) / sizeof(good_sizes[0]))
static const uint32_t bad_sizes[] = {0, 0xFFFFF001U, 0xFFFFFFFFU, 0x7FFFF000U, HOST_HEAP_LIMIT + 1};
#define BAD (int)(sizeof(bad_sizes) / sizeof(bad_sizes[0]))
static const uint32_t raw_sizes[] = {1, 3, PAGE - 1, PAGE, PAGE + 1, 2 * PAGE - 1, 2 * PAGE + 1, 3 * PAGE};
#define RAW (int)(sizeof(raw_sizes) / sizeof(raw_sizes[0]))

static struct process *caller(void)
{
    kalloc_init(); kalloc_set_alignment(PAGE); host_reset();
    host_file_serve(blank, blank_size);
    return host_load_caller();
}

static int malloc_case(uint32_t size)
{
    char what[64];
    host_snprintf(what, sizeof(what), "malloc(%u)", size);
    struct process *p = caller();
    make_stale();
    int maps = host_map_log_count;
    unsigned char *ptr = process_malloc(p, size);
    if (!ptr) { printf("%s: refused\n", what); return 1; }
    int bad = not_stale_backing(what, ptr, ROUND(size));
    if (kalloc_size(ptr) != (long)ROUND(size))
    { printf("%s: backing is %ld bytes, want %llu\n", what, kalloc_size(ptr), (unsigned long long)ROUND(size)); bad = 1; }
    int at = first_nonzero(ptr, 0, ROUND(size));
    if (at >= 0) { printf("%s: stale byte %#x at offset %d (requested %u)\n", what, ptr[at], at, size); bad = 1; }
    struct process_allocation *a = entry(p, ptr);
    if (!a || a->size != size)
    { printf("%s: recorded size %u, want %u\n", what, a ? (unsigned)a->size : 0, size); bad = 1; }
    if (host_map_log_count != maps + 1 || host_map_log[maps].virt != ptr || host_map_log[maps].phys != ptr ||
        host_map_log[maps].phys_end != ptr + ROUND(size))
    { printf("%s: not mapped as its whole pages\n", what); bad = 1; }
    uint32_t base = (uint32_t)(unsigned long)ptr;
    bad |= span(p, base, 0, size, what);
    bad |= span(p, base + size - 1, 0, 1, what);
    if (size % PAGE) bad |= span(p, base + size, -EINVARG, 0, what);
    process_free(p, ptr);
    if (kalloc_is_live(ptr) || entry(p, ptr) || kalloc_bad_frees())
    { printf("%s: not freed cleanly\n", what); bad = 1; }
    if (!bad) printf("%s: %llu bytes zero, %u owned\n", what, (unsigned long long)ROUND(size), size);
    return bad;
}

static int refused_case(uint32_t size)
{
    char what[64];
    host_snprintf(what, sizeof(what), "malloc(%#x)", size);
    struct process *p = caller();
    int live = kalloc_live(), maps = host_map_log_count, used = used_entries(p);
    void *ptr = process_malloc(p, size);
    int bad = ptr != 0 || kalloc_live() != live || host_map_log_count != maps || used_entries(p) != used ||
              kalloc_bad_frees();
    printf("%s: result %p, live %d->%d, mappings %d->%d, owned %d->%d%s\n", what, ptr, live, kalloc_live(),
           maps, host_map_log_count, used, used_entries(p), bad ? " FAIL" : "");
    return bad;
}

static int raw_case(uint32_t size)
{
    char what[64];
    host_snprintf(what, sizeof(what), "raw program of %u bytes", size);
    caller();
    for (uint32_t i = 0; i < sizeof(content); i++) content[i] = (unsigned char)(i * 7 + 1) | 1;
    host_reset();
    host_file_serve(content, size);
    host_file_size(size);
    make_stale();
    struct process *out = 0;
    int rc = process_load("0:/raw.bin", &out);
    if (rc || !out || out->filetype != PROCESS_FILETYPE_BINARY)
    { printf("%s: load returned %d\n", what, rc); return 1; }
    unsigned char *ptr = out->ptr;
    int bad = not_stale_backing(what, ptr, ROUND(size));
    if (kalloc_size(ptr) != (long)ROUND(size))
    { printf("%s: backing is %ld bytes, want %llu\n", what, kalloc_size(ptr), (unsigned long long)ROUND(size)); bad = 1; }
    if (memcmp(ptr, content, size)) { printf("%s: payload changed\n", what); bad = 1; }
    int at = first_nonzero(ptr, size, ROUND(size));
    if (at >= 0) { printf("%s: stale byte %#x at offset %d\n", what, ptr[at], at); bad = 1; }
    // The user stack comes from the stale area too and must be fully zero.
    bad |= not_stale_backing(what, out->stack, USER_PROGRAM_STACK_SIZE);
    at = first_nonzero(out->stack, 0, USER_PROGRAM_STACK_SIZE);
    if (at >= 0) { printf("%s: stale stack byte %#x at offset %d\n", what, ((unsigned char *)out->stack)[at], at); bad = 1; }
    if (out->size != size) { printf("%s: recorded size %u\n", what, out->size); bad = 1; }
    bad |= span(out, RORTHOX_PROGRAM_VIRTUAL_ADDRESS, 0, size, what);
    bad |= span(out, RORTHOX_PROGRAM_VIRTUAL_ADDRESS + size - 1, 0, 1, what);
    bad |= span(out, RORTHOX_PROGRAM_VIRTUAL_ADDRESS + size, -EINVARG, 0, what);
    if (!bad) printf("%s: payload kept, %llu bytes padding zero, stack zero, %u owned\n", what,
                     (unsigned long long)(ROUND(size) - size), size);
    return bad;
}

static int run(int n)
{
    if (n < GOOD) return malloc_case(good_sizes[n]);
    n -= GOOD;
    if (n < BAD) return refused_case(bad_sizes[n]);
    return raw_case(raw_sizes[n - BAD]);
}

int main(int argc, char **argv)
{
    int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < 52 || blank_size == sizeof(blank)) return 2;
    close(fd);
    int total = GOOD + BAD + RAW, failed = 0;
    for (int n = 0; n < total; n++)
    {
        fflush(0); int pid = fork();
        if (!pid) { int rc = run(n); fflush(0); _exit(rc); }
        int status; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { printf("FAIL user-pages case %d status=%d\n", n, status); failed++; }
    }
    printf("user-pages host: %d of %d failed\n", failed, total);
    return failed != 0;
}
