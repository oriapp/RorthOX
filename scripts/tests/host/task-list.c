/*
 * Host check of the task list in kernel/task/task.c (scripts/tests/task-list.sh).
 *
 * Builds the real task.c against stubbed paging and heap code, then for every list of 1 to 4
 * tasks, every choice of current task and every removal order, frees the tasks one by one with
 * task_free() and checks after each step:
 *   - head->prev and tail->next are NULL, and every pair of neighbours points at each other
 *     both ways (next and prev);
 *   - the list holds exactly the tasks not removed yet, in creation order;
 *   - removing the current task makes its successor current (the head if it was the tail,
 *     nothing if the list is empty), and removing another task leaves the current one alone;
 *   - nothing writes to a task after it was freed.
 * It also checks the first two task ids (0 and 1) and their scheduling fields, and that
 * task_get_next() works with no current task. Each case runs in its own process, so a crash
 * fails that case only.
 *
 * Sleeping tasks (syscall 12), built with UndefinedBehaviorSanitizer (signed overflow is an error):
 *   select    task_get_next() for every list of 1 to 4 tasks, every runnable/sleeping mix and
 *             every current task (and none) gives the first runnable task after the current one,
 *             wrapping to the head, the current one last, and NULL when all sleep; it changes nothing
 *   wake      task_wake_sleepers(now) wakes a sleeper exactly when now has reached its deadline,
 *             also across the 2^32 wrap, at deadline 0 and at the longest delay; a runnable task
 *             with the same deadline field and a sleeper far from due stay as they are; several
 *             sleepers due on one tick all wake
 *   idle      task_sleep_until() puts the current task to sleep and switches away; with every task
 *             asleep task_next calls kernel_page and halts (a stand-in that ends each halt with a
 *             tick) until a task is due, then runs that one, in round-robin order
 *   empty     task_next with no tasks stops the kernel (PANIC) without halting
 *   lifetime  sleeping tasks freed in every order (3 and 4 tasks, every current task, poisoned
 *             memory): wake scans and selection afterwards touch only the tasks left
 * Prints "task-list host check: sleep: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kalloc.h"
#include "kernel.h"
#include "task/task.h"
#include "task/process.h"

extern struct task *current_task;
extern struct task *task_head;
extern struct task *task_tail;

// Later versions of task.c may split linking a new task off task_new().
void task_publish(struct task *task) __attribute__((weak));

// The sleep cases: task_return and the idle halt record what happened instead of running.
static int dispatch_armed, idle_armed, panic_armed;
static jmp_buf dispatch_jump, panic_jump;
static struct registers *dispatched;
static int idle_calls, kernel_pages, kernel_pages_before_idle;
static uint32_t idle_now;

// Stubs for what task.c calls outside the list code.
struct paging_4gb_chunk *paging_new_4gb(uint8_t flags)
{
    return kzalloc(sizeof(struct paging_4gb_chunk));
}
void paging_free_4gb(struct paging_4gb_chunk *chunk) { kfree(chunk); }
void paging_switch(struct paging_4gb_chunk *directory) {}
int paging_set(uint32_t *directory, void *virt, uint32_t val) { return 0; }
int paging_map(struct paging_4gb_chunk *directory, void *virt, void *phys, int flags) { return 0; }
uint32_t paging_get(uint32_t *directory, void *virt) { return 0; }
void *paging_get_physical_address(uint32_t *directory, void *virt) { return virt; }
/* User copies are tested with real ownership and a mapping model elsewhere.
   A list-only case must never enter those paths. */
int process_user_span(struct process *p, uint32_t a, int w, uint32_t *n) { exit(4); }
int paging_user_translate(uint32_t *d, uint32_t a, int w, void **p) { exit(4); }
void kernel_page() { kernel_pages++; }
void user_registers() {}
void task_return(struct registers *regs)
{
    if (!dispatch_armed)
        exit(4);
    dispatched = regs;
    longjmp(dispatch_jump, 1);
}
/* The idle loop in task_next: each call stands for one halt that ends with a PIT tick, which
   wakes the sleepers due at the next count. Only the sleep cases arm it. */
