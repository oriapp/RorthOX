/*
 * Host check of program lookup in kernel/isr80h/process.c (scripts/tests/program-lookup.sh and
 * scripts/tests/program-lookup-config.sh), built with AddressSanitizer.
 *
 * The real syscall 6 and 7 handlers and their lookup run against a stand-in for
 * process_load_with_arguments() that records every path it is asked to load and answers from a
 * script (-ENOENT unless told otherwise). The caller's name and command nodes come from
 * stand-ins for the checked user copies. The arguments are the search directories the kernel
 * was built with, in order (the default config.mk gives "0:/ 0:/bin/").
 *
 *   miss      a bare name that is nowhere: every directory is tried once, in order, as
 *             directory + name; syscall 7 returns -EIO, syscall 6 returns 0, nothing starts
 *   hit       found in directory k: the directories before it are tried, nothing after it, and
 *             the program is made the current process and started; syscall 7 hands the loader its command nodes unchanged
 *             (the name the caller gave, not the path) and frees them before starting it,
 *             syscall 6 hands it no arguments
 *   terminal  any other loader error in directory k (-ENOMEM, -EIO, -EINVARG, -EISTKN,
 *             -EINFORMAT) stops the lookup there; syscall 7 returns that error
 *   path      names with a '/' or ':' are tried once, as "0:/" + name, and a miss is -EIO
 *   length    names of 1 to 120 and 511 characters that are nowhere: a directory is tried
 *             only when directory + name fits in RORTHOX_MAX_PATH bytes with its terminator,
 *             the name is never cut short, names over 104 characters are refused with
 *             -EINVARG before any load (syscall 6 sees at most RORTHOX_MAX_PATH - 1 of them),
 *             and when no directory fits the result is still -EIO
 *   empty     syscall 7 refuses an empty name with -EINVARG, syscall 6 returns 0
 *
 * Prints "program-lookup host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "kernel.h"
#include "status.h"
#include "task/task.h"
#include "task/process.h"
#include "isr80h/process.h"

#define MAX_DIRS 8
#define MAX_ATTEMPTS 16
// Longest name the syscalls take: "0:/" + name + terminator in RORTHOX_MAX_PATH bytes.
#define LONGEST (RORTHOX_MAX_PATH - 4)

static const char *dirs[MAX_DIRS];
static int dir_count;

// The calling task and the process a successful load "starts".
static struct task caller;
static struct task child_task;
static struct process child;

// What the caller passes: its name (syscall 6) or command nodes (syscall 7).
#define NAME_ADDRESS 0x1000u
#define STACK_ADDRESS 0x2000u
#define NODE_ADDRESS 0x3000u
static char user_name[RORTHOX_COMMAND_ARGUMENT_SIZE];
static struct command_argument user_nodes[2];

// The loader's script and what it saw.
static int script[MAX_ATTEMPTS];
static int attempts;
static char attempt_path[MAX_ATTEMPTS][RORTHOX_MAX_PATH + 1];
static int attempt_problem;
static int switched, started, switched_before_start, freed_before_start, live_before;
static jmp_buf started_jump;
static int failed;

#define FAIL(...) do { printf("  FAIL " __VA_ARGS__); printf("\n"); failed++; return; } while (0)

static int text_length(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int text_same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

static void text_join(char *out, const char *a, const char *b)
{
    int n = 0;
    for (; *a; a++)
        out[n++] = *a;
    for (; *b; b++)
        out[n++] = *b;
    out[n] = 0;
}

// ---- stand-ins for what isr80h/process.c calls ----

struct task *task_current()
{
    return &caller;
}

int task_get_stack_item(struct task *task, int index, uint32_t *value)
{
    *value = NAME_ADDRESS;
    return 0;
}

int copy_string_from_task(struct task *task, void *virtual, void *phys, int max)
{
    if ((uintptr_t)virtual != NAME_ADDRESS)
        return -EINVARG;
    // Like the real copy: a longer string is cut at max - 1 characters and terminated.
    int i = 0;
    for (; i < max - 1 && user_name[i]; i++)
        ((char *)phys)[i] = user_name[i];
    ((char *)phys)[i] = 0;
    return 0;
}

int task_copy_from_user(struct task *task, void *destination, uint32_t source, uint32_t size)
{
    if (source == STACK_ADDRESS && size == sizeof(uint32_t))
    {
        *(uint32_t *)destination = NODE_ADDRESS;
        return 0;
    }
    if (source >= NODE_ADDRESS && source < NODE_ADDRESS + 2 && size == sizeof(struct command_argument))
    {
        int i = source - NODE_ADDRESS;
        struct command_argument *node = destination;
        *node = user_nodes[i];
        node->next = user_nodes[i].next ? (void *)(uintptr_t)(NODE_ADDRESS + 1) : 0;
        return 0;
    }
    return -EINVARG;
}

int task_copy_to_user(struct task *task, uint32_t destination, const void *source, uint32_t size)
{
    return -EINVARG;
}

void process_get_arguments(struct process *process, int *argc, char ***argv)
{
}

int process_terminate(struct process *process)
{
    return 0;
}

void task_next()
{
}

// Syscall 12 is not under test here; stop the check if it is ever reached.
uint32_t pit_get_ticks(void)
{
    printf("pit_get_ticks() called unexpectedly\n");
    exit(4);
}

uint32_t pit_ms_to_ticks(uint32_t milliseconds)
{
    printf("pit_ms_to_ticks() called unexpectedly\n");
    exit(4);
}

void task_sleep_until(uint32_t deadline)
{
    printf("task_sleep_until() called unexpectedly\n");
    exit(4);
}

// The syscall under test: 6 or 7, and for 7 whether its nodes must reach the loader.
static int syscall_number;

int process_load_with_arguments(const char *filename, struct process **process, struct command_argument *root)
{
    if (attempts >= MAX_ATTEMPTS)
    {
        attempt_problem = 1;
        return -ENOMEM;
    }
    int n = 0;
    while (n < RORTHOX_MAX_PATH && filename[n])
        n++;
    if (n == RORTHOX_MAX_PATH)
        attempt_problem = 1;    // not terminated inside the kernel's path buffer
    for (int i = 0; i < n; i++)
        attempt_path[attempts][i] = filename[i];
    attempt_path[attempts][n] = 0;

    if (syscall_number == 6 && root)
        attempt_problem = 2;
    if (syscall_number == 7 && (!root || !text_same(root->argument, user_nodes[0].argument) || !root->next ||
                                !text_same(root->next->argument, "tag") || root->next->next || !kalloc_is_live(root)))
        attempt_problem = 3;

    int res = script[attempts++];
    if (res == 0)
        *process = &child;
    return res;
}

int process_switch(struct process *process)
{
    if (process != &child)
        attempt_problem = 4;
    switched = 1;
    return 0;
}

int task_switch(struct task *task)
{
    if (task != &child_task)
        attempt_problem = 5;
    return 0;
}

void task_return(struct registers *regs)
{
    started = 1;
    switched_before_start = switched;
    freed_before_start = kalloc_live() == live_before;
    longjmp(started_jump, 1);
}

// ---- running one syscall ----

static void *result;

static void run(int number, const char *name, const int *answers, int answer_count)
{
    kalloc_init();
    child.task = &child_task;
    caller.registers.esp = STACK_ADDRESS;
    for (int i = 0; i < MAX_ATTEMPTS; i++)
        script[i] = i < answer_count ? answers[i] : -ENOENT;
    attempts = 0;
    attempt_problem = 0;
    switched = 0;
    started = 0;
    switched_before_start = 0;
    freed_before_start = 0;
    syscall_number = number;

    int i = 0;
    for (; name[i] && i < (int)sizeof(user_name) - 1; i++)
        user_name[i] = name[i];
    user_name[i] = 0;
    for (int k = 0; k < 2; k++)
    {
        for (int j = 0; j < (int)sizeof(user_nodes[k].argument); j++)
            user_nodes[k].argument[j] = 0;
    }
    for (int j = 0; j <= i; j++)
        user_nodes[0].argument[j] = user_name[j];
    text_join(user_nodes[1].argument, "tag", "");
    user_nodes[0].next = &user_nodes[1];
    user_nodes[1].next = 0;

    live_before = kalloc_live();
    result = (void *)1;
    if (setjmp(started_jump) == 0)
        result = number == 6 ? isr80h_command6_process_load_start(0) : isr80h_command7_invoke_system_command(0);
}

// The run made exactly these attempts, with no problem seen by the stand-ins.
static int attempts_were(const char *what, char expected[][RORTHOX_MAX_PATH + 1], int count)
{
    if (attempt_problem)
    {
        printf("  FAIL %s: the loader got a bad call (problem %d)\n", what, attempt_problem);
        return 0;
    }
    if (kalloc_bad_frees() || kalloc_poison_errors())
    {
        printf("  FAIL %s: bad free or write after free\n", what);
        return 0;
    }
    if (attempts != count)
    {
        printf("  FAIL %s: %d load(s), expected %d\n", what, attempts, count);
        for (int i = 0; i < attempts; i++)
            printf("       tried %s\n", attempt_path[i]);
        return 0;
    }
    for (int i = 0; i < count; i++)
    {
        if (!text_same(attempt_path[i], expected[i]))
        {
            printf("  FAIL %s: load %d was %s, expected %s\n", what, i + 1, attempt_path[i], expected[i]);
            return 0;
        }
    }
    return 1;
}

// Expected paths for a bare name: every directory where it fits, in order.
static int bare_paths(const char *name, char out[][RORTHOX_MAX_PATH + 1])
{
    int count = 0;
    for (int i = 0; i < dir_count; i++)
    {
        if (text_length(dirs[i]) + text_length(name) + 1 <= RORTHOX_MAX_PATH)
            text_join(out[count++], dirs[i], name);
    }
    return count;
}

// Syscall 6 failed (returned 0, nothing started) and syscall 7 returned <error>.
static int failed_with(const char *what, int number, int error)
{
    void *want = number == 6 ? 0 : ERROR(error);
    if (started)
    {
        printf("  FAIL %s: syscall %d started a program\n", what, number);
        return 0;
    }
    if (result != want)
    {
        printf("  FAIL %s: syscall %d returned %d, expected %d\n", what, number, ERROR_I(result), ERROR_I(want));
        return 0;
    }
    if (kalloc_live() != live_before)
    {
        printf("  FAIL %s: syscall %d left %d allocation(s)\n", what, number, kalloc_live() - live_before);
        return 0;
    }
    return 1;
}

static void miss_case(void)
{
    char want[MAX_ATTEMPTS][RORTHOX_MAX_PATH + 1];
    int count = bare_paths("abc.elf", want);
    for (int number = 6; number <= 7; number++)
    {
        run(number, "abc.elf", 0, 0);
        if (!attempts_were("miss", want, count) || !failed_with("miss", number, -EIO))
            FAIL("miss: syscall %d", number);
    }
    printf("  PASS miss\n");
}

static void hit_case(void)
{
    char want[MAX_ATTEMPTS][RORTHOX_MAX_PATH + 1];
    bare_paths("abc.elf", want);
    for (int k = 0; k < dir_count; k++)
    {
        int answers[MAX_ATTEMPTS];
        for (int i = 0; i < k; i++)
            answers[i] = -ENOENT;
        answers[k] = 0;
        for (int number = 6; number <= 7; number++)
        {
            run(number, "abc.elf", answers, k + 1);
            if (!attempts_were("hit", want, k + 1))
                FAIL("hit: syscall %d, found in %s", number, dirs[k]);
            if (!started)
                FAIL("hit: syscall %d, found in %s, did not start it (returned %d)", number, dirs[k], ERROR_I(result));
            if (!switched_before_start)
                FAIL("hit: syscall %d, found in %s, did not make it the current process before starting it",
                     number, dirs[k]);
            if (number == 7 && !freed_before_start)
                FAIL("hit: syscall 7 did not free its command nodes before starting the program");
        }
    }
    printf("  PASS hit\n");
}

static void terminal_case(void)
{
    static const int errors[] = { -ENOMEM, -EIO, -EINVARG, -EISTKN, -EINFORMAT };
    char want[MAX_ATTEMPTS][RORTHOX_MAX_PATH + 1];
    bare_paths("abc.elf", want);
    for (int e = 0; e < (int)(sizeof(errors) / sizeof(errors[0])); e++)
    {
        for (int k = 0; k < dir_count; k++)
        {
            int answers[MAX_ATTEMPTS];
            for (int i = 0; i < k; i++)
                answers[i] = -ENOENT;
            answers[k] = errors[e];
            for (int number = 6; number <= 7; number++)
            {
                run(number, "abc.elf", answers, k + 1);
                if (!attempts_were("terminal", want, k + 1) || !failed_with("terminal", number, errors[e]))
                    FAIL("terminal: syscall %d, error %d in %s", number, errors[e], dirs[k]);
            }
        }
    }
    printf("  PASS terminal\n");
}

static void path_case(void)
{
    static const char *names[] = { "bin/abc.elf", "dir/sub/abc.elf", "/abc.elf", "abc.elf/", "0:/abc.elf",
                                   "a:abc.elf", "abc.elf:" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    {
        char want[1][RORTHOX_MAX_PATH + 1];
        text_join(want[0], "0:/", names[i]);
        for (int number = 6; number <= 7; number++)
        {
            run(number, names[i], 0, 0);
            if (!attempts_were("path", want, 1) || !failed_with("path", number, -EIO))
                FAIL("path: syscall %d, %s", number, names[i]);
        }
    }
    printf("  PASS path\n");
}

static void length_case(void)
{
    char name[RORTHOX_COMMAND_ARGUMENT_SIZE];
    for (int length = 1; length <= 121; length++)
    {
        int real = length == 121 ? RORTHOX_COMMAND_ARGUMENT_SIZE - 1 : length;
        for (int i = 0; i < real; i++)
            name[i] = 'a' + i % 26;
        name[real] = 0;
        for (int number = 6; number <= 7; number++)
        {
            char want[MAX_ATTEMPTS][RORTHOX_MAX_PATH + 1];
            int count = real > LONGEST ? 0 : bare_paths(name, want);
            run(number, name, 0, 0);
            if (!attempts_were("length", want, count))
                FAIL("length: syscall %d, %d characters", number, real);
            // Syscall 6 refuses a long name with -EINVARG too (its only failure that isn't 0).
            if (real > LONGEST && (result != ERROR(-EINVARG) || started))
                FAIL("length: syscall %d, %d characters: returned %d, expected %d", number, real,
                     ERROR_I(result), -EINVARG);
            if (real <= LONGEST && !failed_with("length", number, -EIO))
                FAIL("length: syscall %d, %d characters", number, real);
        }
    }
    printf("  PASS length\n");
}

static void empty_case(void)
{
    run(7, "", 0, 0);
    if (attempts || !failed_with("empty", 7, -EINVARG))
        FAIL("empty: syscall 7 (%d load(s))", attempts);
    run(6, "", 0, 0);
    if (!failed_with("empty", 6, 0))
        FAIL("empty: syscall 6");
    printf("  PASS empty\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc - 1 > MAX_DIRS)
    {
        printf("usage: program-lookup <search directory>... (as built into the kernel, in order)\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        dirs[dir_count++] = argv[i];

    void (*const cases[])(void) = { miss_case, hit_case, terminal_case, path_case, length_case, empty_case };
    int total = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < total; i++)
    {
        cases[i]();
        fflush(0);
    }
    printf("program-lookup host check: %d of %d cases failed\n", failed, total);
    fflush(0);
    return failed ? 1 : 0;
}
