/*
 * Host check of ELF load segments (scripts/tests/elf-segments.sh).
 *
 * Builds the real kernel/task/process.c, task.c and loader/formats/elfloader.c with the host C
 * compiler, with the file and paging layers of process-host.c and the heap of kalloc.c, set to
 * page-aligned blocks like the kernel heap. Unused heap memory is filled with a pattern that is
 * neither 0 nor file data, so memory that nobody zeroed shows up. A caller process runs in slot
 * 0, and every case loads a program into slot 1 with process_load_for_slot().
 *
 * A good program must get, for each PT_LOAD with p_memsz > 0 and nothing else, one mapping of
 * its pages: from the page of p_vaddr, as many pages as p_vaddr % 4096 + p_memsz needs, present
 * and user, writable only with PF_W. Each mapping must be page aligned and be exactly one live
 * allocation of its own (not the file buffer, not shared with another segment), holding the
 * p_filesz bytes of the file at the right page offset and zeroes in every other byte. When the
 * process ends, everything it took must be freed. A file with the ELF magic that is malformed
 * or unsupported (header fields, program header table, segment offsets and sizes, alignment,
 * the load window, overlapping or page-sharing segments, the entry point, PT_INTERP/PT_DYNAMIC,
 * any length from 4 to 51 bytes) must be refused with -EINVARG, must not be loaded as a flat
 * binary, and must leave everything as it was. A file without the ELF magic still loads as a
 * flat binary. Every allocation and mapping of a good multi-segment load fails in turn and must
 * also leave everything as it was. Each case runs in its own process.
 *
 * The malformed files, the short files and the good made-up program also run with the file
 * buffer (the kernel's copy of the file, allocated by elf_load) between two unreadable pages,
 * once starting right after the lower one and once ending right before the upper one: a read of
 * even one byte before or past the file crashes the case.
 *
 * Usage: elf-segments <file.elf>...   (real programs that must load like the made-up ones)
 */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "memory/memory.h"
#include "memory/paging/paging.h"
#include "string/string.h"
#include "task/task.h"
#include "task/process.h"
#include "loader/formats/elfloader.h"

#define MAX_FILE (64 * 1024)
#define MAX_PHDRS 8
#define PAGE 4096u
#define NOT_REACHED 10
#define PT_GNU_STACK 0x6474e551

#define WINDOW_START RORTHOX_PROGRAM_VIRTUAL_ADDRESS
#ifdef RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT
#define WINDOW_END RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT
#else
#define WINDOW_END 0x500000
#endif

static unsigned char file[MAX_FILE];
static int file_size;
static unsigned char shell[MAX_FILE];
static int shell_size;
static char message[200];
static int quiet;
// Where the file buffer goes: KALLOC_GUARD_NONE (in the heap), _BELOW or _ABOVE (guard pages).
static int guard;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

static struct process *const untouched = (struct process *)0x1234;

static struct elf_header *header(void)
{
    return (struct elf_header *)file;
}

static struct elf32_phdr *phdr(int i)
{
    return (struct elf32_phdr *)(file + header()->e_phoff + i * 32);
}

static void set_phdr(int i, uint32_t type, uint32_t offset, uint32_t vaddr, uint32_t filesz, uint32_t memsz,
                     uint32_t flags, uint32_t align)
{
    struct elf32_phdr *p = phdr(i);
    p->p_type = type;
    p->p_offset = offset;
    p->p_vaddr = vaddr;
    p->p_paddr = vaddr;
    p->p_filesz = filesz;
    p->p_memsz = memsz;
    p->p_flags = flags;
    p->p_align = align;
}

