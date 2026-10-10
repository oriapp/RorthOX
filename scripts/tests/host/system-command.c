/*
 * Host check of syscall 7 (run a command with arguments) in kernel/isr80h/process.c and
 * kernel/task/process.c (scripts/tests/argument-oom.sh).
 *
 * Builds the real isr80h/process.c, task/process.c, task/task.c and loader/formats/elfloader.c
 * with the host C compiler, with the file and paging layers from process-host.c and the heap
 * from kalloc.c. A caller process runs in slot 0 and asks for "blank.elf arg-one arg-two" (three
 * command nodes it owns). Then, in turn, each heap allocation, each fopen/fstat/fread and each
 * mapping of that syscall fails, for blank.elf and for a flat binary. A failed syscall must return
 * an error to the caller and leave everything as it was: no new process in a slot, the task list,
 * the current task and the keyboard process unchanged, no file open, no allocation left (the
 * arguments included), no bad kfree, no write after kfree, the command nodes untouched, the new
 * page directory freed before the memory it maps and never loaded, and the new process never
 * started. When nothing fails, the new process must be started with argc 3 and copies of the
 * three arguments, and must hold the keyboard. Each case runs in its own process.
 *
 * Usage: system-command <blank.elf>
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
#include "isr80h/process.h"

#define MAX_FILE (64 * 1024)
#define MAX_STEPS 64
#define NOT_REACHED 10
#define ARGS 3

static unsigned char blank[MAX_FILE];
static int blank_size;
static const char flat[] = "A flat binary: not an ELF file, so the loader falls back to loading it as it is.";
static const char *const words[ARGS] = { "blank.elf", "arg-one", "arg-two" };
static char message[200];
static int quiet;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

enum
{
    FAULT_NONE,
    FAULT_ALLOC,
    FAULT_OPEN,
    FAULT_STAT,
    FAULT_READ,
    FAULT_MAP,
};
static const char *fault_name[] = { "nothing", "allocation", "fopen", "fstat", "fread", "mapping" };

static struct command_argument *nodes[ARGS];
static struct command_argument copies[ARGS];

// The caller in slot 0 with its user stack and command list, then the file the child loads.
static struct process *setup(int is_elf, struct host_snapshot *before)
{
    kalloc_init();
    host_reset();
    host_file_serve(blank, blank_size);
    struct process *caller = host_load_caller();

    // The command list and the stack the syscall reads it from (kept in the 32-bit heap area,
    // like user memory in the identity map).
    for (int i = 0; i < ARGS; i++)
    {
        nodes[i] = kzalloc(sizeof(struct command_argument));
    }
    uint32_t *stack = kzalloc(64);
    if (!nodes[0] || !nodes[1] || !nodes[2] || !stack)
    {
        printf("setup: no memory\n");
        exit(2);
    }
    for (int i = 0; i < ARGS; i++)
    {
        strncpy(nodes[i]->argument, words[i], sizeof(nodes[i]->argument));
        nodes[i]->next = i + 1 < ARGS ? nodes[i + 1] : 0;
        memcpy(&copies[i], nodes[i], sizeof(copies[i]));
    }
    stack[0] = (uint32_t)(unsigned long)nodes[0];
    caller->task->registers.esp = (uint32_t)(unsigned long)stack;

    if (is_elf)
    {
        host_file_serve(blank, blank_size);
    }
    else
    {
        host_file_serve(flat, sizeof(flat));
    }
    host_reset();
    if (!host_snapshot_take(before, message, sizeof(message)))
    {
        printf("setup: %s\n", message);
        exit(2);
    }
    return caller;
}

static int nodes_untouched(void)
{
    for (int i = 0; i < ARGS; i++)
    {
        if (!kalloc_is_live(nodes[i]) || memcmp(nodes[i], &copies[i], sizeof(copies[i])) != 0)
        {
            return 0;
        }
    }
    return 1;
}

static int check_failure(const char *what, const struct host_snapshot *before, struct process *caller)
{
    struct host_snapshot after;
    if (!host_snapshot_take(&after, message, sizeof(message)))
    {
        say("%s: %s\n", what, message);
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
    if (!nodes_untouched())
    {
        say("%s: the caller's command nodes were changed or freed\n", what);
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

static int check_started(const char *what, const struct host_snapshot *before, struct process *caller)
{
    struct process *child = process_get(1);
    struct host_snapshot after;
    if (!child || !host_task_return_regs || host_task_return_regs != &child->task->registers)
    {
        say("%s: the new process in slot 1 was not started\n", what);
        return 1;
    }
    if (!host_snapshot_take(&after, message, sizeof(message)))
    {
        say("%s: %s\n", what, message);
        return 1;
    }
    if (after.task_count != before->task_count + 1 || after.tail != child->task || after.current_task != child->task)
    {
        say("%s: the new task is not the current task at the end of the list\n", what);
        return 1;
    }
    if (after.current_process != child)
    {
        say("%s: the new process does not get the keyboard\n", what);
        return 1;
    }
    if (child->arguments.argc != ARGS || !kalloc_is_live(child->arguments.argv))
    {
        say("%s: argc is %d, expected %d\n", what, child->arguments.argc, ARGS);
        return 1;
    }
    for (int i = 0; i < ARGS; i++)
    {
        char *arg = child->arguments.argv[i];
        if (!kalloc_is_live(arg) || arg == nodes[i]->argument || strncmp(arg, words[i], 100) != 0)
        {
            say("%s: argv[%d] is not a copy of \"%s\"\n", what, i, words[i]);
            return 1;
        }
    }
    if (!nodes_untouched() || host_open_files || kalloc_bad_frees() || kalloc_poison_errors() ||
        host_paging_problems)
    {
        say("%s: command nodes changed, file left open, bad kfree or paging problem\n", what);
        return 1;
    }
    return 0;
}

static int syscall_case(int is_elf, int fault, int n)
{
    struct host_snapshot before;
    struct process *caller = setup(is_elf, &before);
    char what[120];
    int first_call = kalloc_calls() + 1;
    host_snprintf(what, sizeof(what), "%s, %s %d fails", is_elf ? "blank.elf" : "flat binary", fault_name[fault], n);

    switch (fault)
    {
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

    void *volatile ret = 0;
    volatile int started = 0;
    host_task_return_armed = 1;
    if (setjmp(host_task_return_jump) == 0)
    {
        ret = isr80h_command7_invoke_system_command(0);
    }
    else
    {
        started = 1;
    }
    host_task_return_armed = 0;

    if (fault == FAULT_NONE)
    {
        host_snprintf(what, sizeof(what), "%s, nothing fails", is_elf ? "blank.elf" : "flat binary");
        return check_started(what, &before, caller);
    }
    int failed = fault == FAULT_ALLOC ? kalloc_failed() : host_failed;
    if (!failed)
    {
        return started ? NOT_REACHED : (say("%s: returned %d although nothing failed\n", what, (int)(long)ret), 1);
    }
    if (fault == FAULT_ALLOC)
    {
        host_snprintf(what + strlen(what), sizeof(what) - strlen(what), " (in %s)", kalloc_caller(first_call + n - 1));
    }
    if (started)
    {
        say("%s: the new process was started\n", what);
        return 1;
    }
    if (!ISERR(ret))
    {
        say("%s: returned %d, expected an error\n", what, (int)(long)ret);
        return 1;
    }
    return check_failure(what, &before, caller);
}

static int run(int is_elf, int fault, int n)
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
        int rc = syscall_case(is_elf, fault, n);
        fflush(NULL);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    say("%s, %s %d fails: crashed (signal %d)\n", is_elf ? "blank.elf" : "flat binary", fault_name[fault], n,
        WTERMSIG(status));
    return 1;
}

int main(int argc, char **argv)
{
    int cases = 0;
    int failed = 0;
    int reached[2][FAULT_MAP + 1] = {};

    if (argc != 2)
    {
        printf("usage: system-command <blank.elf>\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < (int)sizeof(struct elf_header) || blank_size == (int)sizeof(blank))
    {
        printf("system-command: cannot read %s, or it is not 1 to %d bytes\n", argv[1], MAX_FILE - 1);
        return 2;
    }

    for (int is_elf = 1; is_elf >= 0; is_elf--)
    {
        cases++;
        failed += run(is_elf, FAULT_NONE, 0) != 0;
        for (int fault = FAULT_ALLOC; fault <= FAULT_MAP; fault++)
        {
            for (int n = 1; n <= MAX_STEPS; n++)
            {
                quiet = failed >= 6;
                int rc = run(is_elf, fault, n);
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

    // The steps must really be reached: blank.elf makes at least 6 allocations for the process
    // and 1 + 3 for the arguments (argv and the three strings), and maps them all.
    if (reached[1][FAULT_ALLOC] < 6 + 1 + ARGS || reached[1][FAULT_MAP] < 3 + 1 + ARGS ||
        reached[0][FAULT_ALLOC] < 6 + 1 + ARGS || reached[0][FAULT_MAP] < 2 + 1 + ARGS)
    {
        printf("system-command: too few failure points reached\n");
        failed++;
    }
    printf("system-command: failure points: blank.elf %d allocations, %d mappings; flat binary %d allocations, %d "
           "mappings\n",
           reached[1][FAULT_ALLOC], reached[1][FAULT_MAP], reached[0][FAULT_ALLOC], reached[0][FAULT_MAP]);
    printf("system-command host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