void IRQ_ON_AND_PAUSE(void)
{
    if (!idle_armed)
        exit(4);
    if (idle_calls == 0)
        kernel_pages_before_idle = kernel_pages;
    if (++idle_calls > 1000)
        exit(5);
    task_wake_sleepers(++idle_now);
}
struct elf_header *elf_header(struct elf_file *file) { return 0; }
void print(const char *str) {}
void PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s", msg);
    if (panic_armed)
        longjmp(panic_jump, 1);
    exit(3);
}

static struct process process;
static char message[200];
// Set for the cases after the first few failures: they only count.
static int quiet;

#define say(...) (quiet ? 0 : printf(__VA_ARGS__))

static struct task *new_task(void)
{
    process.filetype = PROCESS_FILETYPE_BINARY;
    struct task *task = task_new(&process);
    if (ISERR(task) || !task)
    {
        say("task_new failed\n");
        exit(1);
    }
    if (task_publish)
    {
        task_publish(task);
    }
    return task;
}

static const char *name_of(struct task *t, struct task **tasks, int n)
{
    static char names[4][8];
    static int next;
    char *s = names[next++ % 4];
    if (!t)
    {
        return "NULL";
    }
    for (int i = 0; i < n; i++)
    {
        if (tasks[i] == t)
        {
            snprintf(s, 8, "%c", 'A' + i);
            return s;
        }
    }
    return "?";
}

// 0 if the kernel's list is exactly list[0..len) with want_current current, else 1 (see message).
static int check_list(struct task **list, int len, struct task *want_current, struct task **tasks, int n)
{
    struct task *head = len ? list[0] : NULL;
    struct task *tail = len ? list[len - 1] : NULL;
    if (task_head != head)
    {
        snprintf(message, sizeof(message), "head is %s, expected %s", name_of(task_head, tasks, n), name_of(head, tasks, n));
        return 1;
    }
    if (task_tail != tail)
    {
        snprintf(message, sizeof(message), "tail is %s, expected %s", name_of(task_tail, tasks, n), name_of(tail, tasks, n));
        return 1;
    }
    for (int i = 0; i < len; i++)
    {
        struct task *prev = i > 0 ? list[i - 1] : NULL;
        struct task *next = i + 1 < len ? list[i + 1] : NULL;
        if (list[i]->prev != prev)
        {
            snprintf(message, sizeof(message), "%s->prev is %s, expected %s", name_of(list[i], tasks, n),
                     name_of(list[i]->prev, tasks, n), name_of(prev, tasks, n));
            return 1;
        }
        if (list[i]->next != next)
        {
            snprintf(message, sizeof(message), "%s->next is %s, expected %s", name_of(list[i], tasks, n),
                     name_of(list[i]->next, tasks, n), name_of(next, tasks, n));
            return 1;
        }
    }
    if (current_task != want_current)
    {
        snprintf(message, sizeof(message), "current is %s, expected %s", name_of(current_task, tasks, n),
                 name_of(want_current, tasks, n));
        return 1;
    }
    if (kalloc_poison_errors())
    {
        snprintf(message, sizeof(message), "a freed task was written to");
        return 1;
    }
    if (kalloc_bad_frees())
    {
        snprintf(message, sizeof(message), "bad kfree");
        return 1;
    }
    return 0;
}

// One removal order. Exit status 0 = pass; prints one line on failure.
static int removal_case(int n, int current, const int *order)
{
    struct task *tasks[4];
    struct task *list[4];
    char what[80];
    int len = n;
    int pos = snprintf(what, sizeof(what), "%d tasks, current %c, remove", n, 'A' + current);
    for (int i = 0; i < n; i++)
    {
        pos += snprintf(what + pos, sizeof(what) - pos, " %c", 'A' + order[i]);
    }

    kalloc_init();
    for (int i = 0; i < n; i++)
    {
        tasks[i] = new_task();
        list[i] = tasks[i];
    }
    current_task = tasks[current];
    struct task *want_current = tasks[current];
    if (check_list(list, len, want_current, tasks, n))
    {
        say("%s: before removing: %s\n", what, message);
        return 1;
    }

    for (int step = 0; step < n; step++)
    {
        struct task *victim = tasks[order[step]];
        int at = 0;
        while (list[at] != victim)
        {
            at++;
        }
        if (victim == want_current)
        {
            if (at + 1 < len)
            {
                want_current = list[at + 1];
            }
            else
            {
                // The tail: the head is next, or nothing if it was the last task.
                want_current = len > 1 ? list[0] : NULL;
            }
        }
        for (int i = at; i + 1 < len; i++)
        {
            list[i] = list[i + 1];
        }
        len--;

        task_free(victim);
        if (check_list(list, len, want_current, tasks, n))
        {
            say("%s: after removing %c: %s\n", what, 'A' + order[step], message);
            return 1;
        }
    }
    return 0;
}