/*
 * The good made-up program, 0x4000 bytes, every byte after the program headers non-zero:
 *   0 PT_LOAD  R+X  off 0x1000 vaddr 0x400000 filesz 0x1800 memsz 0x1800  (entry 0x400010)
 *   1 PT_NOTE       off 0x0200 vaddr 0x402100 filesz 0x20   memsz 0x20    (not mapped)
 *   2 PT_LOAD  R+W  off 0x3010 vaddr 0x403010 filesz 0x100  memsz 0x900   (.data, .bss in its page)
 *   3 PT_LOAD  R+W  off 0      vaddr 0x405000 filesz 0      memsz 0x30    (only .bss)
 *   4 PT_LOAD  R+W  off 0x3800 vaddr 0x407800 filesz 0x10   memsz 0x3923  (.bss over 5 pages)
 *   5 PT_GNU_STACK  everything 0                                          (not mapped)
 *   6 PT_LOAD  R    off 0      vaddr 0x40d000 filesz 0      memsz 0       (empty, not mapped)
 */
static void make_good(void)
{
    memset(file, 0, sizeof(file));
    file_size = 0x4000;
    for (int i = 0; i < file_size; i++)
    {
        file[i] = (unsigned char)(i % 251 + 1);
    }
    struct elf_header *h = header();
    memset(h, 0, 52 + 7 * 32);
    h->e_ident[0] = 0x7f;
    h->e_ident[1] = 'E';
    h->e_ident[2] = 'L';
    h->e_ident[3] = 'F';
    h->e_ident[EI_CLASS] = ELFCLASS32;
    h->e_ident[EI_DATA] = ELFDATA2LSB;
    h->e_ident[6] = 1;
    h->e_type = ET_EXEC;
    h->e_machine = 3;
    h->e_version = 1;
    h->e_entry = 0x400010;
    h->e_phoff = 52;
    h->e_ehsize = 52;
    h->e_phentsize = 32;
    h->e_phnum = 7;
    set_phdr(0, PT_LOAD, 0x1000, 0x400000, 0x1800, 0x1800, PF_R | PF_X, 0x1000);
    set_phdr(1, PT_NOTE, 0x200, 0x402100, 0x20, 0x20, PF_R, 4);
    set_phdr(2, PT_LOAD, 0x3010, 0x403010, 0x100, 0x900, PF_R | PF_W, 0x1000);
    set_phdr(3, PT_LOAD, 0, 0x405000, 0, 0x30, PF_R | PF_W, 0x1000);
    set_phdr(4, PT_LOAD, 0x3800, 0x407800, 0x10, 0x3923, PF_R | PF_W, 0x1000);
    set_phdr(5, PT_GNU_STACK, 0, 0, 0, 0, PF_R | PF_W, 0x10);
    set_phdr(6, PT_LOAD, 0, 0x40d000, 0, 0, PF_R, 0x1000);
}

static void serve_file(void)
{
    host_file_serve(file, file_size);
}

// The caller in slot 0, unused heap filled, then the file the child loads. Fills before.
static struct process *setup(struct host_snapshot *before)
{
    static const unsigned char stale[] = { 0xC3, 0x5E, 0x91, 0x2B };
    kalloc_init();
    kalloc_set_alignment(PAGE);
    host_reset();
    host_file_serve(shell, shell_size);
    struct process *caller = host_load_caller();
    kalloc_fill_unused(stale, sizeof(stale), 4 * 1024 * 1024);
    serve_file();
    kalloc_guard_next(file_size, guard);
    host_reset();
    if (!host_snapshot_take(before, message, sizeof(message)))
    {
        printf("setup: %s\n", message);
        exit(2);
    }
    return caller;
}

// With guard pages on, the guarded allocation must have been the file buffer. 0 = pass.
static int check_guard(const char *what)
{
    if (guard == KALLOC_GUARD_NONE)
    {
        return 0;
    }
    const char *by = kalloc_guard_call() ? kalloc_caller(kalloc_guard_call()) : "nobody";
    if (strncmp(by, "elf_load", 9) != 0)
    {
        say("%s: the guarded %d-byte allocation was made by %s, not elf_load\n", what, file_size, by);
        return 1;
    }
    return 0;
}

