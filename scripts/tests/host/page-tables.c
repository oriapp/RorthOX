/*
 * The real page tables of paging.c, with the real process_malloc/process_free on top: default
 * identity maps are supervisor-only, a user page opens its PDE and nothing next to it, freeing
 * restores a supervisor identity page, a failed malloc leaves no user page behind, the loaded
 * directory is never changed, and every constructor failure unwinds. Entries are compared as
 * (entry & 0xFFFFF007) with PWT, PCD, PS and G (0x198) clear; A and D (0x60) are ignored.
 * The host heap (kalloc.c) lies below 2 GiB, so its pointers fit in 32-bit page-table entries.
 *
 * A mapping is all or nothing: paging_set and paging_map_range refuse a leaf under a missing or
 * 4 MiB PDE whatever the new value (user, supervisor or not present), a range that touches such
 * a PDE anywhere (first, middle or only its last page), a NULL or loaded directory, a virtual or
 * physical range that wraps past 4 GiB and a negative count, and then no PDE or PTE has changed.
 * A failed process_malloc changes no entry at all, and process_free keeps a page it could not
 * revoke (ownership and backing). Cases that crash or hang on a broken paging.c run in a child.
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
unsigned int alarm(unsigned int seconds);
// string.c brings its own snprintf; format with the C library's checked one.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define host_snprintf_what(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)

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

// Put a copy back (the tables are the ones the saved PDEs point to).
static void restore_directory(uint32_t *directory, uint32_t *saved)
{
    for (int i = 0; i < TOTAL_TABLES; i++)
    {
        directory[i] = saved[i * (TOTAL_TABLES + 1)];
        uint32_t *table = (uint32_t *)(unsigned long)(directory[i] & 0xFFFFF000u);
        for (int j = 0; j < TOTAL_TABLES; j++) table[j] = saved[i * (TOTAL_TABLES + 1) + 1 + j];
    }
}

// Run fn in a child: a crash (a write through a PDE that holds no table) or a hang fails only
// this case.
static void in_child(int (*fn)(void), const char *what)
{
    fflush(0);
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(20);
        int ok = fn();
        fflush(0);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) printf("  %s: killed by signal %d\n", what, WTERMSIG(status));
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, what);
}

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

    // Any other value too: the PDE is checked before its table address is followed. The
    // table address stays in the PDE, so a write through it shows up as a changed PTE.
    static const uint32_t values[] = {0x12345000u | P | W, 0x12345000u | U | W, 0x12345000u | P, 0};
    static const char *const value_names[] = {"supervisor", "not-present user", "read-only supervisor", "zero"};
    for (int kind = 0; kind < 2; kind++)
    {
        for (int v = 0; v < 4; v++)
        {
            char what[120];
            da[pde_hi] = kind ? keep | PAGING_PAGE_SIZE_FLAG : keep & ~P;
            copy_directory(da, saved_a);
            int res = paging_set(da, (void *)(unsigned long)hi, values[v]);
            int changed = changed_entries(da, saved_a, 0, 0);
            host_snprintf_what(what, sizeof(what), "%s value under a %s PDE refused, nothing changed",
                               value_names[v], kind ? "4 MiB" : "not-present");
            if (res != -EINVARG || changed) printf("  %s: returned %d, %d entries changed\n", what, res, changed);
            check(res == -EINVARG && changed == 0, what);
            restore_directory(da, saved_a);
        }
    }
    da[pde_hi] = keep;

    // Unaligned addresses change nothing.
    copy_directory(da, saved_a);
    check(paging_set(da, (void *)0x00403800, 0x00403000u | P | W | U) == -EINVARG &&
          paging_map(a, (void *)0x00403000, (void *)0x00403800, P | W | U) == -EINVARG &&
          changed_entries(da, saved_a, 0, 0) == 0, "unaligned virtual or physical address refused, nothing changed");

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

// Child cases: b is loaded, a is not.
static struct paging_4gb_chunk *child_a, *child_b;

static int null_directory_refused(void)
{
    paging_switch(child_b);
    return paging_set(0, (void *)0x00400000, 0x00400000u | P | W) == -EINVARG &&
           paging_set(0, (void *)0x00400000, 0x00400000u | P | W | U) == -EINVARG;
}

// A PDE of 0 has no table at all: following it would write near address 0.
static int empty_pde_refused(void)
{
    uint32_t *d = paging_4gb_chunk_get_directory(child_a);
    paging_switch(child_b);
    d[2] = 0;
    int ok = 1;
    for (int v = 0; v < 3; v++)
    {
        uint32_t flags = v == 0 ? P | W | U : v == 1 ? P | W : U | W;
        int r1 = paging_set(d, (void *)0x00800000, 0x12345000u | flags);
        int r2 = paging_map_range(child_a, (void *)0x007FF000, (void *)0x12345000, 2, flags);
        if (r1 != -EINVARG || r2 >= 0)
        {
            printf("  flags %#x: paging_set %d, paging_map_range %d\n", flags, r1, r2);
            ok = 0;
        }
    }
    return ok && d[2] == 0 && entry_is(pte(d, 0x007FF000u), 0x007FF000u | P | W);
}

// paging_map_range must refuse, and no PDE or PTE may change.
static void range_refused(struct paging_4gb_chunk *chunk, uint32_t virt, uint32_t phys, int count, int flags, const char *what)
{
    uint32_t *d = paging_4gb_chunk_get_directory(chunk);
    copy_directory(d, saved_a);
    int res = paging_map_range(chunk, (void *)(unsigned long)virt, (void *)(unsigned long)phys, count, flags);
    int changed = changed_entries(d, saved_a, 0, 0);
    if (res >= 0 || changed) printf("  %s: returned %d, %d entries changed\n", what, res, changed);
    check(res < 0 && changed == 0, what);
    restore_directory(d, saved_a);
}

static void range_cases(void)
{
    kalloc_init();
    kalloc_set_alignment(PAGE);
    struct paging_4gb_chunk *a = new_directory(), *b = new_directory();
    uint32_t *d = paging_4gb_chunk_get_directory(a);
    paging_switch(b);
    const uint32_t line = 0x00800000u, phys = 0x12340000u;   // PDE 1 | PDE 2 | PDE 3 at 0xc00000
    char what[160];

    // A bad PDE anywhere in the range: first, second, a middle one, or the one holding only the
    // last page. Every value: user, supervisor and not present.
    static const int flag_sets[] = {P | W | U, P | W, U | W};
    static const char *const flag_names[] = {"user", "supervisor", "not-present"};
    struct { uint32_t virt; int count; uint32_t bad; const char *where; } spans[] = {
        {line - 2 * PAGE, 4, 2, "second of two PDEs"},
        {line - 2 * PAGE, 4, 1, "first of two PDEs"},
        {line - PAGE, TOTAL_TABLES + 2, 2, "middle of three PDEs"},
        {line - PAGE, TOTAL_TABLES + 2, 3, "third of three PDEs, its only page the last"},
    };
    for (int kind = 0; kind < 2; kind++)
        for (int s = 0; s < 4; s++)
            for (int f = 0; f < 3; f++)
            {
                uint32_t keep = d[spans[s].bad];
                d[spans[s].bad] = kind ? keep | PAGING_PAGE_SIZE_FLAG : keep & ~P;
                host_snprintf_what(what, sizeof(what), "range: %s %s PDE (%s leaves): refused, nothing changed",
                                   kind ? "4 MiB" : "not-present", spans[s].where, flag_names[f]);
                range_refused(a, spans[s].virt, phys, spans[s].count, flag_sets[f], what);
                d[spans[s].bad] = keep;
            }

    // Wrapping ranges, negative counts, unaligned addresses.
    range_refused(a, 0xFFFFE000u, phys, 3, P | W | U, "range: virtual end wraps past 4 GiB (user): refused, nothing changed");
    range_refused(a, 0xFFFFF000u, phys, 2, P | W, "range: virtual end wraps past 4 GiB (supervisor): refused, nothing changed");
    range_refused(a, 0x00400000u, 0xFFFFE000u, 3, P | W | U, "range: physical end wraps past 4 GiB: refused, nothing changed");
    range_refused(a, 0, 0x1000u, TOTAL_TABLES * TOTAL_TABLES + 1, P | W,
                  "range: one page more than 4 GiB: refused, nothing changed");
    range_refused(a, 0x00400000u, phys, -1, P | W | U, "range: count -1 refused, nothing changed");
    range_refused(a, 0x00400000u, phys, -2147483647 - 1, P | W | U, "range: count INT_MIN refused, nothing changed");
    range_refused(a, 0x00400800u, phys, 2, P | W | U, "range: unaligned virtual address refused, nothing changed");
    range_refused(a, 0x00400000u, phys + 0x800, 2, P | W | U, "range: unaligned physical address refused, nothing changed");

    // The loaded directory is never changed.
    paging_switch(a);
    range_refused(a, line - PAGE, phys, 2, P | W | U, "range: loaded directory (user): refused, nothing changed");
    range_refused(a, line - PAGE, phys, 2, P | W, "range: loaded directory (supervisor): refused, nothing changed");
    paging_switch(b);

    // An empty range is a no-op.
    copy_directory(d, saved_a);
    check(paging_map_range(a, (void *)(unsigned long)(line - PAGE), (void *)(unsigned long)phys, 0, P | W | U) == 0 &&
          changed_entries(d, saved_a, 0, 0) == 0, "range: count 0 returns 0, nothing changed");

    // A good range across a 4 MiB line: exact leaves, both PDEs gain U and nothing else.
    d[1] |= 0x20;
    uint32_t pde1 = d[1], pde2 = d[2];
    copy_directory(d, saved_a);
    check(paging_map_range(a, (void *)(unsigned long)(line - PAGE), (void *)(unsigned long)phys, 2, P | W | U) == 0,
          "range: user pages across a 4 MiB line mapped");
    check(entry_is(pte(d, line - PAGE), phys | P | W | U) && entry_is(pte(d, line), (phys + PAGE) | P | W | U),
          "range: both user leaves exactly as mapped");
    check(d[1] == (pde1 | U) && d[2] == (pde2 | U), "range: both PDEs gain U only (table address, W and A kept)");
    uint32_t allowed[] = {line - PAGE, line, (uint32_t)-2, (uint32_t)-3};
    check(changed_entries(d, saved_a, allowed, 4) == 0, "range: no other PDE or PTE changed");
    // Revoking them (supervisor identity) keeps the PDEs open: other user pages may share them.
    check(paging_map_range(a, (void *)(unsigned long)(line - PAGE), (void *)(unsigned long)(line - PAGE), 2, P | W) == 0 &&
          entry_is(pte(d, line - PAGE), (line - PAGE) | P | W) && entry_is(pte(d, line), line | P | W) &&
          d[1] == (pde1 | U) && d[2] == (pde2 | U), "range: supervisor restoration does not demote the PDEs");
    // Supervisor and not-present values never open a PDE.
    uint32_t pde5 = d[5];
    check(paging_map_range(a, (void *)0x01400000, (void *)(unsigned long)phys, 2, P | W) == 0 &&
          paging_map_range(a, (void *)0x01402000, (void *)(unsigned long)phys, 2, U | W) == 0 &&
          d[5] == pde5 && entry_is(pte(d, 0x01400000u), phys | P | W) && entry_is(pte(d, 0x01402000u), phys | U | W),
          "range: supervisor and not-present leaves leave the PDE as it was");

    child_a = a;
    child_b = b;
    in_child(null_directory_refused, "NULL directory refused (with another directory loaded)");
    in_child(empty_pde_refused, "PDE 0 (no table): paging_set and paging_map_range refuse, nothing written");
    paging_free_4gb(a);
    paging_free_4gb(b);
    check(kalloc_live() == 0 && kalloc_bad_frees() == 0, "range: both directories freed exactly once");
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

    // A two-page malloc across a 4 MiB line whose second PDE is not present: nothing may be
    // mapped, not even the first page, and its PDE must not gain U. No user page is left in
    // that PDE (the pages above were freed), so U comes off it first.
    char *probe = kzalloc(PAGE);
    uint32_t next = addr(probe) + PAGE;
    kfree(probe);
    uint32_t line = (next + PAGE + 0x3FFFFFu) & ~0x3FFFFFu;
    if (line - PAGE > next) kzalloc(line - PAGE - next);
    uint32_t keep = d[line >> 22];
    d[line >> 22] = keep & ~P;
    d[(line - PAGE) >> 22] &= ~U;
    copy_directory(d, saved_a);
    int live = kalloc_live();
    char *q = process_malloc(&process, 2 * PAGE);
    check(last_kzalloc == (void *)(unsigned long)(line - PAGE), "failed process_malloc: its backing straddled the 4 MiB line");
    check(!q && kalloc_live() == live && kalloc_bad_frees() == 0, "failed process_malloc returns NULL and frees its backing");
    check(entry_is(pte(d, line - PAGE), (line - PAGE) | P | W) && !translates(d, line - PAGE, 0),
          "failed process_malloc: the page before the line is a supervisor page");
    int changed = changed_entries(d, saved_a, 0, 0);
    if (changed) printf("  failed process_malloc: %d entries changed (PDE %#x was %#x)\n", changed,
                        d[(line - PAGE) >> 22], saved_a[((line - PAGE) >> 22) * (TOTAL_TABLES + 1)]);
    check(changed == 0, "failed process_malloc: no PDE or PTE changed (the first page's PDE gained no U)");
    d[line >> 22] = keep;
    check(entry_is(pte(d, line), line | P | W), "failed process_malloc: the page after the line is untouched");
    int owned = 0;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++) owned += process.allocations[i].ptr != 0;
    check(owned == 0, "failed process_malloc records no allocation");

    // process_free whose revocation is refused (its PDE no longer present): the page stays
    // owned and its backing stays allocated, since a user mapping may still reach it.
    char *r = process_malloc(&process, PAGE);
    uint32_t ra = addr(r);
    check(r && entry_is(pte(d, ra), ra | P | W | U), "process_malloc: one user page");
    if (r)
    {
        uint32_t keep_r = d[ra >> 22];
        d[ra >> 22] = keep_r & ~P;
        copy_directory(d, saved_a);
        process_free(&process, r);
        check(kalloc_is_live(r) && process.allocations[0].ptr == r && changed_entries(d, saved_a, 0, 0) == 0,
              "process_free with a refused revocation keeps the page owned and its backing live");
        d[ra >> 22] = keep_r;
        process_free(&process, r);
        check(!kalloc_is_live(r) && process.allocations[0].ptr == 0 && entry_is(pte(d, ra), ra | P | W) && !translates(d, ra, 0),
              "process_free then revokes and frees it");
    }
}

int main(void)
{
    constructor_cases();
    paging_set_cases();
    range_cases();
    process_cases();
    printf("page-tables: %d of %d failed\n", failed, cases);
    return failed != 0;
}