static int ids_case(void)
{
    kalloc_init();
    struct task *first = new_task();
    struct task *second = new_task();
    if (first->id != 0 || second->id != 1)
    {
        say("task ids: first %u second %u, expected 0 and 1\n", first->id, second->id);
        return 1;
    }
    if (first->burstTime != 1000 || first->remainingTime != 1000)
    {
        say("task ids: first task burstTime %d remainingTime %d, expected 1000 and 1000\n", first->burstTime,
               first->remainingTime);
        return 1;
    }
    if (second->burstTime != 1000 || second->remainingTime != 1000)
    {
        say("task ids: second task burstTime %d remainingTime %d, expected 1000 and 1000\n", second->burstTime,
               second->remainingTime);
        return 1;
    }
    return 0;
}

static int next_without_current_case(void)
{
    kalloc_init();
    current_task = NULL;
    if (task_get_next() != NULL)
    {
        say("task_get_next: empty list did not give NULL\n");
        return 1;
    }
    struct task *first = new_task();
    new_task();
    current_task = NULL;
    if (task_get_next() != first)
    {
        say("task_get_next: no current task did not give the head\n");
        return 1;
    }
    return 0;
}

// ---- sleeping tasks ----

// A fresh list of n tasks (the old one is dropped with the allocator).
static void fresh_list(struct task **tasks, int n)
{
    kalloc_init();
    task_head = task_tail = current_task = NULL;
    for (int i = 0; i < n; i++)
    {
        tasks[i] = new_task();
    }
}

// What task_get_next() must give: starting after the current task (at the head if there is
// none or it is the tail), the first runnable task, the current one last; -1 for none.
static int model_next(int n, int current, unsigned sleeping)
{
    int start = current < 0 || current == n - 1 ? 0 : current + 1;
    for (int k = 0; k < n; k++)
    {
        int i = (start + k) % n;
        if (!(sleeping & (1u << i)))
        {
            return i;
        }
    }
    return -1;
}

// Every list of 1 to 4 tasks, every runnable/sleeping mix, every current task and none.
static int select_case(void)
{
    int bad = 0;
    for (int n = 1; n <= 4; n++)
    {
        for (unsigned sleeping = 0; sleeping < (1u << n); sleeping++)
        {
            for (int current = -1; current < n; current++)
            {
                struct task *tasks[4];
                fresh_list(tasks, n);
                for (int i = 0; i < n; i++)
                {
                    tasks[i]->state = (sleeping & (1u << i)) ? TASK_SLEEPING : TASK_RUNNABLE;
                    tasks[i]->wake_tick = 0;
                }
                current_task = current < 0 ? NULL : tasks[current];
                struct task *got = task_get_next();
                int want = model_next(n, current, sleeping);
                int changed = current_task != (current < 0 ? NULL : tasks[current]);
                for (int i = 0; i < n; i++)
                {
                    changed |= tasks[i]->wake_tick != 0 ||
                               tasks[i]->state != ((sleeping & (1u << i)) ? TASK_SLEEPING : TASK_RUNNABLE);
                }
                if (got != (want < 0 ? NULL : tasks[want]) || changed)
                {
                    if (bad++ < 4)
                    {
                        say("task_get_next: %d tasks, sleeping mask %x, current %d: got %s, expected %s%s\n", n,
                            sleeping, current, name_of(got, tasks, n), want < 0 ? "NULL" : name_of(tasks[want], tasks, n),
                            changed ? ", and it changed the list" : "");
                    }
                }
            }
        }
    }
    return bad != 0;
}