// After a load that must have changed nothing. 0 = pass.
static int check_unchanged(const char *what, const struct host_snapshot *before, struct process *out,
                           struct process *caller)
{
    struct host_snapshot after;
    if (!host_snapshot_take(&after, message, sizeof(message)))
    {
        say("%s: %s\n", what, message);
        return 1;
    }
    if (out != untouched)
    {
        say("%s: *process was changed\n", what);
        return 1;
    }
    if (host_snapshot_compare(before, &after, message, sizeof(message)))
    {
        say("%s: %s\n", what, message);
        return 1;
    }
    if (kalloc_bad_frees() || kalloc_poison_errors())
    {
        say("%s: bad kfree or write after kfree\n", what);
        return 1;
    }
    if (host_paging_problems)
    {
        say("%s: %s\n", what, host_paging_problem);
        return 1;
    }
    for (int i = 0; i < host_switch_count && i < 64; i++)
    {
        if (host_switches[i] && host_switches[i] != caller->task->page_directory)
        {
            say("%s: the new page directory was loaded\n", what);
            return 1;
        }
    }
    return 0;
}

// The live allocation that holds [start, end), or NULL.
static unsigned char *owner(const unsigned char *start, const unsigned char *end)
{
    for (int i = 0;; i++)
    {
        unsigned char *base = kalloc_live_at(i);
        if (!base)
        {
            return NULL;
        }
        if (base <= start && end <= base + kalloc_size(base))
        {
            return base;
        }
    }
}

// The mappings of a loaded child, checked against the program headers of file[]. 0 = pass.
static int check_segments(const char *what, struct process *child)
{
    struct elf_header *h = header();
    struct paging_4gb_chunk *dir = child->task->page_directory;
    struct elf_file *elf = child->elf_file;
    int used[HOST_MAX_MAP_LOG] = {};
    int loads = 0;

    if (host_map_log_count > HOST_MAX_MAP_LOG)
    {
        say("%s: %d mappings\n", what, host_map_log_count);
        return 1;
    }
    // The stack is the only mapping that is not a segment.
    for (int m = 0; m < host_map_log_count; m++)
    {
        if (host_map_log[m].directory == dir &&
            (uint32_t)(uintptr_t)host_map_log[m].virt == (uint32_t)RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_END &&
            host_map_log[m].phys == child->stack)
        {
            used[m] = 1;
        }
    }
    for (int i = 0; i < h->e_phnum; i++)
    {
        struct elf32_phdr *p = phdr(i);
        if (p->p_type != PT_LOAD || p->p_memsz == 0)
        {
            continue;
        }
        loads++;
        uint32_t lo = p->p_vaddr & ~(PAGE - 1);
        uint32_t hi = (p->p_vaddr + p->p_memsz + PAGE - 1) & ~(PAGE - 1);
        int found = -1;
        for (int m = 0; m < host_map_log_count; m++)
        {
            if (!used[m] && host_map_log[m].directory == dir && (uint32_t)(uintptr_t)host_map_log[m].virt == lo)
            {
                found = m;
                break;
            }
        }
        if (found < 0)
        {
            say("%s: program header %d (vaddr 0x%x) is not mapped from its first page 0x%x\n", what, i,
                p->p_vaddr, lo);
            return 1;
        }
        used[found] = 1;
        struct host_map_record *r = &host_map_log[found];
        unsigned char *phys = r->phys;
        unsigned char *phys_end = r->phys_end;
        if (((uintptr_t)phys | (uintptr_t)phys_end) % PAGE || phys_end - phys != (long)(hi - lo))
        {
            say("%s: program header %d maps %ld bytes at %p, expected 0x%x page-aligned bytes\n", what, i,
                (long)(phys_end - phys), (void *)phys, hi - lo);
            return 1;
        }
        int want_flags = PAGING_IS_PRESENT | PAGING_ACCESS_FROM_ALL | ((p->p_flags & PF_W) ? PAGING_IS_WRITEABLE : 0);
        if (r->flags != want_flags)
        {
            say("%s: program header %d mapped with flags 0x%x, expected 0x%x\n", what, i, r->flags, want_flags);
            return 1;
        }
        unsigned char *base = owner(phys, phys_end);
        if (!base)
        {
            say("%s: program header %d maps memory that is not one live allocation\n", what, i);
            return 1;
        }
        if (base == (unsigned char *)elf->elf_memory || base == (unsigned char *)elf || base == child->stack ||
            base == (unsigned char *)child)
        {
            say("%s: program header %d maps the %s\n", what, i,
                base == (unsigned char *)elf->elf_memory ? "file buffer" : "ELF object, stack or process");
            return 1;
        }
        if (base != phys || kalloc_size(base) != (long)(hi - lo))
        {
            say("%s: program header %d maps part of a %ld-byte allocation, expected its own 0x%x bytes\n", what, i,
                kalloc_size(base), hi - lo);
            return 1;
        }
        for (int m = 0; m < host_map_log_count; m++)
        {
            if (m != found && host_map_log[m].directory == dir && owner(host_map_log[m].phys, host_map_log[m].phys_end) == base)
            {
                say("%s: program header %d shares its memory with another mapping\n", what, i);
                return 1;
            }
        }
        for (uint32_t v = lo; v < hi; v++)
        {
            unsigned char want = 0;
            if (v >= p->p_vaddr && v - p->p_vaddr < p->p_filesz)
            {
                want = file[p->p_offset + (v - p->p_vaddr)];
            }
            if (phys[v - lo] != want)
            {
                say("%s: program header %d: byte at 0x%x is 0x%02x, expected 0x%02x (%s)\n", what, i, v,
                    phys[v - lo], want,
                    v < p->p_vaddr ? "padding before the segment" :
                    v - p->p_vaddr < p->p_filesz ? "file data" :
                    v - p->p_vaddr < p->p_memsz ? ".bss" : "padding after the segment");
                return 1;
            }
        }
    }
    for (int m = 0; m < host_map_log_count; m++)
    {
        if (!used[m] && host_map_log[m].directory == dir)
        {
            say("%s: extra mapping at 0x%x (%ld bytes) that is not a loaded segment\n", what,
                (uint32_t)(uintptr_t)host_map_log[m].virt,
                (long)((char *)host_map_log[m].phys_end - (char *)host_map_log[m].phys));
            return 1;
        }
    }
    if (loads == 0)
    {
        say("%s: no PT_LOAD with memory in the file\n", what);
        return 1;
    }
    return 0;
}

