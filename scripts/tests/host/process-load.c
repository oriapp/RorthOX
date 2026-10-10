/*
 * Host check of building a process in kernel/task/process.c (scripts/tests/load-unwind.sh).
 *
 * Builds the real process.c, task.c and loader/formats/elfloader.c with the host C compiler,
 * with the file and paging layers from process-host.c and the heap from kalloc.c. A caller
 * process runs in slot 0 (its task current, its process holding the keyboard). Then
 * process_load_for_slot() loads a child into slot 1 while, in turn, each heap allocation, each
 * fopen/fstat/fread and each mapping fails, for blank.elf and for a flat binary. Every failed
 * load must return an error and leave everything as it was: no process in the slot, the task
 * list, the current task and the keyboard process unchanged, *process untouched, no file open,
 * no allocation left, no bad kfree, no write after kfree, and the child's page directory freed
 * before the memory it maps and never loaded. A load where nothing fails must publish the child
 * completely. Loading into an occupied or invalid slot must fail without allocating. Each case
 * runs in its own process, so a crash fails that case only.
 *
 * Usage: process-load <blank.elf>
 */
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "memory/memory.h"
#include "string/string.h"
#include "task/task.h"
#include "task/process.h"
#include "loader/formats/elfloader.h"

#define MAX_FILE (64 * 1024)
#define MAX_STEPS 64
// Exit status of a case whose chosen failure never happened: the steps of that kind are done.
#define NOT_REACHED 10


static unsigned char blank[MAX_FILE];
static int blank_size;
static const char flat[] = "A flat binary: not an ELF file, so the loader falls back to loading it as it is.";
static char message[200];
static int quiet;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

enum fault
{
    FAULT_NONE,
    FAULT_ALLOC,
    FAULT_OPEN,
    FAULT_STAT,
    FAULT_READ,
    FAULT_MAP,
};
static const char *fault_name[] = { "nothing", "allocation", "fopen", "fstat", "fread", "mapping" };

static struct process *const untouched = (struct process *)0x1234;

static void serve_child(int is_elf)
{
    if (is_elf)
    {
        host_file_serve(blank, blank_size);
    }
    else
    {
        host_file_serve(flat, sizeof(flat));
    }
}

// The caller in slot 0, then the file the child loads. Fills before.
static struct process *setup(int is_elf, struct host_snapshot *before)
{
    kalloc_init();
    host_reset();
    host_file_serve(blank, blank_size);
    struct process *caller = host_load_caller();
    serve_child(is_elf);
    host_reset();
    if (!host_snapshot_take(before, message, sizeof(message)))
    {
        printf("setup: %s\n", message);
        exit(2);
    }
    return caller;
}

// Common checks after a load that must have changed nothing. 0 = pass.
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
    if (host_terminated_prints)
    {
        say("%s: printed a termination message\n", what);
        return 1;
    }
    return 0;
}

static int check_success(const char *what, const struct host_snapshot *before, struct process *out, int is_elf)
{
    struct host_snapshot after;
    if (out == untouched || !out || process_get(1) != out)
    {
        say("%s: the process is not in slot 1\n", what);
        return 1;
    }
    if (!host_snapshot_take(&after, message, sizeof(message)))
    {
        say("%s: %s\n", what, message);
        return 1;
    }
    if (after.task_count != before->task_count + 1 || after.tail != out->task || !kalloc_is_live(out->task) ||
        out->task->process != out)
    {
        say("%s: the new task is not the new tail of the list\n", what);
        return 1;
    }
    if (after.current_task != before->current_task || after.current_process != before->current_process)
    {
        say("%s: the current task or the keyboard process changed\n", what);
        return 1;
    }
    if (out->id != 1 || kalloc_size(out->stack) != USER_PROGRAM_STACK_SIZE || !out->task->page_directory)
    {
        say("%s: id, stack or page directory not set up\n", what);
        return 1;
    }
    if (is_elf ? (out->filetype != PROCESS_FILETYPE_ELF || !kalloc_is_live(out->elf_file))
               : (out->filetype != PROCESS_FILETYPE_BINARY || kalloc_size(out->ptr) != (long)sizeof(flat)))
    {
        say("%s: the program data is not owned by the process\n", what);
        return 1;
    }
    if (host_open_files || kalloc_bad_frees() || kalloc_poison_errors() || host_paging_problems)
    {
        say("%s: file left open, bad kfree, write after kfree or paging problem\n", what);
        return 1;
    }
    return 0;
}