// Deadlines around the 2^32 wrap: (now, deadline, due). Delays are below 2^31 ticks.
static const struct
{
    uint32_t now, deadline;
    int due;
} wake_table[] = {
    { 100, 101, 0 }, { 100, 100, 1 }, { 100, 99, 1 },
    { 0xFFFFFFFFu, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 },
    { 0xFFFFFFFEu, 0xFFFFFFFFu, 0 }, { 0xFFFFFFFFu, 0xFFFFFFFFu, 1 },
    { 5, 0xFFFFFFFBu, 1 }, { 0xFFFFFFF0u, 0xA, 0 }, { 0xA, 0xA, 1 }, { 9, 0xA, 0 },
    { 0x80000001u, 0x7FFFFFFFu, 1 }, { 0x7FFFFFFFu, 0x80000001u, 0 },
    { 0x7FFFFFFFu, 0x80000000u, 0 }, { 0x80000000u, 0x80000000u, 1 },
    { 1000, 1000 + 214748367u, 0 }, { 1000 + 214748366u, 1000 + 214748367u, 0 },
    { 1000 + 214748367u, 1000 + 214748367u, 1 },
    { 0xFFFFFFF0u, 0xFFFFFFF0u + 214748367u, 0 }, { 0xFFFFFFF0u + 214748367u, 0xFFFFFFF0u + 214748367u, 1 },
};

// One sleeper per row, next to a runnable task with the same deadline field and a sleeper that
// is far from due: only the first may change, and only its state.
static int wake_case(void)
{
    int bad = 0;
    for (unsigned r = 0; r < sizeof(wake_table) / sizeof(wake_table[0]); r++)
    {
        struct task *tasks[3];
        uint32_t now = wake_table[r].now, deadline = wake_table[r].deadline;
        fresh_list(tasks, 3);
        tasks[0]->state = TASK_SLEEPING;
        tasks[0]->wake_tick = deadline;
        tasks[1]->state = TASK_RUNNABLE;
        tasks[1]->wake_tick = deadline;
        tasks[2]->state = TASK_SLEEPING;
        tasks[2]->wake_tick = now + 0x40000000u;
        task_wake_sleepers(now);
        int ok = tasks[0]->state == (wake_table[r].due ? TASK_RUNNABLE : TASK_SLEEPING) &&
                 tasks[0]->wake_tick == deadline && tasks[1]->state == TASK_RUNNABLE && tasks[1]->wake_tick == deadline &&
                 tasks[2]->state == TASK_SLEEPING && tasks[2]->wake_tick == now + 0x40000000u;
        if (!ok)
        {
            say("task_wake_sleepers: now 0x%x deadline 0x%x: sleeper %s, expected %s\n", now, deadline,
                tasks[0]->state == TASK_RUNNABLE ? "woken" : "still asleep", wake_table[r].due ? "woken" : "still asleep");
            bad++;
        }
    }

    // Several due on one tick: all of them wake, the one not due yet stays asleep.
    struct task *tasks[4];
    static const uint32_t deadlines[4] = { 10, 10, 11, 9 };
    fresh_list(tasks, 4);
    for (int i = 0; i < 4; i++)
    {
        tasks[i]->state = TASK_SLEEPING;
        tasks[i]->wake_tick = deadlines[i];
    }
    task_wake_sleepers(10);
    int ok = tasks[0]->state == TASK_RUNNABLE && tasks[1]->state == TASK_RUNNABLE && tasks[2]->state == TASK_SLEEPING &&
             tasks[3]->state == TASK_RUNNABLE;
    task_wake_sleepers(11);
    ok &= tasks[2]->state == TASK_RUNNABLE;
    if (!ok)
    {
        say("task_wake_sleepers: deadlines 10 10 11 9 at 10, then 11: not every due task woke\n");
        bad++;
    }
    return bad != 0;
}