// A good load: mapped as above, then everything freed when the process ends.
static int good_case(const char *what)
{
    struct host_snapshot before;
    setup(&before);
    struct process *out = untouched;
    int res = process_load_for_slot("0:/good.elf", &out, 1);
    if (res != 0 || out == untouched || !out || process_get(1) != out)
    {
        say("%s: process_load_for_slot returned %d\n", what, res);
        return 1;
    }
    if (check_guard(what))
    {
        return 1;
    }
    if (out->filetype != PROCESS_FILETYPE_ELF)
    {
        say("%s: not loaded as an ELF file\n", what);
        return 1;
    }
    if (host_paging_problems || host_open_files || kalloc_bad_frees() || kalloc_poison_errors())
    {
        say("%s: paging problem, open file, bad kfree or write after kfree\n", what);
        return 1;
    }
    if (check_segments(what, out))
    {
        return 1;
    }
    // Ending the process frees the program data first, then the page directory (as exit does).
    host_paging_problems = 0;
    process_terminate(out);
    struct host_snapshot after;
    if (!host_snapshot_take(&after, message, sizeof(message)))
    {
        say("%s: after exit: %s\n", what, message);
        return 1;
    }
    if (after.live != before.live || kalloc_bad_frees() || kalloc_poison_errors() || process_get(1))
    {
        say("%s: after exit %d allocations left over (bad frees %d, writes after kfree %d)\n", what,
            after.live - before.live, kalloc_bad_frees(), kalloc_poison_errors());
        return 1;
    }
    return 0;
}

