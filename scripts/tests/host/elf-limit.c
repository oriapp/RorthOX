/*
 * Host check for scripts/tests/elf-limit.sh: an ELF file above ELF_FILE_LIMIT_BYTES (config.mk) is
 * refused with -ENOMEM before its buffer is allocated and before anything past the ELF magic is
 * read.
 *
 * Builds the real kernel/loader/formats/elfloader.c, kernel/task/process.c, kernel/task/task.c
 * and kernel/isr80h/process.c with the host C compiler, with the stand-ins of
 * scripts/tests/host/process-host.c and the heap of scripts/tests/host/kalloc.c. kzalloc, kmalloc
 * and fread are wrapped (-Wl,--wrap) to keep a ledger of the load: every allocation request and
 * every read, in order. fread serves the file from a buffer here, so a file of the limit and
 * above can be served in full; fstat reports the size set with host_file_size(), which can be far
 * more than the bytes there are.
 *
 * Refused loads: elf_load() and process_load() return -ENOMEM, leave their output pointer alone,
 * close the file and free everything (process_load: the task list, slots and heap as before), the
 * first read is at most an ELF header, no allocation request is above EARLY_REQUEST_LIMIT (only the
 * loader's own objects come before the refusal), and after the first read nothing is allocated or
 * read. The file is
 * opened once: a too-large ELF file is never tried as a flat binary. A file of exactly the limit
 * loads, its buffer is the limit, and every byte is read. A file without the ELF magic above the
 * limit still goes to the flat binary loader and is refused there by the load window (-EINVARG).
 * Each case runs in its own process with a time limit.
 *
 * Usage: elf-limit <blank.elf>
 * Prints a ledger line and "PASS <case>" or "FAIL <case>: ..." per case, then
 * "elf-limit host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "memory/paging/paging.h"
#include "task/process.h"
#include "loader/formats/elfloader.h"

#ifndef ELF_FILE_LIMIT_BYTES
#error "ELF_FILE_LIMIT_BYTES is missing from the build configuration"
#endif

unsigned int alarm(unsigned int seconds);

#define LIMIT ((uint32_t)ELF_FILE_LIMIT_BYTES)
#define WINDOW ((uint32_t)(RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT - RORTHOX_PROGRAM_VIRTUAL_ADDRESS))
// The host heap (kalloc.c) is 64 MiB; bigger requests are counted and refused, not tried.
#define HOST_HEAP_ROOM (48u * 1024 * 1024)
// A refused load asks only for objects such as struct elf_file and struct process, none this big.
#define EARLY_REQUEST_LIMIT (64u * 1024)
// The bytes served: enough for the limit plus one page.
#define SERVED (LIMIT + 4096u)

static unsigned char file_data[SERVED];
static uint32_t file_length;
static uint32_t file_position;
static int file_open_seen;
static unsigned char blank[65536];
static int blank_size;

// The ledger of the load under test.
static int recording;
static int reads;                 // fread calls
static unsigned long long first_read;   // bytes asked for by the first fread
static unsigned long long read_bytes;   // bytes copied by fread
static int late_allocations;      // allocation requests after the first read
static unsigned long long late_allocation_size;
static int late_reads;            // fread calls after the first
static unsigned long long largest_request;
static unsigned long long file_sized_requests; // requests of the advertised size or more
static uint32_t advertised;

static void ledger_reset(void)
{
    reads = late_allocations = late_reads = 0;
    first_read = read_bytes = late_allocation_size = largest_request = file_sized_requests = 0;
}

static void note_allocation(size_t size)
{
    if (!recording)
    {
        return;
    }
    if (size > largest_request)
    {
        largest_request = size;
    }
    if (advertised && size >= advertised)
    {
        file_sized_requests++;
    }
    if (reads)
    {
        late_allocations++;
        late_allocation_size = size;
    }
}

void *__real_kzalloc(size_t size);
void *__real_kmalloc(size_t size);
void *__wrap_kzalloc(size_t size)
{
    note_allocation(size);
    return size > HOST_HEAP_ROOM ? 0 : __real_kzalloc(size);
}
void *__wrap_kmalloc(size_t size)
{
    note_allocation(size);
    return size > HOST_HEAP_ROOM ? 0 : __real_kmalloc(size);
}

// The file layer's fread, serving file_data[0..file_length); a new fopen starts at offset 0.
int __wrap_fread(void *ptr, uint32_t size, uint32_t nmemb, int fd)
{
    unsigned long long bytes = (unsigned long long)size * nmemb;
    if (file_open_seen != host_open_calls)
    {
        file_open_seen = host_open_calls;
        file_position = 0;
    }
    if (recording)
    {
        if (!reads)
        {
            first_read = bytes;
        }
        else
        {
            late_reads++;
        }
        reads++;
    }
    if (fd <= 0 || host_open_files <= 0)
    {
        return -EIO;
    }
    if (!size || bytes > file_length - file_position)
    {
        return 0;
    }
    memcpy(ptr, file_data + file_position, bytes);
    file_position += bytes;
    if (recording)
    {
        read_bytes += bytes;
    }
    return nmemb;
}

// Serve blank.elf padded with zeroes to <served> bytes (at most SERVED), and report <size> as the
// file size. elf = 0 replaces the ELF magic.
static void serve(uint32_t served, uint32_t size, int elf)
{
    memset(file_data, 0, sizeof(file_data));
    memcpy(file_data, blank, blank_size);
    if (!elf)
    {
        memcpy(file_data, "RAW!", 4);
    }
    file_length = served;
    file_open_seen = -1;
    host_file_size(size);
    advertised = size;
}

enum
{
    ELF_AT_LIMIT,
    ELF_LIMIT_PLUS_1,
    ELF_LIMIT_PLUS_PAGE,
    ELF_2GIB,
    ELF_4GIB_MINUS_1,
    PROCESS_AT_LIMIT,
    PROCESS_LIMIT_PLUS_1,
    PROCESS_4GIB_MINUS_1,
    RAW_LIMIT_PLUS_1,
    CASE_COUNT
};

static const char *const case_name[] = {
    "elf_load-at-limit", "elf_load-limit+1", "elf_load-limit+page", "elf_load-2GiB", "elf_load-4GiB-1",
    "process_load-at-limit", "process_load-limit+1", "process_load-4GiB-1", "raw-limit+1",
};

static uint32_t case_size(int which)
{
    switch (which)
    {
    case ELF_AT_LIMIT:
    case PROCESS_AT_LIMIT:
        return LIMIT;
    case ELF_LIMIT_PLUS_1:
    case PROCESS_LIMIT_PLUS_1:
    case RAW_LIMIT_PLUS_1:
        return LIMIT + 1;
    case ELF_LIMIT_PLUS_PAGE:
        return LIMIT + 4096;
    case ELF_2GIB:
        return 0x80000000u;
    default:
        return 0xffffffffu;
    }
}

#define FAIL(...) do { printf("FAIL %s: ", what); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

static void ledger_line(const char *what, uint32_t size, int res, int want)
{
    printf("ledger %s: size=%u result=%d expected=%d opens=%d first read=%llu read bytes=%llu "
           "reads after it=%d allocations after it=%d largest request=%llu file-sized requests=%llu\n",
           what, size, res, want, host_open_calls, first_read, read_bytes, late_reads, late_allocations,
           largest_request, file_sized_requests);
}

// What every refused ELF load must show in the ledger.
static int refused_early(const char *what)
{
    if (first_read > sizeof(struct elf_header))
        FAIL("the first read asked for %llu bytes, more than an ELF header", first_read);
    if (late_allocations)
        FAIL("%d allocation request(s) after the first read (last: %llu bytes)", late_allocations,
             late_allocation_size);
    if (late_reads)
        FAIL("%d read(s) after the first read, %llu bytes read in all", late_reads, read_bytes);
    if (file_sized_requests)
        FAIL("%llu allocation request(s) of the file size or more", file_sized_requests);
    if (largest_request > EARLY_REQUEST_LIMIT)
        FAIL("an allocation request of %llu bytes, more than %u", largest_request, EARLY_REQUEST_LIMIT);
    if (host_open_calls != 1)
        FAIL("the file was opened %d times (a second open is the flat binary loader)", host_open_calls);
    if (host_open_files)
        FAIL("%d file(s) left open", host_open_files);
    return 0;
}

static int run_elf_load(int which, const char *what)
{
    uint32_t size = case_size(which);
    struct elf_file *const untouched = (struct elf_file *)0x1234;
    struct elf_file *out = untouched;
    kalloc_init();
    host_reset();
    serve(size <= SERVED ? size : SERVED, size, 1);
    ledger_reset();
    recording = 1;
    int res = elf_load("0:/big.elf", &out);
    recording = 0;
    int want = which == ELF_AT_LIMIT ? 0 : -ENOMEM;
    ledger_line(what, size, res, want);
    if (res != want)
        FAIL("elf_load returned %d, expected %d", res, want);
    if (which == ELF_AT_LIMIT)
    {
        if (out == untouched || !out || !kalloc_is_live(out) || !kalloc_is_live(out->elf_memory))
            FAIL("no ELF object and buffer after a good load");
        if (kalloc_size(out->elf_memory) != (long)LIMIT || out->in_memory_size != LIMIT)
            FAIL("buffer of %ld bytes (in_memory_size %u), expected %u", kalloc_size(out->elf_memory),
                 out->in_memory_size, LIMIT);
        if (read_bytes != LIMIT || memcmp(out->elf_memory, file_data, LIMIT))
            FAIL("%llu bytes read, expected the whole file of %u bytes in the buffer", read_bytes, LIMIT);
        if (host_open_files)
            FAIL("the file was left open");
        elf_close(out);
        if (kalloc_live() || kalloc_bad_frees() || kalloc_poison_errors())
            FAIL("elf_close left %d allocations (%d bad frees, %d writes after free)", kalloc_live(),
                 kalloc_bad_frees(), kalloc_poison_errors());
        return 0;
    }
    if (out != untouched)
        FAIL("*file_out was changed");
    if (refused_early(what))
        return 1;
    if (kalloc_live() || kalloc_bad_frees() || kalloc_poison_errors())
        FAIL("%d allocations left (%d bad frees, %d writes after free)", kalloc_live(), kalloc_bad_frees(),
             kalloc_poison_errors());
    return 0;
}

static int run_process_load(int which, const char *what)
{
    uint32_t size = case_size(which);
    char message[200] = "";
    struct host_snapshot before, after;
    kalloc_init();
    host_reset();
    serve(blank_size, blank_size, 1);
    host_file_serve(blank, blank_size);
    host_load_caller();
    host_reset();
    serve(size <= SERVED ? size : SERVED, size, which != RAW_LIMIT_PLUS_1);
    if (!host_snapshot_take(&before, message, sizeof(message)))
        FAIL("task list before the load: %s", message);
    struct process *const untouched = (struct process *)0x1234;
    struct process *out = untouched;
    ledger_reset();
    recording = 1;
    int res = process_load("0:/big.elf", &out);
    recording = 0;
    int want = which == PROCESS_AT_LIMIT ? 0 : which == RAW_LIMIT_PLUS_1 ? -EINVARG : -ENOMEM;
    ledger_line(what, size, res, want);
    if (res != want)
        FAIL("process_load returned %d, expected %d", res, want);
    if (which == PROCESS_AT_LIMIT)
    {
        if (out == untouched || !out || out->filetype != PROCESS_FILETYPE_ELF || !out->elf_file ||
            kalloc_size(out->elf_file->elf_memory) != (long)LIMIT || read_bytes != LIMIT)
            FAIL("expected an ELF process with a %u-byte file buffer and the whole file read", LIMIT);
        return 0;
    }
    if (out != untouched)
        FAIL("the output process pointer was changed");
    if (which == RAW_LIMIT_PLUS_1)
    {
        // Not an ELF file: the flat binary loader opens it again and refuses it by the window.
        if (host_open_calls != 2)
            FAIL("opened %d times, expected 2 (the ELF loader, then the flat binary loader)", host_open_calls);
        if (file_sized_requests)
            FAIL("%llu allocation request(s) of the file size or more", file_sized_requests);
        if (host_open_files)
            FAIL("%d file(s) left open", host_open_files);
    }
    else if (refused_early(what))
    {
        return 1;
    }
    if (!host_snapshot_take(&after, message, sizeof(message)) ||
        host_snapshot_compare(&before, &after, message, sizeof(message)))
        FAIL("not rolled back: %s", message);
    if (kalloc_bad_frees() || kalloc_poison_errors())
        FAIL("%d bad frees, %d writes after free", kalloc_bad_frees(), kalloc_poison_errors());
    return 0;
}

static int run_case(int which)
{
    const char *what = case_name[which];
    alarm(30);
    if (which == ELF_AT_LIMIT || which == PROCESS_AT_LIMIT)
    {
        if (LIMIT + 65536u > HOST_HEAP_ROOM)
        {
            printf("%s: not run, the limit is more than the host heap holds\nPASS %s\n", what, what);
            return 0;
        }
    }
    int bad = which < PROCESS_AT_LIMIT ? run_elf_load(which, what) : run_process_load(which, what);
    if (!bad)
    {
        printf("PASS %s\n", what);
    }
    return bad;
}

int main(int argc, char **argv)
{
    int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < (int)sizeof(struct elf_header) || blank_size == (int)sizeof(blank))
    {
        printf("usage: elf-limit <blank.elf> (1 to %d bytes)\n", (int)sizeof(blank) - 1);
        return 2;
    }
    close(fd);
    if (LIMIT < (uint32_t)blank_size || LIMIT % 4096)
    {
        printf("elf-limit: ELF_FILE_LIMIT_BYTES %u is not a page multiple of at least blank.elf's size\n", LIMIT);
        return 2;
    }
    printf("elf-limit: ELF_FILE_LIMIT_BYTES = %u, load window = %u bytes\n", LIMIT, WINDOW);
    int failed = 0;
    for (int which = 0; which < CASE_COUNT; which++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 2;
        }
        if (!pid)
        {
            int rc = run_case(which);
            fflush(0);
            _exit(rc);
        }
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status))
        {
            if (WIFSIGNALED(status))
            {
                printf("FAIL %s: stopped by signal %d\n", case_name[which], WTERMSIG(status));
            }
            failed++;
        }
    }
    printf("elf-limit host check: %d of %d cases failed\n", failed, CASE_COUNT);
    return failed != 0;
}
