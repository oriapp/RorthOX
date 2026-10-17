/*
 * Host check of elf_load() in kernel/loader/formats/elfloader.c (scripts/tests/elf-unwind.sh).
 *
 * Builds the real elfloader.c, and process_load_elf()/process_load_data() cut out of
 * kernel/task/process.c, with the host C compiler. The file calls (fopen, fstat, fread, fclose)
 * are stubs that serve one file from memory and can fail; the heap is scripts/tests/host/kalloc.c.
 * Every failing elf_load() must return an error, leave *file_out alone, close the file and leave
 * nothing allocated. A failed open is -ENOENT when the file is missing and -EIO for any other
 * error (-EINVARG, -ENOMEM). A file that is not an ELF file must be gone from the heap before
 * process_load_data() falls back to loading it as a flat binary; a file with the ELF magic that
 * is not a supported ELF file (here a 64-bit one) is -EINVARG and is never tried as a flat binary. A good ELF file keeps
 * its object, its buffer, the pointer array and one zeroed allocation per loaded segment (and
 * nothing else) until elf_close(). A short fread (0 items, with nothing or all but the last byte
 * copied) is -EIO before any header is read, and is not tried as a flat binary. Each case runs in
 * its own process.
 *
 * Usage: elf-load <blank.elf>
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "string/string.h"
#include "memory/memory.h"
#include "fs/file.h"
#include "loader/formats/elfloader.h"
#include "task/process.h"

#define MAX_FILE (64 * 1024)
#define FD 3

static unsigned char content[MAX_FILE];
static int content_size;
static unsigned char blank[MAX_FILE];
static int blank_size;
// What fopen_status() returns instead of opening the file (0: it opens it).
static int fail_open;
static int fail_stat;
static int fail_read;
// 1: fread returns 0 items and copies nothing; 2: it copies all but the last byte and returns 0.
static int short_read;
static int is_open;
static int opens;
static uint32_t position;

#define say printf

// The file layer: one file, served from content[].
int fopen_status(const char *filename, const char *mode_str)
{
    if (fail_open)
    {
        return fail_open;
    }
    opens++;
    is_open = 1;
    position = 0;
    return FD;
}

int fopen(const char *filename, const char *mode_str)
{
    int res = fopen_status(filename, mode_str);
    return res > 0 ? res : 0;
}

int fstat(int fd, struct file_stat *stat)
{
    if (fd != FD || !is_open || fail_stat)
    {
        return -EIO;
    }
    stat->filesize = content_size;
    stat->flags = 0;
    return 0;
}

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd)
{
    if (fd != FD || !is_open || fail_read)
    {
        return -EIO;
    }
    if (short_read)
    {
        if (short_read == 2 && size * nmemb > 1)
        {
            memcpy(ptr, content, size * nmemb - 1 < (uint32_t)content_size ? (int)(size * nmemb - 1) : content_size);
        }
        return 0;
    }
    if (!size || nmemb > ((uint32_t)content_size - position) / size) return 0;
    memcpy(ptr, content + position, size * nmemb);
    position += size * nmemb;
    return nmemb;
}

int fclose(int fd)
{
    if (fd != FD || !is_open)
    {
        return -EIO;
    }
    is_open = 0;
    return 0;
}

// process_load_data() falls back to this for files that are not ELF files.
static int binary_calls;
static int binary_live_at_entry;
static int binary_open_at_entry;

static int process_load_binary(const char *filename, struct process *process)
{
    binary_calls++;
    binary_live_at_entry = kalloc_live();
    binary_open_at_entry = is_open;
    process->filetype = PROCESS_FILETYPE_BINARY;
    return 0;
}

#include "process-load-data.c"

static struct elf_file *const untouched = (struct elf_file *)0x1234;

// After a good load exactly these are live: the ELF object, the file buffer, the pointer array,
// and for each PT_LOAD with p_memsz > 0 one allocation of its whole pages (from the page of
// p_vaddr) holding p_filesz bytes of the file at p_vaddr % 4096 and zeroes everywhere else.
static int owned_allocations(const char *what, struct elf_file *out)
{
    const struct elf_header *header = (const struct elf_header *)blank;
    const struct elf32_phdr *phdrs = (const struct elf32_phdr *)(blank + header->e_phoff);
    int loads = 0;
    char used[64] = {};
    int live = kalloc_live();
    if (live > (int)sizeof(used))
    {
        say("%s: %d allocations live\n", what, live);
        return 1;
    }
    for (int i = 0; i < header->e_phnum; i++)
    {
        const struct elf32_phdr *phdr = &phdrs[i];
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
        {
            continue;
        }
        loads++;
        uint32_t offset = phdr->p_vaddr % 4096;
        long span = (long)((offset + phdr->p_memsz + 4095) / 4096 * 4096);
        int found = -1;
        for (int j = 0; j < live && found < 0; j++)
        {
            unsigned char *base = kalloc_live_at(j);
            if (used[j] || base == (unsigned char *)out || base == out->elf_memory || kalloc_size(base) != span)
            {
                continue;
            }
            int same = memcmp(base + offset, blank + phdr->p_offset, phdr->p_filesz) == 0;
            for (long k = 0; same && k < span; k++)
            {
                if ((k < offset || k >= offset + phdr->p_filesz) && base[k] != 0)
                {
                    same = 0;
                }
            }
            if (same)
            {
                found = j;
            }
        }
        if (found < 0)
        {
            say("%s: no live %ld-byte allocation holds program header %d's segment (file bytes, then zeroes)\n",
                what, span, i);
            return 1;
        }
        used[found] = 1;
    }
    if (live != 3 + loads)
    {
        say("%s: %d allocations live, expected %d (ELF object, file buffer, pointer array, %d segments)\n", what,
            live, 3 + loads, loads);
        return 1;
    }
    return 0;
}

static void serve(const void *data, int size)
{
    memcpy(content, (void *)data, size);
    content_size = size;
}

static int clean_failure(const char *what, int res, int want, struct elf_file *out)
{
    if (res >= 0 || (want && res != want))
    {
        say("%s: elf_load returned %d, expected %d\n", what, res, want ? want : -1);
        return 1;
    }
    if (out != untouched)
    {
        say("%s: *file_out was changed\n", what);
        return 1;
    }
    if (is_open)
    {
        say("%s: the file was left open\n", what);
        return 1;
    }
    if (kalloc_live() != 0)
    {
        say("%s: %d allocations left (ELF object and file buffer must be freed)\n", what, kalloc_live());
        return 1;
    }
    if (kalloc_bad_frees() || kalloc_poison_errors())
    {
        say("%s: bad kfree or write after kfree\n", what);
        return 1;
    }
    return 0;
}

enum
{
    OBJECT_ALLOC,
    OPEN,
    OPEN_NOMEM,
    OPEN_MISSING,
    STAT,
    BUFFER_ALLOC,
    READ,
    SHORT_READ,
    SHORT_TAIL,
    NOT_ELF,
    BAD_CLASS,
    GOOD,
    FALLBACK,
    NO_FALLBACK,
    NO_FALLBACK_SHORT,
    NO_FALLBACK_CLASS,
    CASE_COUNT
};

static const char *case_name[] = { "ELF object allocation fails", "fopen fails with -EINVARG", "fopen fails with -ENOMEM",
                                   "fopen finds no such file", "fstat fails",
                                   "file buffer allocation fails", "fread fails",
                                   "fread reads 0 items and nothing", "fread reads 0 items, all but the last byte",
                                   "not an ELF file",
                                   "64-bit ELF file", "good ELF file", "fallback to a flat binary",
                                   "no fallback after fopen fails", "no fallback after a short fread",
                                   "no fallback for a 64-bit ELF file" };

static int run_case(int which)
{
    struct elf_file *out = untouched;
    const char *what = case_name[which];
    static const char not_elf[] = "This is a flat binary, not an ELF file. It is long enough for a header.....";
    kalloc_init();
    serve(blank, blank_size);

    switch (which)
    {
    case OBJECT_ALLOC:
        kalloc_fail_at(1);
        return clean_failure(what, elf_load("0:/blank.elf", &out), -ENOMEM, out);
    case OPEN:
        fail_open = -EINVARG;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EIO, out);
    case OPEN_NOMEM:
        fail_open = -ENOMEM;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EIO, out);
    case OPEN_MISSING:
        fail_open = -ENOENT;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -ENOENT, out);
    case STAT:
        fail_stat = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), 0, out);
    case BUFFER_ALLOC:
        kalloc_fail_at(2);
        return clean_failure(what, elf_load("0:/blank.elf", &out), -ENOMEM, out);
    case READ:
        fail_read = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), 0, out);
    case SHORT_READ:
        short_read = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EIO, out);
    case SHORT_TAIL:
        short_read = 2;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EIO, out);
    case NOT_ELF:
        serve(not_elf, sizeof(not_elf));
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EINFORMAT, out);
    case BAD_CLASS:
        content[EI_CLASS] = ELFCLASS64;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EINVARG, out);
    case GOOD:
    {
        int res = elf_load("0:/blank.elf", &out);
        if (res != 0 || out == untouched || !out)
        {
            say("%s: elf_load returned %d\n", what, res);
            return 1;
        }
        if (is_open || !kalloc_is_live(out) || !kalloc_is_live(out->elf_memory) ||
            memcmp(out->elf_memory, blank, blank_size) != 0)
        {
            say("%s: expected the file closed and the ELF object and its buffer kept\n", what);
            return 1;
        }
        if (owned_allocations(what, out))
        {
            return 1;
        }
        elf_close(out);
        if (kalloc_live() != 0 || kalloc_bad_frees())
        {
            say("%s: elf_close left %d allocations\n", what, kalloc_live());
            return 1;
        }
        return 0;
    }
    case FALLBACK:
    {
        static struct process process;
        serve(not_elf, sizeof(not_elf));
        int res = process_load_data("0:/blank.elf", &process);
        if (res != 0 || binary_calls != 1 || process.filetype != PROCESS_FILETYPE_BINARY)
        {
            say("%s: returned %d, flat binary loader called %d times\n", what, res, binary_calls);
            return 1;
        }
        if (binary_live_at_entry != 0 || binary_open_at_entry)
        {
            say("%s: %d allocations of the failed ELF load%s still there when the binary load started\n", what,
                binary_live_at_entry, binary_open_at_entry ? " and its open file" : "");
            return 1;
        }
        return 0;
    }
    case NO_FALLBACK:
    {
        static struct process process;
        fail_open = -EINVARG;
        int res = process_load_data("0:/blank.elf", &process);
        if (res >= 0 || binary_calls != 0)
        {
            say("%s: returned %d, flat binary loader called %d times\n", what, res, binary_calls);
            return 1;
        }
        return kalloc_live() == 0 ? 0 : (say("%s: %d allocations left\n", what, kalloc_live()), 1);
    }
    case NO_FALLBACK_CLASS:
    {
        static struct process process;
        content[EI_CLASS] = ELFCLASS64;
        int res = process_load_data("0:/blank.elf", &process);
        if (res != -EINVARG || binary_calls != 0)
        {
            say("%s: returned %d, flat binary loader called %d times\n", what, res, binary_calls);
            return 1;
        }
        return kalloc_live() == 0 && !is_open ? 0 : (say("%s: %d allocations left\n", what, kalloc_live()), 1);
    }
    case NO_FALLBACK_SHORT:
    {
        static struct process process;
        short_read = 1;
        int res = process_load_data("0:/blank.elf", &process);
        if (res != -EIO || binary_calls != 0)
        {
            say("%s: returned %d, flat binary loader called %d times\n", what, res, binary_calls);
            return 1;
        }
        return kalloc_live() == 0 && !is_open ? 0 : (say("%s: %d allocations left\n", what, kalloc_live()), 1);
    }
    }
    return 1;
}

int main(int argc, char **argv)
{
    int failures = 0;
    if (argc != 2)
    {
        printf("usage: elf-load <blank.elf>\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < (int)sizeof(struct elf_header) || blank_size == (int)sizeof(blank))
    {
        printf("elf-load: cannot read %s, or it is not 1 to %d bytes\n", argv[1], MAX_FILE - 1);
        return 2;
    }

    for (int which = 0; which < CASE_COUNT; which++)
    {
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 2;
        }
        if (pid == 0)
        {
            int rc = run_case(which);
            fflush(NULL);
            _exit(rc);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            continue;
        }
        if (WIFSIGNALED(status))
        {
            printf("%s: crashed (signal %d)\n", case_name[which], WTERMSIG(status));
        }
        failures++;
    }
    printf("elf-load host check: %d of %d cases failed\n", failures, CASE_COUNT);
    return failures ? 1 : 0;
}