// A load that must be refused with want and change nothing.
static int bad_case(const char *what, int want)
{
    struct host_snapshot before;
    struct process *caller = setup(&before);
    struct process *out = untouched;
    int res = process_load_for_slot("0:/bad.elf", &out, 1);
    if (res != want)
    {
        say("%s: process_load_for_slot returned %d, expected %d%s\n", what, res, want,
            res == 0 && process_get(1) && process_get(1)->filetype == PROCESS_FILETYPE_BINARY ?
                " (loaded as a flat binary)" : "");
        return 1;
    }
    if (check_guard(what))
    {
        return 1;
    }
    return check_unchanged(what, &before, out, caller);
}

// A file without the ELF magic loads as a flat binary.
static int flat_case(const char *what)
{
    struct host_snapshot before;
    setup(&before);
    struct process *out = untouched;
    int res = process_load_for_slot("0:/flat.bin", &out, 1);
    if (res != 0 || out == untouched || !out || out->filetype != PROCESS_FILETYPE_BINARY ||
        kalloc_size(out->ptr) != file_size || memcmp(out->ptr, file, file_size) != 0)
    {
        say("%s: returned %d, not loaded as a flat binary\n", what, res);
        return 1;
    }
    return 0;
}

enum fault
{
    FAULT_ALLOC,
    FAULT_MAP,
};

// Allocation or mapping n of the good load fails.
static int fault_case(int fault, int n)
{
    struct host_snapshot before;
    struct process *caller = setup(&before);
    struct process *out = untouched;
    char what[100];
    int first = kalloc_calls() + 1;
    if (fault == FAULT_ALLOC)
    {
        kalloc_fail_at(kalloc_calls() + n);
    }
    else
    {
        host_fail_map_at = n;
    }
    int res = process_load_for_slot("0:/good.elf", &out, 1);
    int failed = fault == FAULT_ALLOC ? kalloc_failed() : host_failed;
    host_snprintf(what, sizeof(what), "good file, %s %d fails", fault == FAULT_ALLOC ? "allocation" : "mapping", n);
    if (!failed)
    {
        return res == 0 ? NOT_REACHED : (say("%s: returned %d although nothing failed\n", what, res), 1);
    }
    if (fault == FAULT_ALLOC)
    {
        host_snprintf(what + strlen(what), sizeof(what) - strlen(what), " (in %s)", kalloc_caller(first + n - 1));
    }
    if (res >= 0)
    {
        say("%s: returned %d, expected an error\n", what, res);
        return 1;
    }
    return check_unchanged(what, &before, out, caller);
}