// task_sleep_until and the idle loop of task_next, with tasks A B C (C is the tail).
static struct task *container(struct registers *regs, struct task **t)
{
    for (int i = 0; i < 3; i++)
    {
        if (regs == &t[i]->registers)
        {
            return t[i];
        }
    }
    return NULL;
}

static int idle_case(void)
{
    struct task *t[3];
    fresh_list(t, 3);
    t[1]->state = TASK_SLEEPING;
    t[1]->wake_tick = 7;
    t[2]->state = TASK_SLEEPING;
    t[2]->wake_tick = 5;
    current_task = t[0];
    idle_now = 0;
    dispatch_armed = idle_armed = 1;

    // A sleeps until 6: all asleep, the kernel halts until tick 5 wakes C.
    idle_calls = kernel_pages = kernel_pages_before_idle = 0;
    if (!setjmp(dispatch_jump))
    {
        task_sleep_until(6);
        say("task_sleep_until returned\n");
        return 1;
    }
    if (dispatched != &t[2]->registers || current_task != t[2] || t[0]->state != TASK_SLEEPING ||
        t[0]->wake_tick != 6 || idle_calls != 5 || kernel_pages_before_idle < 1)
    {
        say("A sleeps until 6, B until 7, C until 5: ran %s after %d halts (expected C after 5), A %s until %u, "
            "kernel_page before the halt %d\n", name_of(container(dispatched, t), t, 3), idle_calls,
            t[0]->state == TASK_SLEEPING ? "asleep" : "awake", t[0]->wake_tick, kernel_pages_before_idle);
        return 1;
    }

    // C sleeps until 9: tick 6 wakes A, which comes after C across the wrap to the head.
    idle_calls = 0;
    if (!setjmp(dispatch_jump))
    {
        task_sleep_until(9);
        return 1;
    }
    if (dispatched != &t[0]->registers || current_task != t[0] || idle_calls != 1 || t[1]->state != TASK_SLEEPING ||
        t[2]->state != TASK_SLEEPING)
    {
        say("C sleeps until 9 at 5: ran %s after %d halts (expected A after 1)\n", name_of(container(dispatched, t), t, 3),
            idle_calls);
        return 1;
    }

    // A is the only runnable task: task_next runs it again without halting.
    idle_calls = 0;
    if (!setjmp(dispatch_jump))
    {
        task_next();
        return 1;
    }
    if (dispatched != &t[0]->registers || idle_calls != 0)
    {
        say("only A runnable: ran %s after %d halts (expected A, no halt)\n", name_of(container(dispatched, t), t, 3),
            idle_calls);
        return 1;
    }

    // A sleeps until 20: tick 7 wakes B.
    idle_calls = 0;
    if (!setjmp(dispatch_jump))
    {
        task_sleep_until(20);
        return 1;
    }
    if (dispatched != &t[1]->registers || idle_calls != 1 || t[2]->state != TASK_SLEEPING)
    {
        say("A sleeps until 20 at 6: ran %s after %d halts (expected B after 1)\n", name_of(container(dispatched, t), t, 3),
            idle_calls);
        return 1;
    }
    return 0;
}

// No task at all: task_next stops the kernel as before, it doesn't halt.
static int empty_case(void)
{
    kalloc_init();
    task_head = task_tail = current_task = NULL;
    idle_armed = dispatch_armed = panic_armed = 1;
    idle_calls = 0;
    if (!setjmp(panic_jump))
    {
        task_next();
        say("task_next with no tasks returned\n");
        return 1;
    }
    if (idle_calls != 0)
    {
        say("task_next with no tasks halted %d times before stopping\n", idle_calls);
        return 1;
    }
    return 0;
}

