/*
 * Host check of elf_load() in kernel/loader/formats/elfloader.c (scripts/tests/elf-unwind.sh).
 *
 * Builds the real elfloader.c, and process_load_elf()/process_load_data() cut out of
 * kernel/task/process.c, with the host C compiler. The file calls (fopen, fstat, fread, fclose)
 * are stubs that serve one file from memory and can fail; the heap is scripts/tests/host/kalloc.c.
 * Every failing elf_load() must return an error, leave *file_out alone, close the file and leave
 * nothing allocated. A file that is not an ELF file must be gone from the heap before
 * process_load_data() falls back to loading it as a flat binary. A good ELF file keeps its
 * object and its buffer until elf_close(). Each case runs in its own process.
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
static int fail_open;
static int fail_stat;
static int fail_read;
static int is_open;
static int opens;

#define say printf

// The file layer: one file, served from content[].
int fopen(const char *filename, const char *mode_str)
{
    if (fail_open)
    {
        return 0;
    }
    opens++;
    is_open = 1;
    return FD;
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
    memcpy(ptr, content, size * nmemb < (uint32_t)content_size ? (int)(size * nmemb) : content_size);
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
    STAT,
    BUFFER_ALLOC,
    READ,
    NOT_ELF,
    BAD_CLASS,
    GOOD,
    FALLBACK,
    NO_FALLBACK,
    CASE_COUNT
};

static const char *case_name[] = { "ELF object allocation fails", "fopen fails", "fstat fails",
                                   "file buffer allocation fails", "fread fails", "not an ELF file",
                                   "64-bit ELF file", "good ELF file", "fallback to a flat binary",
                                   "no fallback after fopen fails" };

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
        fail_open = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), 0, out);
    case STAT:
        fail_stat = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), 0, out);
    case BUFFER_ALLOC:
        kalloc_fail_at(2);
        return clean_failure(what, elf_load("0:/blank.elf", &out), -ENOMEM, out);
    case READ:
        fail_read = 1;
        return clean_failure(what, elf_load("0:/blank.elf", &out), 0, out);
    case NOT_ELF:
        serve(not_elf, sizeof(not_elf));
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EINFORMAT, out);
    case BAD_CLASS:
        content[EI_CLASS] = ELFCLASS64;
        return clean_failure(what, elf_load("0:/blank.elf", &out), -EINFORMAT, out);
    case GOOD:
    {
        int res = elf_load("0:/blank.elf", &out);
        if (res != 0 || out == untouched || !out)
        {
            say("%s: elf_load returned %d\n", what, res);
            return 1;
        }
        if (is_open || kalloc_live() != 2 || !kalloc_is_live(out) || !kalloc_is_live(out->elf_memory) ||
            memcmp(out->elf_memory, blank, blank_size) != 0)
        {
            say("%s: expected the file closed and the ELF object and its buffer kept (%d allocations)\n", what,
                kalloc_live());
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
        fail_open = 1;
        int res = process_load_data("0:/blank.elf", &process);
        if (res >= 0 || binary_calls != 0)
        {
            say("%s: returned %d, flat binary loader called %d times\n", what, res, binary_calls);
            return 1;
        }
        return kalloc_live() == 0 ? 0 : (say("%s: %d allocations left\n", what, kalloc_live()), 1);
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