// The malformed files: each is the good file with one change. Returns its name, NULL past the end.
static const char *make_bad(int which)
{
    make_good();
    struct elf_header *h = header();
    switch (which)
    {
    case 0: h->e_ident[EI_CLASS] = ELFCLASSNONE; return "class 0";
    case 1: h->e_ident[EI_CLASS] = ELFCLASS64; return "64-bit class";
    case 2: h->e_ident[EI_DATA] = ELFDATANONE; return "encoding 0";
    case 3: h->e_ident[EI_DATA] = ELFDATA2MSB; return "big-endian";
    case 4: h->e_ident[6] = 0; return "identification version 0";
    case 5: h->e_version = 0; return "e_version 0";
    case 6: h->e_machine = 62; return "machine x86-64";
    case 7: h->e_type = ET_DYN; return "ET_DYN";
    case 8: h->e_type = ET_REL; return "ET_REL";
    case 9: h->e_ehsize = 64; return "e_ehsize 64";
    case 10: h->e_phentsize = 56; return "e_phentsize 56";
    case 11: h->e_phnum = 0; return "no program headers";
    case 12: h->e_phoff = 0; return "e_phoff 0 (the table would be the ELF header)";
    case 13: h->e_phoff = file_size; return "program header table at the end of the file";
    case 14: h->e_phoff = file_size - 32; return "program header table runs past the end";
    case 15: h->e_phoff = 0xFFFFFFE0u; return "e_phoff + table size wraps";
    case 16: h->e_phnum = 0xFFFF; return "e_phnum 65535";
    case 17: h->e_entry = 0x403010; return "entry in a segment without PF_X";
    case 18: h->e_entry = 0x401800; return "entry just past the code segment";
    case 19: h->e_entry = WINDOW_START - 0x10; return "entry below the load window";
    case 20: phdr(0)->p_vaddr = WINDOW_START - PAGE; h->e_entry = WINDOW_START - PAGE + 0x10;
        return "segment below the load window";
    case 21: phdr(4)->p_vaddr = WINDOW_END - 0x800; phdr(4)->p_memsz = 0x1000; return "segment ends past the load window";
    case 22: phdr(4)->p_vaddr = WINDOW_END; phdr(4)->p_offset = 0x3000; return "segment at the end of the load window";
    case 23: phdr(4)->p_vaddr = 0xFFFFF800u; phdr(4)->p_memsz = 0x1000; return "p_vaddr + p_memsz wraps";
    case 24: phdr(2)->p_offset = 0xFFFFF010u; return "p_offset past the end of the file";
    case 25: phdr(2)->p_filesz = 0x1000; phdr(2)->p_memsz = 0x1000; return "file data runs past the end of the file";
    case 26: phdr(2)->p_filesz = 0xFFFFFFF0u; phdr(2)->p_memsz = 0xFFFFFFF0u; return "p_offset + p_filesz wraps";
    case 27: phdr(2)->p_filesz = 0x200; phdr(2)->p_memsz = 0x100; return "p_filesz > p_memsz";
    case 28: phdr(2)->p_offset = 0x3020; return "p_offset and p_vaddr on different page offsets";
    case 29: phdr(0)->p_align = 0x3000; return "p_align not a power of two";
    case 30: phdr(0)->p_align = 0x10000; return "p_offset and p_vaddr not congruent modulo p_align";
    case 31: phdr(3)->p_vaddr = 0x403800; phdr(3)->p_offset = 0x800; return "segments overlap";
    case 32: phdr(3)->p_vaddr = 0x403910; phdr(3)->p_offset = 0x910; return "segments touch inside one page";
    case 33: phdr(3)->p_vaddr = 0x402000; return "segments not in address order";
    case 34: phdr(1)->p_type = PT_INTERP; return "PT_INTERP";
    case 35: phdr(1)->p_type = PT_DYNAMIC; return "PT_DYNAMIC";
    case 36:
        for (int i = 0; i < h->e_phnum; i++)
        {
            if (phdr(i)->p_type == PT_LOAD)
            {
                phdr(i)->p_type = PT_NOTE;
            }
        }
        return "no PT_LOAD";
    case 37:
        phdr(0)->p_memsz = 0;
        phdr(0)->p_filesz = 0;
        return "code segment empty";
    case 38: phdr(4)->p_memsz = 0xFFFFF000u; return "p_vaddr + p_memsz wraps back into the window";
    case 39:
        phdr(2)->p_offset = 0xFFFFF010u;
        phdr(2)->p_filesz = 0x1000;
        phdr(2)->p_memsz = 0x1000;
        return "p_offset + p_filesz wraps back into the file";
    case 40: h->e_phoff = file_size - 6 * 32; return "program header table one entry past the end";
    }
    return NULL;
}

static int run(int (*fn)(int, int), int a, int b, const char *what)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        int rc = fn(a, b);
        fflush(NULL);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    say("%s: crashed (signal %d)\n", what, WTERMSIG(status));
    return 1;
}

static char **real_files;

static const char *const guard_names[] = {
    "",
    ", file buffer right after a guard page",
    ", file buffer right before a guard page",
};

static int run_good(int which, int where)
{
    if (which < 0)
    {
        char what[120];
        make_good();
        guard = where;
        host_snprintf(what, sizeof(what), "made-up program with 4 loaded segments%s", guard_names[where]);
        return good_case(what);
    }
    int fd = open(real_files[which], O_RDONLY);
    file_size = fd < 0 ? -1 : read(fd, file, sizeof(file));
    if (file_size < 52 || file_size == (int)sizeof(file))
    {
        printf("%s: cannot read it, or it is not 52 to %d bytes\n", real_files[which], MAX_FILE - 1);
        return 1;
    }
    return good_case(real_files[which]);
}