// Sleeping tasks freed in every order, with poisoned memory: wake scans and selection after
// each step see only the tasks left.
static int lifetime_case(int n, int current, const int *order)
{
    struct task *tasks[4];
    fresh_list(tasks, n);
    for (int i = 0; i < n; i++)
    {
        tasks[i]->state = TASK_SLEEPING;
        tasks[i]->wake_tick = 100 * (i + 1);
    }
    current_task = tasks[current];
    for (int step = 0; step < n - 1; step++)
    {
        task_free(tasks[order[step]]);
        task_wake_sleepers(50);
        if (task_get_next() != NULL || kalloc_poison_errors() || kalloc_bad_frees())
        {
            say("sleeping tasks: %d tasks, after freeing %c: something ran or a freed task was written\n", n,
                'A' + order[step]);
            return 1;
        }
    }
    // The last one wakes and runs, then goes too.
    struct task *last = tasks[order[n - 1]];
    task_wake_sleepers(1000);
    if (task_get_next() != last || last->state != TASK_RUNNABLE)
    {
        say("sleeping tasks: %d tasks, the last one left (%c) did not wake\n", n, 'A' + order[n - 1]);
        return 1;
    }
    task_free(last);
    task_wake_sleepers(2000);
    return task_get_next() != NULL || kalloc_poison_errors() || kalloc_bad_frees();
}

static int run(int (*fn)(int, int, const int *), int n, int current, const int *order, int (*plain)(void),
               const char *what)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        int rc = fn ? fn(n, current, order) : plain();
        fflush(stdout);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return 0;
    }
    if (WIFSIGNALED(status))
    {
        if (fn)
        {
            say("%d tasks, current %c: crashed (signal %d) in removal order", n, 'A' + current, WTERMSIG(status));
            for (int i = 0; i < n; i++)
            {
                say(" %c", 'A' + order[i]);
            }
            say("\n");
        }
        else
        {
            say("%s: crashed (signal %d)\n", what, WTERMSIG(status));
        }
    }
    return 1;
}

// Next permutation of order[0..n) in lexicographic order; 0 when done.
static int next_permutation(int *order, int n)
{
    int i = n - 2;
    while (i >= 0 && order[i] > order[i + 1])
    {
        i--;
    }
    if (i < 0)
    {
        return 0;
    }
    int j = n - 1;
    while (order[j] < order[i])
    {
        j--;
    }
    int t = order[i];
    order[i] = order[j];
    order[j] = t;
    for (int a = i + 1, b = n - 1; a < b; a++, b--)
    {
        t = order[a];
        order[a] = order[b];
        order[b] = t;
    }
    return 1;
}

int main(void)
{
    int cases = 0;
    int failed = 0;
    int shown = 0;

    cases++;
    shown += run(NULL, 0, 0, NULL, ids_case, "task ids");
    cases++;
    shown += run(NULL, 0, 0, NULL, next_without_current_case, "task_get_next");
    failed = shown;

    for (int n = 1; n <= 4; n++)
    {
        for (int current = 0; current < n; current++)
        {
            int order[4] = { 0, 1, 2, 3 };
            do
            {
                cases++;
                // Show only the first few failures; the count says how many there were.
                quiet = shown >= 4;
                int f = run(removal_case, n, current, order, NULL, NULL);
                failed += f;
                shown += f;
            } while (next_permutation(order, n));
        }
    }

    printf("task-list host check: %d of %d cases failed\n", failed, cases);

    // Sleeping tasks: selection, wake, idle halt, no tasks, freeing sleepers.
    int sleep_cases = 0, sleep_failed = 0;
    quiet = 0;
    int (*const plain[])(void) = { select_case, wake_case, idle_case, empty_case };
    const char *const plain_names[] = { "select", "wake", "idle", "empty" };
    for (int i = 0; i < 4; i++)
    {
        sleep_cases++;
        int f = run(NULL, 0, 0, NULL, plain[i], plain_names[i]);
        if (f)
        {
            printf("FAIL sleep case %s\n", plain_names[i]);
        }
        sleep_failed += f;
    }
    for (int n = 3; n <= 4; n++)
    {
        for (int current = 0; current < n; current++)
        {
            int order[4] = { 0, 1, 2, 3 };
            do
            {
                sleep_cases++;
                quiet = sleep_failed >= 4;
                sleep_failed += run(lifetime_case, n, current, order, NULL, NULL);
            } while (next_permutation(order, n));
        }
    }
    printf("task-list host check: sleep: %d of %d cases failed\n", sleep_failed, sleep_cases);
    return failed || sleep_failed ? 1 : 0;
}
