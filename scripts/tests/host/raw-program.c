/* Real process/ELF loading: size, allocation ordering, prefix and tail I/O. */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "task/process.h"
#include "loader/formats/elfloader.h"

#define WINDOW (RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT - RORTHOX_PROGRAM_VIRTUAL_ADDRESS)
static unsigned char content[3 * 1024 * 1024];
static unsigned char blank[65536];
static int blank_size;
static uint32_t watched;
static int requests;
void *__real_kzalloc(size_t);
void *__wrap_kzalloc(size_t size)
{
    if (watched && size == watched) requests++;
    /* Extreme advertised lengths must not exhaust the host or mask ordering. */
    if (size > sizeof(content)) return 0;
    return __real_kzalloc(size);
}
static const uint32_t lengths[] = {0, 1, 3, 4, WINDOW-1, WINDOW, WINDOW+1, 2621440,
                                  0x7fffffffU, 0xfffff001U, 0xffffffffU};
static int one(int n)
{
    kalloc_init(); host_reset();
    host_file_serve(blank, blank_size);
    host_load_caller();
    struct host_snapshot before, after;
    char message[200] = "output pointer changed";
    host_reset();
    if (n < 11)
    {
        uint32_t size = lengths[n];
        memset(content, 0x90, sizeof(content));
        host_file_serve(content, size <= sizeof(content) ? size : 4);
        host_file_size(size);
        watched = size;
    }
    else
    {
        memcpy(content, blank, blank_size);
        host_file_serve(content, n == 11 ? WINDOW + 1 : blank_size);
        if (n == 12) { content[4] = 2; host_file_serve(content, blank_size); }
        if (n == 13 || n == 14) host_fail_read_at = n - 12;
        if (n == 15 || n == 16) host_short_read_at = n - 14;
        if (n == 17) { host_file_serve("\177ELF", 4); }
        if (n == 18 || n == 19) { host_fail_read_at = n - 17; host_read_error = -ENOMEM; }
    }
    host_snapshot_take(&before, message, sizeof(message));
    struct process *out = (void *)0x1234;
    int rc = process_load("0:/case.bin", &out);
    int invalid = n < 11 && (lengths[n] == 0 || lengths[n] > WINDOW);
    int want = invalid || n == 12 || n == 17 ? -EINVARG : n >= 13 && n <= 16 ? -EIO : n >= 18 ? -ENOMEM : 0;
    int bad = rc != want;
    printf("raw case %d: size=%u result=%d expected=%d file allocations=%d read bytes=%u\n",
           n, n < 11 ? lengths[n] : 0, rc, want, requests, host_read_bytes);
    if (invalid && (requests || host_read_bytes > 4)) bad = 1;
    if (want < 0)
    {
        host_snapshot_take(&after, message, sizeof(message));
        if (out != (void *)0x1234 || host_snapshot_compare(&before, &after, message, sizeof(message)))
        { printf("rollback: %s\n", message); bad = 1; }
    }
    else if (!rc)
    {
        if (n < 11 && (out->filetype != PROCESS_FILETYPE_BINARY || out->size != lengths[n] ||
                      memcmp(out->ptr, content, lengths[n]))) bad = 1;
        if (n == 11 && (out->filetype != PROCESS_FILETYPE_ELF ||
                       memcmp(out->elf_file->elf_memory, content, WINDOW+1))) bad = 1;
    }
    if (host_open_files || kalloc_bad_frees() || kalloc_poison_errors()) bad = 1;
    return bad;
}
int main(int argc, char **argv)
{
    int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < 52 || blank_size == sizeof(blank)) return 2;
    close(fd);
    int failed = 0;
    for (int n = 0; n < 20; n++)
    {
        fflush(0); int pid = fork();
        if (!pid) { int rc = one(n); fflush(0); _exit(rc); }
        int status; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { printf("FAIL raw case %d status=%d\n", n, status); failed++; }
    }
    printf("raw-program host: %d of 20 failed\n", failed);
    return failed != 0;
}