static int run_bad(int which, int where)
{
    char what[160];
    const char *name = make_bad(which);
    guard = where;
    host_snprintf(what, sizeof(what), "malformed: %s%s", name, guard_names[where]);
    return bad_case(what, -EINVARG);
}

static int run_short(int length, int where)
{
    char what[120];
    make_good();
    file_size = length;
    guard = where;
    host_snprintf(what, sizeof(what), "the first %d bytes of the good file%s", length, guard_names[where]);
    return bad_case(what, -EINVARG);
}

static int run_flat(int which, int unused)
{
    static const char text[] = "A flat binary: not an ELF file, so the loader falls back to loading it as it is.";
    memset(file, 0, sizeof(file));
    if (which == 0)
    {
        memcpy(file, (void *)text, sizeof(text));
        file_size = sizeof(text);
        return flat_case("flat binary");
    }
    make_good();
    file[3] = 'f';
    file_size = 200;
    return flat_case("flat binary that starts like ELF magic");
}

static int run_fault(int fault, int n)
{
    make_good();
    return fault_case(fault, n);
}

int main(int argc, char **argv)
{
    int cases = 0;
    int failed = 0;
    int reached[2] = {};

    int fd = argc > 1 ? open(argv[1], O_RDONLY) : -1;
    shell_size = fd < 0 ? -1 : read(fd, shell, sizeof(shell));
    if (shell_size < 52 || shell_size == (int)sizeof(shell))
    {
        printf("usage: elf-segments <file.elf>... (the first one is also loaded as the caller)\n");
        return 2;
    }
    real_files = argv + 1;

    cases++;
    failed += run(run_good, -1, KALLOC_GUARD_NONE, "made-up program") != 0;
    for (int i = 0; i < argc - 1; i++)
    {
        cases++;
        failed += run(run_good, i, KALLOC_GUARD_NONE, argv[i + 1]) != 0;
    }
    for (int where = KALLOC_GUARD_NONE; where <= KALLOC_GUARD_ABOVE; where++)
    {
        if (where != KALLOC_GUARD_NONE)
        {
            cases++;
            failed += run(run_good, -1, where, "made-up program") != 0;
        }
        for (int which = 0; make_bad(which); which++)
        {
            cases++;
            quiet = failed >= 12;
            failed += run(run_bad, which, where, make_bad(which)) != 0;
        }
        for (int length = 4; length < 52; length++)
        {
            cases++;
            quiet = failed >= 12;
            failed += run(run_short, length, where, "short file") != 0;
        }
    }
    for (int which = 0; which < 2; which++)
    {
        cases++;
        failed += run(run_flat, which, 0, "flat binary") != 0;
    }
    for (int fault = FAULT_ALLOC; fault <= FAULT_MAP; fault++)
    {
        for (int n = 1; n <= 64; n++)
        {
            quiet = failed >= 12;
            int rc = run(run_fault, fault, n, "fault");
            if (rc == NOT_REACHED)
            {
                break;
            }
            cases++;
            reached[fault]++;
            failed += rc != 0;
        }
    }
    // The good made-up load allocates at least the process, the ELF object, the file, the
    // pointer array, 4 segments, the stack, the task and the page directory, and maps 4 segments
    // and the stack.
    if (reached[FAULT_ALLOC] < 11 || reached[FAULT_MAP] < 5)
    {
        printf("elf-segments: too few failure points reached (allocations %d, mappings %d)\n", reached[FAULT_ALLOC],
               reached[FAULT_MAP]);
        failed++;
    }
    printf("elf-segments: failure points: %d allocations, %d mappings\n", reached[FAULT_ALLOC], reached[FAULT_MAP]);
    printf("elf-segments host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
