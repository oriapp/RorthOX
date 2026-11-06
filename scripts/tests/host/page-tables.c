/*
 * The real page tables of paging.c, with the real process_malloc/process_free on top: default
 * identity maps are supervisor-only, a user page opens its PDE and nothing next to it, freeing
 * restores a supervisor identity page, a failed malloc leaves no user page behind, the loaded
 * directory is never changed, and every constructor failure unwinds. Entries are compared as
 * (entry & 0xFFFFF007) with PWT, PCD, PS and G (0x198) clear; A and D (0x60) are ignored.
 * The host heap (kalloc.c) lies below 2 GiB, so its pointers fit in 32-bit page-table entries.
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "memory/paging/paging.h"
#include "task/task.h"
#include "task/process.h"
#include "loader/formats/elfloader.h"
#include "fs/file.h"

#define P PAGING_IS_PRESENT
#define W PAGING_IS_WRITEABLE
#define U PAGING_ACCESS_FROM_ALL
#define PAGE PAGING_PAGE_SIZE
#define MUST_BE_CLEAR 0x198u
#define TOTAL_TABLES PAGING_TOTAL_ENTRIES_PER_TABLE

static int failed, cases;

// Linked with -Wl,--wrap=kzalloc: remembers where the last allocation went.
static void *last_kzalloc;
void *__real_kzalloc(size_t);
void *__wrap_kzalloc(size_t size)
{
    last_kzalloc = __real_kzalloc(size);
    return last_kzalloc;
}

static void check(int ok, const char *what)
{
    cases++;
    if (!ok)
    {
        failed++;
        printf("FAIL %s\n", what);
    }
}

// Unused by the cases here; process.c and paging.c need them to link.
void paging_load_directory(uint32_t *directory) {}
void PANIC(const char *msg, const char *file, int line) { printf("PANIC %s\n", msg); exit(3); }
void print(const char *str) {}
int fopen(const char *filename, const char *mode_str) { return -EIO; }
int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd) { return -EIO; }
int fstat(int fd, struct file_stat *stat) { return -EIO; }
int fclose(int fd) { return 0; }
int file_open(const char *filename, FILE_MODE mode) { return -EIO; }
struct elf_header *elf_header(struct elf_file *file) { return 0; }
struct elf32_phdr *elf_pheader(struct elf_header *header) { return 0; }
struct elf32_phdr *elf_program_header(struct elf_header *header, int index) { return 0; }
void *elf_phdr_phys_address(struct elf_file *file, struct elf32_phdr *phdr) { return 0; }
int elf_load(const char *filename, struct elf_file **file_out) { return -EIO; }
void elf_close(struct elf_file *file) {}
struct task *task_new(struct process *process) { return 0; }
void task_publish(struct task *task) {}
void task_discard(struct task *task) {}
int task_free(struct task *task) { return 0; }

static uint32_t *table_of(uint32_t *directory, uint32_t address)
{
    return (uint32_t *)(unsigned long)(directory[address >> 22] & 0xFFFFF000u);
}

static uint32_t pte(uint32_t *directory, uint32_t address)
{
    return table_of(directory, address)[(address >> 12) & 0x3FF];
}

// entry & 0xFFFFF007 is want, and PWT, PCD, PS and G are clear.
static int entry_is(uint32_t entry, uint32_t want)
{
    return (entry & 0xFFFFF007u) == want && !(entry & MUST_BE_CLEAR);
}

static uint32_t addr(const void *p) { return (uint32_t)(unsigned long)p; }

// Every PDE points to its own live, page-aligned table; every PTE maps its address to itself.
static int default_map_ok(uint32_t *directory, uint32_t pde_flags, uint32_t pte_flags, const char *who)
{
    int bad = 0;
    for (uint32_t i = 0; i < TOTAL_TABLES; i++)
    {
        uint32_t *table = (uint32_t *)(unsigned long)(directory[i] & 0xFFFFF000u);
        if (!entry_is(directory[i], addr(table) | pde_flags) || !kalloc_is_live(table) || kalloc_size(table) != PAGE)
        {
            if (bad++ < 3) printf("  %s: PDE %u is %#x\n", who, i, directory[i]);
            continue;
        }
        for (uint32_t j = 0; j < TOTAL_TABLES; j++)
        {
            uint32_t want = (i << 22) | (j << 12) | pte_flags;
            if (!entry_is(table[j], want) && bad++ < 3)
            {
                printf("  %s: PTE %#x is %#x, expected %#x\n", who, (i << 22) | (j << 12), table[j], want);
            }
        }
    }
    return bad == 0;
}

static struct paging_4gb_chunk *new_directory(void)
{
    struct paging_4gb_chunk *chunk = paging_new_4gb(P | W);
    if (!chunk)
    {
        printf("page-tables: paging_new_4gb failed\n");
        exit(2);
    }
    return chunk;
}

static void copy_directory(uint32_t *directory, uint32_t *saved)
{
    for (int i = 0; i < TOTAL_TABLES; i++)
    {
        saved[i * (TOTAL_TABLES + 1)] = directory[i];
        uint32_t *table = (uint32_t *)(unsigned long)(directory[i] & 0xFFFFF000u);
        for (int j = 0; j < TOTAL_TABLES; j++) saved[i * (TOTAL_TABLES + 1) + 1 + j] = table[j];
    }
}

// Entries (PDEs as -1 - index) that differ from a copy, other than the ones listed.
static int changed_entries(uint32_t *directory, uint32_t *saved, const uint32_t *allowed, int n)
{
    int changed = 0;
    for (int i = 0; i < TOTAL_TABLES; i++)
    {
        uint32_t *table = (uint32_t *)(unsigned long)(directory[i] & 0xFFFFF000u);
        int pde_allowed = 0;
        for (int k = 0; k < n; k++) pde_allowed |= allowed[k] == (uint32_t)(-1 - i);
        if (directory[i] != saved[i * (TOTAL_TABLES + 1)] && !pde_allowed) changed++;
        for (int j = 0; j < TOTAL_TABLES; j++)
        {
            uint32_t a = ((uint32_t)i << 22) | ((uint32_t)j << 12);
            int ok = 0;
            for (int k = 0; k < n; k++) ok |= allowed[k] == a;
            if (table[j] != saved[i * (TOTAL_TABLES + 1) + 1 + j] && !ok) changed++;
        }
    }
    return changed;
}

static uint32_t saved_a[TOTAL_TABLES * (TOTAL_TABLES + 1)];
static uint32_t saved_b[TOTAL_TABLES * (TOTAL_TABLES + 1)];

static int translates(uint32_t *directory, uint32_t address, int write)
{
    void *out = 0;
    return paging_user_translate(directory, address, write, &out) == 0 && out == (void *)(unsigned long)address;
}

static void constructor_cases(void)
{
    kalloc_init();
    kalloc_set_alignment(PAGE);
    struct paging_4gb_chunk *chunk = new_directory();
    uint32_t *d = paging_4gb_chunk_get_directory(chunk);
    check(kalloc_calls() == TOTAL_TABLES + 2 && kalloc_live() == TOTAL_TABLES + 2, "new: directory, 1024 tables and the chunk");
    check(default_map_ok(d, P | W, P | W, "default"), "default map: identity, present, writable, supervisor-only");
    check(entry_is(pte(d, 0x80000000u), 0x80000000u | P | W) && entry_is(pte(d, 0xFFFFF000u), 0xFFFFF000u | P | W),
          "identity above 0x80000000 and at 0xfffff000");
    check(!translates(d, 0x00100000u, 0) && !translates(d, 0x00B8000u, 0) && !translates(d, 0xFFFFF000u, 0),
          "no user translation in a fresh directory");
    paging_free_4gb(chunk);
    check(kalloc_live() == 0 && kalloc_bad_frees() == 0, "free: every table, the directory and the chunk exactly once");

    int unwind_bad = 0;
    for (int n = 1; n <= TOTAL_TABLES + 2; n++)
    {
        kalloc_init();
        kalloc_set_alignment(PAGE);
        kalloc_fail_at(n);
        struct paging_4gb_chunk *c = paging_new_4gb(P | W);
        if (c || !kalloc_failed() || kalloc_live() != 0 || kalloc_bad_frees() != 0)
        {
            if (unwind_bad++ < 3) printf("  allocation %d failing: chunk %p, live %d, bad frees %d\n", n, (void *)c, kalloc_live(), kalloc_bad_frees());
        }
    }
    check(unwind_bad == 0, "new: each of the 1026 allocation failures unwinds without leaks or bad frees");
}

static void paging_set_cases(void)
{
    kalloc_init();
    kalloc_set_alignment(PAGE);
    struct paging_4gb_chunk *a = new_directory(), *b = new_directory();
    uint32_t *da = paging_4gb_chunk_get_directory(a), *db = paging_4gb_chunk_get_directory(b);
    int shared = 0;
    for (int i = 0; i < TOTAL_TABLES; i++)
        for (int j = 0; j < TOTAL_TABLES; j++)
            shared += (da[i] & 0xFFFFF000u) == (db[j] & 0xFFFFF000u);
    check(da != db && shared == 0, "two directories share no table");
    copy_directory(db, saved_b);

    // A read-only user page in a table that also holds supervisor pages; Accessed set on the PDE.
    uint32_t ro = 0x00401000u, pde_ro = ro >> 22;
    da[pde_ro] |= 0x20;
    uint32_t pde_before = da[pde_ro];
    copy_directory(da, saved_a);
    check(paging_set(da, (void *)(unsigned long)ro, 0x12345000u | P | U) == 0, "user read-only page set");
    check(da[pde_ro] == (pde_before | U), "PDE gains U only (table address, W and A kept)");
    check(entry_is(pte(da, ro), 0x12345000u | P | U), "the user PTE is exactly what was set");
    uint32_t allowed_ro[] = {ro, (uint32_t)(-1 - (int)pde_ro)};
    check(changed_entries(da, saved_a, allowed_ro, 2) == 0, "no other PDE or PTE changed (neighbours stay supervisor)");
    check(entry_is(pte(da, ro - PAGE), (ro - PAGE) | P | W) && entry_is(pte(da, ro + PAGE), (ro + PAGE) | P | W),
          "the neighbouring PTEs are supervisor identity pages");
    void *out = 0;
    check(paging_user_translate(da, ro + 8, 0, &out) == 0 && out == (void *)0x12345008, "read-only user page: read translates");
    check(paging_user_translate(da, ro + 8, 1, &out) == -EINVARG, "read-only user page: write refused");
    check(!translates(da, ro + PAGE, 0) && !translates(da, ro - PAGE, 0), "neighbours in the user PDE refuse user reads");

    // Writable user page, then supervisor restoration (what process_free does).
    uint32_t rw = 0x00402000u;
    check(paging_set(da, (void *)(unsigned long)rw, rw | P | W | U) == 0 && translates(da, rw, 1), "user writable page: write translates");
    check(paging_set(da, (void *)(unsigned long)rw, rw | P | W) == 0, "supervisor restoration set");
    check(entry_is(pte(da, rw), rw | P | W) && !translates(da, rw, 0) && !translates(da, rw, 1),
          "restored page: identity, writable, supervisor-only, no user translation");
    check(da[pde_ro] & U, "restoration does not demote the PDE (other user pages share it)");

    // Supervisor and not-present values never open a PDE.
    uint32_t sup = 0x80001000u;
    uint32_t pde_sup = da[sup >> 22];
    check(paging_set(da, (void *)(unsigned long)sup, sup | P | W) == 0 && da[sup >> 22] == pde_sup, "supervisor value: PDE unchanged");
    check(paging_set(da, (void *)(unsigned long)sup, sup | U | W) == 0 && da[sup >> 22] == pde_sup, "not-present user value: PDE unchanged");
    paging_set(da, (void *)(unsigned long)sup, sup | P | W);

    // A user page under a missing or 4 MiB PDE is refused, with nothing changed.
    uint32_t hi = 0xFFFFF000u, pde_hi = hi >> 22, keep = da[pde_hi];
    da[pde_hi] = keep & ~P;
    copy_directory(da, saved_a);
    check(paging_set(da, (void *)(unsigned long)hi, hi | P | U) == -EINVARG && changed_entries(da, saved_a, 0, 0) == 0,
          "user page under a not-present PDE refused, nothing changed");
    da[pde_hi] = keep | PAGING_PAGE_SIZE_FLAG;
    copy_directory(da, saved_a);
    check(paging_set(da, (void *)(unsigned long)hi, hi | P | U) == -EINVARG && changed_entries(da, saved_a, 0, 0) == 0,
          "user page under a 4 MiB PDE refused, nothing changed");
    da[pde_hi] = keep;

    // The loaded directory is never changed; an inactive one still can be.
    paging_switch(a);
    copy_directory(da, saved_a);
    check(paging_set(da, (void *)0x00403000, 0x00403000u | P | W | U) == -EINVARG && changed_entries(da, saved_a, 0, 0) == 0,
          "loaded directory: user value refused, nothing changed");
    check(paging_set(da, (void *)0x00403000, 0x00403000u | P | W) == -EINVARG && changed_entries(da, saved_a, 0, 0) == 0,
          "loaded directory: supervisor value refused, nothing changed");
    paging_switch(b);
    check(paging_set(da, (void *)0x00403000, 0x00403000u | P | W | U) == 0 && entry_is(pte(da, 0x00403000u), 0x00403000u | P | W | U),
          "after switching away the directory can be changed");
    check(changed_entries(db, saved_b, 0, 0) == 0, "the other directory did not change");
    paging_free_4gb(a);
    paging_free_4gb(b);
    check(kalloc_live() == 0 && kalloc_bad_frees() == 0, "both directories freed exactly once");
}

static struct process process;
static struct task task;

static void process_cases(void)
{
    kalloc_init();
    kalloc_set_alignment(PAGE);
    struct paging_4gb_chunk *chunk = new_directory();
    uint32_t *d = paging_4gb_chunk_get_directory(chunk);
    task.page_directory = chunk;
    process.task = &task;
    int base = kalloc_live();

    // malloc: two user pages, nothing else; free: both back to supervisor identity pages.
    copy_directory(d, saved_a);
    char *p = process_malloc(&process, PAGE + 100);
    uint32_t a = addr(p);
    check(p && (a & (PAGE - 1)) == 0, "process_malloc: page-aligned allocation");
    check(p && entry_is(pte(d, a), a | P | W | U) && entry_is(pte(d, a + PAGE), (a + PAGE) | P | W | U) &&
          (d[a >> 22] & U) && (d[(a + PAGE) >> 22] & U), "process_malloc: both pages user-writable identity, PDEs opened");
    uint32_t allowed[] = {a, a + PAGE, (uint32_t)(-1 - (int)(a >> 22)), (uint32_t)(-1 - (int)((a + PAGE) >> 22))};
    check(p && changed_entries(d, saved_a, allowed, 4) == 0, "process_malloc: nothing else changed");
    process_free(&process, p);
    check(p && entry_is(pte(d, a), a | P | W) && entry_is(pte(d, a + PAGE), (a + PAGE) | P | W) &&
          !translates(d, a, 0) && !translates(d, a + PAGE, 0), "process_free: supervisor identity pages, no user translation");
    check(!kalloc_is_live(p) && kalloc_live() == base && kalloc_bad_frees() == 0, "process_free: backing freed");
    check(p && changed_entries(d, saved_a, allowed, 4) == 0, "process_free: nothing else changed");

    // A two-page malloc across a 4 MiB line whose second PDE is not present: the first page
    // was already made a user page when the second fails; it must be a supervisor page again.
    char *probe = kzalloc(PAGE);
    uint32_t next = addr(probe) + PAGE;
    kfree(probe);
    uint32_t line = (next + PAGE + 0x3FFFFFu) & ~0x3FFFFFu;
    if (line - PAGE > next) kzalloc(line - PAGE - next);
    uint32_t keep = d[line >> 22];
    d[line >> 22] = keep & ~P;
    int live = kalloc_live();
    char *q = process_malloc(&process, 2 * PAGE);
    check(last_kzalloc == (void *)(unsigned long)(line - PAGE), "failed process_malloc: its backing straddled the 4 MiB line");
    check(!q && kalloc_live() == live && kalloc_bad_frees() == 0, "failed process_malloc returns NULL and frees its backing");
    check(entry_is(pte(d, line - PAGE), (line - PAGE) | P | W) && !translates(d, line - PAGE, 0),
          "failed process_malloc: the page mapped before the failure is supervisor-only again");
    d[line >> 22] = keep;
    check(entry_is(pte(d, line), line | P | W), "failed process_malloc: the page after the line is untouched");
    int owned = 0;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++) owned += process.allocations[i].ptr != 0;
    check(owned == 0, "failed process_malloc records no allocation");
}

int main(void)
{
    constructor_cases();
    paging_set_cases();
    process_cases();
    printf("page-tables: %d of %d failed\n", failed, cases);
    return failed != 0;
}
