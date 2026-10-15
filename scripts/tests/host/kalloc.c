#define _GNU_SOURCE
#include "kalloc.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_SIZE (64 * 1024 * 1024)
#define MAX_ALLOCS 8192

struct allocation
{
    unsigned char *ptr;
    size_t size;
    int live;
};

static unsigned char *arena;
static size_t arena_used;
static struct allocation allocs[MAX_ALLOCS];
static const void *callers[MAX_ALLOCS];
static int alloc_count;
static int call_count;
static int fail_at;
static int fail_happened;
static int bad_frees;
static size_t alignment = 16;
static size_t guard_size;
static int guard_where;
static int guard_call;

void kalloc_init(void)
{
    if (!arena)
    {
        // Below 2 GiB, so (int)pointer keeps the value and stays positive.
        arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (arena == MAP_FAILED || (uintptr_t)arena + ARENA_SIZE > 0x7fffffffu)
        {
            fprintf(stderr, "kalloc: no memory below 2 GiB\n");
            exit(2);
        }
    }
    arena_used = 0;
    alloc_count = 0;
    call_count = 0;
    fail_at = 0;
    fail_happened = 0;
    bad_frees = 0;
    alignment = 16;
    guard_size = 0;
    guard_where = KALLOC_GUARD_NONE;
    guard_call = 0;
}

void kalloc_set_alignment(size_t align)
{
    if (align < 16 || (align & (align - 1)))
    {
        fprintf(stderr, "kalloc: bad alignment %zu\n", align);
        exit(2);
    }
    alignment = align;
}

void kalloc_fill_unused(const void *pattern, size_t size, size_t bytes)
{
    if (!arena || size == 0 || arena_used + bytes > ARENA_SIZE)
    {
        fprintf(stderr, "kalloc: cannot fill %zu unused bytes\n", bytes);
        exit(2);
    }
    for (size_t i = 0; i < bytes; i++)
    {
        arena[arena_used + i] = ((const unsigned char *)pattern)[i % size];
    }
}

void kalloc_guard_next(size_t size, int where)
{
    guard_size = size;
    guard_where = where;
    guard_call = 0;
}

int kalloc_guard_call(void)
{
    return guard_call;
}

// size bytes on pages of their own between two PROT_NONE pages, below 2 GiB like the arena.
static unsigned char *guarded(size_t size)
{
    size_t pages = (size + 4095) / 4096;
    unsigned char *base = mmap(NULL, (pages + 2) * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (base == MAP_FAILED || (uintptr_t)base + (pages + 2) * 4096 > 0x7fffffffu ||
        mprotect(base + 4096, pages * 4096, PROT_READ | PROT_WRITE) != 0)
    {
        fprintf(stderr, "kalloc: no guarded memory below 2 GiB\n");
        exit(2);
    }
    return guard_where == KALLOC_GUARD_BELOW ? base + 4096 : base + 4096 + pages * 4096 - size;
}

void kalloc_fail_at(int n)
{
    fail_at = n;
    fail_happened = 0;
}

int kalloc_failed(void)
{
    return fail_happened;
}

int kalloc_calls(void)
{
    return call_count;
}

static struct allocation *find(const void *ptr)
{
    for (int i = 0; i < alloc_count; i++)
    {
        if (allocs[i].ptr == ptr)
        {
            return &allocs[i];
        }
    }
    return NULL;
}

int kalloc_live(void)
{
    int n = 0;
    for (int i = 0; i < alloc_count; i++)
    {
        n += allocs[i].live;
    }
    return n;
}

int kalloc_is_live(const void *ptr)
{
    struct allocation *a = find(ptr);
    return a && a->live;
}

long kalloc_size(const void *ptr)
{
    struct allocation *a = find(ptr);
    return a && a->live ? (long)a->size : -1;
}

void *kalloc_live_at(int i)
{
    for (int j = 0; j < alloc_count; j++)
    {
        if (allocs[j].live && i-- == 0)
        {
            return allocs[j].ptr;
        }
    }
    return NULL;
}

int kalloc_bad_frees(void)
{
    return bad_frees;
}

int kalloc_poison_errors(void)
{
    int n = 0;
    for (int i = 0; i < alloc_count; i++)
    {
        if (allocs[i].live)
        {
            continue;
        }
        for (size_t j = 0; j < allocs[i].size; j++)
        {
            if (allocs[i].ptr[j] != KALLOC_POISON)
            {
                n++;
                break;
            }
        }
    }
    return n;
}

const char *kalloc_caller(int n)
{
    Dl_info info;
    if (n < 1 || n > call_count || n > MAX_ALLOCS || !callers[n - 1])
    {
        return "?";
    }
    if (dladdr(callers[n - 1], &info) && info.dli_sname)
    {
        return info.dli_sname;
    }
    return "?";
}

static void *allocate(size_t size, const void *caller)
{
    call_count++;
    if (call_count <= MAX_ALLOCS)
    {
        callers[call_count - 1] = caller;
    }
    if (call_count == fail_at)
    {
        fail_happened = 1;
        return NULL;
    }
    // The kernel heap hands out whole 4 KiB blocks; size 0 gets nothing (K3).
    if (size == 0)
    {
        return NULL;
    }
    size_t start = (arena_used + alignment - 1) & ~(alignment - 1);
    size_t rounded = (size + 15) & ~(size_t)15;
    if (alloc_count == MAX_ALLOCS || start + rounded > ARENA_SIZE)
    {
        fprintf(stderr, "kalloc: test arena full\n");
        exit(2);
    }
    struct allocation *a = &allocs[alloc_count++];
    a->size = size;
    a->live = 1;
    if (guard_where != KALLOC_GUARD_NONE && !guard_call && size == guard_size)
    {
        a->ptr = guarded(size);
        guard_call = call_count;
    }
    else
    {
        a->ptr = arena + start;
        arena_used = start + rounded;
    }
    // Not zeroed for kmalloc: fill with a pattern that is neither 0 nor the poison.
    memset(a->ptr, 0x5A, size);
    return a->ptr;
}

void *kmalloc(size_t size)
{
    return allocate(size, __builtin_return_address(0));
}

void *kzalloc(size_t size)
{
    void *ptr = allocate(size, __builtin_return_address(0));
    if (ptr)
    {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr)
{
    // The kernel heap ignores NULL (and anything outside the heap) since K4.
    if (!ptr)
    {
        return;
    }
    struct allocation *a = find(ptr);
    if (!a || !a->live)
    {
        bad_frees++;
        return;
    }
    a->live = 0;
    memset(a->ptr, KALLOC_POISON, a->size);
}