static int load_case(int is_elf, int fault, int n)
{
    struct host_snapshot before;
    struct process *caller = setup(is_elf, &before);
    struct process *out = untouched;
    char what[120];
    int first_call = kalloc_calls() + 1;
    host_snprintf(what, sizeof(what), "%s, %s %d fails", is_elf ? "blank.elf" : "flat binary", fault_name[fault], n);

    switch (fault)
    {
    case FAULT_NONE:
        break;
    case FAULT_ALLOC:
        kalloc_fail_at(kalloc_calls() + n);
        break;
    case FAULT_OPEN:
        host_fail_open_at = n;
        break;
    case FAULT_STAT:
        host_fail_stat_at = n;
        break;
    case FAULT_READ:
        host_fail_read_at = n;
        break;
    case FAULT_MAP:
        host_fail_map_at = n;
        break;
    }

    int res = process_load_for_slot("0:/blank.elf", &out, 1);
    int failed = fault == FAULT_ALLOC ? kalloc_failed() : host_failed;
    if (fault == FAULT_NONE)
    {
        host_snprintf(what, sizeof(what), "%s, nothing fails", is_elf ? "blank.elf" : "flat binary");
        if (res != 0)
        {
            say("%s: returned %d\n", what, res);
            return 1;
        }
        return check_success(what, &before, out, is_elf);
    }
    if (!failed)
    {
        return res == 0 ? NOT_REACHED : (say("%s: returned %d although nothing failed\n", what, res), 1);
    }
    if (fault == FAULT_ALLOC)
    {
        host_snprintf(what + strlen(what), sizeof(what) - strlen(what), " (in %s)", kalloc_caller(first_call + n - 1));
    }
    if (res >= 0)
    {
        say("%s: returned %d, expected an error\n", what, res);
        return 1;
    }
    return check_unchanged(what, &before, out, caller);
}

// Fill the stack with garbage, so a local the kernel code reads before setting is not 0.
static void dirty_stack(void)
{
    volatile unsigned char junk[8192];
    for (int i = 0; i < (int)sizeof(junk); i++)
    {
        junk[i] = 0x5A;
    }
}

static int slot_case(int slot)
{
    struct host_snapshot before;
    struct process *caller = setup(1, &before);
    struct process *out = untouched;
    char what[80];
    host_snprintf(what, sizeof(what), "load into %s slot %d", slot == 0 ? "the taken" : "the invalid", slot);
    int calls = kalloc_calls();
    dirty_stack();
    int res = process_load_for_slot("0:/blank.elf", &out, slot);
    if (res >= 0)
    {
        say("%s: returned %d, expected an error\n", what, res);
        return 1;
    }
    if (kalloc_calls() != calls)
    {
        say("%s: made %d allocations\n", what, kalloc_calls() - calls);
        return 1;
    }
    return check_unchanged(what, &before, out, caller);
}

static int run(int is_elf, int fault, int n, int slot)
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
        int rc = fault >= 0 ? load_case(is_elf, fault, n) : slot_case(slot);
        fflush(NULL);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (fault >= 0)
    {
        say("%s, %s %d fails: crashed (signal %d)\n", is_elf ? "blank.elf" : "flat binary", fault_name[fault], n,
            WTERMSIG(status));
    }
    else
    {
        say("load into slot %d: crashed (signal %d)\n", slot, WTERMSIG(status));
    }
    return 1;
}

int main(int argc, char **argv)
{
    int cases = 0;
    int failed = 0;
    int reached[2][FAULT_MAP + 1] = {};

    if (argc != 2)
    {
        printf("usage: process-load <blank.elf>\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < (int)sizeof(struct elf_header) || blank_size == (int)sizeof(blank))
    {
        printf("process-load: cannot read %s, or it is not 1 to %d bytes\n", argv[1], MAX_FILE - 1);
        return 2;
    }

    for (int is_elf = 1; is_elf >= 0; is_elf--)
    {
        cases++;
        failed += run(is_elf, FAULT_NONE, 0, 0) != 0;
        for (int fault = FAULT_ALLOC; fault <= FAULT_MAP; fault++)
        {
            for (int n = 1; n <= MAX_STEPS; n++)
            {
                quiet = failed >= 6;
                int rc = run(is_elf, fault, n, 0);
                if (rc == NOT_REACHED)
                {
                    break;
                }
                cases++;
                reached[is_elf][fault]++;
                failed += rc != 0;
            }
        }
    }
    int slots[] = { 0, -1, RORTHOX_MAX_PROCESSES };
    for (int i = 0; i < 3; i++)
    {
        cases++;
        quiet = failed >= 6;
        failed += run(1, -1, 0, slots[i]) != 0;
    }

    // Make sure the steps were really reached: blank.elf makes at least 6 allocations (process,
    // ELF object, ELF file, stack, task, page directory) and 2 mappings (2 segments + stack).
    if (reached[1][FAULT_ALLOC] < 6 || reached[1][FAULT_MAP] < 2 || reached[0][FAULT_ALLOC] < 6 ||
        reached[0][FAULT_MAP] < 1 || reached[1][FAULT_OPEN] < 1 || reached[0][FAULT_READ] < 1)
    {
        printf("process-load: too few failure points reached (allocations %d/%d, mappings %d/%d)\n",
               reached[1][FAULT_ALLOC], reached[0][FAULT_ALLOC], reached[1][FAULT_MAP], reached[0][FAULT_MAP]);
        failed++;
    }
    printf("process-load: failure points: blank.elf %d allocations, %d mappings; flat binary %d allocations, %d "
           "mappings\n",
           reached[1][FAULT_ALLOC], reached[1][FAULT_MAP], reached[0][FAULT_ALLOC], reached[0][FAULT_MAP]);
    printf("process-load host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
