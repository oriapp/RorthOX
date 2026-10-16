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
 */
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
void kernel_page() {}
void user_registers() {}
void task_return(struct registers *regs) { exit(4); }
struct elf_header *elf_header(struct elf_file *file) { return 0; }
void print(const char *str) {}
void PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s", msg);
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
    return failed ? 1 : 0;
}
