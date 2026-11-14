/*
 * Host check of ending a process (scripts/tests/process-exit.sh): the real process.c, task.c,
 * elfloader.c and isr80h/process.c on the stand-ins of process-host.c.
 *
 * Each case loads blank.elf as the caller (slot 0, the only task), then a child into slot 1 with
 * two arguments, one more allocation and two open files, as an ELF file and as a flat binary, and
 * makes the child the running task and the keyboard process:
 *   refused  the child's file type is made invalid: process_terminate fails and changes nothing
 *            (the process, its task in the list, the current task, its page directory and user
 *            mappings, its allocations, open files, stack and program memory, the slots, the
 *            keyboard process, the heap); with the type put back it ends normally
 *   fault    process_terminate as a ring-3 fault calls it
 *   exit     syscall 9: the child ends and the caller runs
 *   stop     syscall 9 with an invalid file type stops the kernel (PANIC) with nothing changed and
 *            never runs another task
 *   orphan   syscall 9 from a task its process doesn't own any more (the process's slot holds
 *            something else, or the process has another task) stops the kernel the same way,
 *            before ending anything
 *   revoke   one revocation of the child's allocations fails during process_terminate (each in
 *            turn): no allocation is left once the page directory is gone
 *   discard  a launch whose argument copy runs out of memory, with one revocation of the copies
 *            made so far failing (each allocation and each revocation in turn): nothing is left
 * After every end the heap, the task list, the current task, the slots, the open files and the
 * keyboard process are as before the child was loaded, the page directory was destroyed before
 * any memory it maps was freed, nothing was freed twice or written after kfree, and "terminated"
 * was printed once. Every case runs in its own process.
 * Prints "process-exit host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include "host.h"
#include "kalloc.h"
#include "process-host.h"
#include "config.h"
#include "status.h"
#include "memory/memory.h"
#include "string/string.h"
#include "memory/paging/paging.h"
#include "fs/file.h"
#include "task/task.h"
#include "task/process.h"
#include "isr80h/process.h"

#define NOT_REACHED 10
#define BAD_TYPE 2

extern struct task *current_task;

static unsigned char blank[65536];
static int blank_size;
// A flat binary: anything without the ELF magic.
static unsigned char raw[3 * PAGING_PAGE_SIZE + 100];
static char message[200];
static struct process *caller;
static struct host_snapshot baseline;
static struct command_argument args[3];

// Set by a stop case: PANIC checks that nothing changed before it ends the case.
static struct process *stop_child;
static struct host_snapshot stop_before;
static struct process stop_copy;
// The PANIC message a stop case expects, if it checks one.
static const char *stop_message;
static int unchanged(const char *what, struct process *child, const struct host_snapshot *before, const struct process *copy);

void __real_PANIC(const char *msg, const char *file, int line);
void __wrap_PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s\n", msg);
    int bad = stop_child && unchanged("stop: at the PANIC", stop_child, &stop_before, &stop_copy);
    if (stop_message && strncmp(msg, stop_message, 100))
    {
        printf("stop: PANIC message is not \"%s\"\n", stop_message);
        bad = 1;
    }
    fflush(0);
    _exit(bad ? 1 : 3);
}

static void copy_text(char *to, const char *from)
{
    while ((*to++ = *from++))
        ;
}

// The arguments of a launch: "child one" (or "child one two").
static struct command_argument *arguments(int count)
{
    static const char *const words[] = {"child", "one", "two"};
    for (int i = 0; i < count; i++)
    {
        copy_text(args[i].argument, words[i]);
        args[i].next = i + 1 < count ? &args[i + 1] : 0;
    }
    return &args[0];
}

// The caller in slot 0, running, with the keyboard; the baseline taken after it.
static void start(int elf)
{
    kalloc_init();
    kalloc_set_alignment(PAGING_PAGE_SIZE);
    host_reset();
    host_file_serve(blank, blank_size);
    caller = host_load_caller();
    host_reset();
    host_terminated_prints = 0;
    if (!host_snapshot_take(&baseline, message, sizeof(message)))
    {
        printf("baseline: %s\n", message);
        exit(2);
    }
    if (elf)
    {
        host_file_serve(blank, blank_size);
    }
    else
    {
        host_file_serve(raw, sizeof(raw));
    }
}

static struct process *setup(int elf)
{
    start(elf);
    struct process *child = 0;
    int res = process_load_with_arguments("0:/child", &child, arguments(2));
    if (res < 0 || !child || process_get(1) != child ||
        child->filetype != (elf ? PROCESS_FILETYPE_ELF : PROCESS_FILETYPE_BINARY))
    {
        printf("could not load the child (%d)\n", res);
        exit(2);
    }
    void *extra = process_malloc(child, 100);
    int h1 = process_open_file(child, "0:/child", FILE_MODE_READ);
    int h2 = process_open_file(child, "0:/child", FILE_MODE_READ);
    if (!extra || h1 != 1 || h2 != 2 || host_paging_problems)
    {
        printf("could not set up the child (%p %d %d %d)\n", extra, h1, h2, host_paging_problems);
        exit(2);
    }
    process_switch(child);
    current_task = child->task;
    return child;
}

// A refused termination: everything the child had is still there, nothing else changed.
static int unchanged(const char *what, struct process *child, const struct host_snapshot *before, const struct process *copy)
{
    struct host_snapshot now;
    if (!host_snapshot_take(&now, message, sizeof(message)) || host_snapshot_compare(before, &now, message, sizeof(message)))
    {
        printf("%s: %s\n", what, message);
        return 1;
    }
    if (!kalloc_is_live(child) || memcmp(child, (void *)copy, sizeof(*copy)))
    {
        printf("%s: the process was freed or changed\n", what);
        return 1;
    }
    struct task *task = copy->task;
    if (!task || !kalloc_is_live(task) || task->process != child || !kalloc_is_live(task->page_directory) ||
        !kalloc_is_live(copy->stack) || !kalloc_is_live(copy->ptr))
    {
        printf("%s: the task, its page directory, the stack or the program memory is gone\n", what);
        return 1;
    }
    uint32_t *directory = task->page_directory->directory_entry;
    for (int i = 0; i < RORTHOX_MAX_PROGRAM_ALLOCATIONS; i++)
    {
        void *ptr = copy->allocations[i].ptr, *out = 0;
        if (ptr && (!kalloc_is_live(ptr) || paging_user_translate(directory, (uint32_t)(unsigned long)ptr, 1, &out) != 0))
        {
            printf("%s: allocation %d was freed or unmapped\n", what, i);
            return 1;
        }
    }
    if (host_paging_problems || host_terminated_prints || kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf("%s: paging problem (%s), \"terminated\" printed, bad kfree or write after kfree\n", what,
               host_paging_problems ? host_paging_problem : "none");
        return 1;
    }
    return 0;
}

// After an end: back to the baseline, the page directory gone before the memory it mapped.
static int ended(const char *what)
{
    struct host_snapshot after;
    if (!host_snapshot_take(&after, message, sizeof(message)) || host_snapshot_compare(&baseline, &after, message, sizeof(message)))
    {
        printf("%s: %s\n", what, message);
        return 1;
    }
    if (host_paging_problems)
    {
        printf("%s: %s\n", what, host_paging_problem);
        return 1;
    }
    if (kalloc_bad_frees() || kalloc_poison_errors() || host_terminated_prints != 1)
    {
        printf("%s: %d bad frees, %d writes after kfree, \"terminated\" printed %d times\n", what,
               kalloc_bad_frees(), kalloc_poison_errors(), host_terminated_prints);
        return 1;
    }
    return 0;
}

static const char *kind(int elf) { return elf ? "ELF" : "flat binary"; }

static int refused_case(int elf)
{
    char what[80];
    host_snprintf(what, sizeof(what), "refused (%s)", kind(elf));
    struct process *child = setup(elf);
    child->filetype = BAD_TYPE;
    struct host_snapshot before;
    struct process copy;
    host_snapshot_take(&before, message, sizeof(message));
    memcpy(&copy, child, sizeof(copy));
    int res = process_terminate(child);
    if (res >= 0)
    {
        printf("%s: process_terminate returned %d for an invalid file type\n", what, res);
        return 1;
    }
    if (unchanged(what, child, &before, &copy))
    {
        return 1;
    }
    child->filetype = elf ? PROCESS_FILETYPE_ELF : PROCESS_FILETYPE_BINARY;
    res = process_terminate(child);
    if (res != 0)
    {
        printf("%s: with the type put back process_terminate returned %d\n", what, res);
        return 1;
    }
    return ended(what);
}

static int fault_case(int elf)
{
    char what[80];
    host_snprintf(what, sizeof(what), "fault (%s)", kind(elf));
    struct process *child = setup(elf);
    int res = process_terminate(child);
    if (res != 0)
    {
        printf("%s: process_terminate returned %d\n", what, res);
        return 1;
    }
    return ended(what);
}

static int exit_case(int elf)
{
    char what[80];
    host_snprintf(what, sizeof(what), "exit (%s)", kind(elf));
    setup(elf);
    struct task *next = caller->task;
    host_task_return_armed = 1;
    if (!setjmp(host_task_return_jump))
    {
        isr80h_command9_exit(0);
        printf("%s: syscall 9 returned\n", what);
        return 1;
    }
    if (host_task_return_regs != &next->registers || host_switch_count < 1 ||
        host_switches[host_switch_count - 1] != next->page_directory)
    {
        printf("%s: the caller does not run next\n", what);
        return 1;
    }
    return ended(what);
}

static int stop_case(int elf)
{
    struct process *child = setup(elf);
    child->filetype = BAD_TYPE;
    host_snapshot_take(&stop_before, message, sizeof(message));
    memcpy(&stop_copy, child, sizeof(stop_copy));
    stop_child = child;
    host_task_return_armed = 1;
    if (!setjmp(host_task_return_jump))
    {
        isr80h_command9_exit(0);
        printf("stop (%s): syscall 9 returned after a failed termination\n", kind(elf));
        return 1;
    }
    printf("stop (%s): another task runs after a failed termination\n", kind(elf));
    return 1;
}

// variant 0: the child's id names an empty slot; 1: the running task is a copy of the child's
// task, so the child points to another task.
static int orphan_case(int elf, int variant)
{
    static struct task stale;
    struct process *child = setup(elf);
    if (variant == 0)
    {
        child->id = RORTHOX_MAX_PROCESSES - 1;
    }
    else
    {
        memcpy(&stale, child->task, sizeof(stale));
        current_task = &stale;
    }
    host_snapshot_take(&stop_before, message, sizeof(message));
    memcpy(&stop_copy, child, sizeof(stop_copy));
    stop_child = child;
    stop_message = "exit: Invalid exiting task";
    host_task_return_armed = 1;
    if (!setjmp(host_task_return_jump))
    {
        isr80h_command9_exit(0);
        printf("orphan (%s) %d: syscall 9 returned\n", kind(elf), variant);
        return 1;
    }
    printf("orphan (%s) %d: another task runs\n", kind(elf), variant);
    return 1;
}

static int revoke_case(int elf, int n)
{
    char what[80];
    host_snprintf(what, sizeof(what), "revoke (%s): revocation %d fails", kind(elf), n);
    struct process *child = setup(elf);
    host_fail_unmap_at = host_unmap_calls + n;
    int res = process_terminate(child);
    if (!host_unmap_failed)
    {
        return res == 0 && !ended(what) ? NOT_REACHED : 1;
    }
    if (res != 0)
    {
        printf("%s: process_terminate returned %d\n", what, res);
        return 1;
    }
    return ended(what);
}

static int discard_case(int elf, int combo)
{
    int n = combo / 4 + 1, m = combo % 4 + 1;
    char what[80];
    host_snprintf(what, sizeof(what), "discard (%s): allocation %d and revocation %d fail", kind(elf), n, m);
    start(elf);
    kalloc_fail_at(kalloc_calls() + n);
    host_fail_unmap_at = m;
    struct process *child = (struct process *)0x1234;
    int res = process_load_with_arguments("0:/child", &child, arguments(3));
    if (!kalloc_failed() || !host_unmap_failed)
    {
        return NOT_REACHED;
    }
    if (res >= 0 || child != (struct process *)0x1234)
    {
        printf("%s: returned %d\n", what, res);
        return 1;
    }
    struct host_snapshot after;
    if (!host_snapshot_take(&after, message, sizeof(message)) || host_snapshot_compare(&baseline, &after, message, sizeof(message)))
    {
        printf("%s: %s\n", what, message);
        return 1;
    }
    if (host_paging_problems || kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf("%s: paging problem (%s), %d bad frees, %d writes after kfree\n", what,
               host_paging_problems ? host_paging_problem : "none", kalloc_bad_frees(), kalloc_poison_errors());
        return 1;
    }
    return 0;
}

static int failed, cases, reached;

// Run fn(a, b) in a child process; want is the exit status of a pass.
static void run(int (*fn)(int, int), int a, int b, int want, const char *name)
{
    fflush(0);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        int rc = fn(a, b);
        fflush(0);
        _exit(rc);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == NOT_REACHED)
    {
        return;
    }
    cases++;
    reached++;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != want)
    {
        failed++;
        printf("FAIL %s %d %d (status %#x)\n", name, a, b, status);
    }
}

static int refused(int elf, int unused) { return refused_case(elf); }
static int fault(int elf, int unused) { return fault_case(elf); }
static int exit_(int elf, int unused) { return exit_case(elf); }
static int stop(int elf, int unused) { return stop_case(elf); }

int main(int argc, char **argv)
{
    int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
    blank_size = fd < 0 ? -1 : read(fd, blank, sizeof(blank));
    if (blank_size < 52 || blank_size == sizeof(blank))
    {
        printf("usage: process-exit blank.elf\n");
        return 2;
    }
    close(fd);
    for (unsigned i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)(0x90 + i % 7);

    for (int elf = 0; elf < 2; elf++)
    {
        run(refused, elf, 0, 0, "refused");
        run(fault, elf, 0, 0, "fault");
        run(exit_, elf, 0, 0, "exit");
        run(stop, elf, 0, 3, "stop");
        run(orphan_case, elf, 0, 3, "orphan");
        run(orphan_case, elf, 1, 3, "orphan");
        reached = 0;
        for (int n = 1; n <= 16; n++) run(revoke_case, elf, n, 0, "revoke");
        if (reached < 4)
        {
            printf("FAIL revoke (%s): only %d revocations failed, expected one for each of the 4 allocations\n", kind(elf), reached);
            failed++;
        }
        reached = 0;
        for (int combo = 0; combo < 4 * 40; combo++) run(discard_case, elf, combo, 0, "discard");
        if (reached < 3)
        {
            printf("FAIL discard (%s): only %d launches had both failures\n", kind(elf), reached);
            failed++;
        }
        printf("%s: %d discard cases with both failures\n", kind(elf), reached);
    }
    printf("process-exit host check: %d of %d cases failed\n", failed, cases);
    return failed != 0;
}
