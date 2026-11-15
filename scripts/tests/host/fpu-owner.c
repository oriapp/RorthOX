/*
 * Host check of the x87/SSE ownership in kernel/task/task.c (scripts/tests/fpu-state.sh).
 *
 * Builds the real task.c against stubbed paging and a stand-in FPU: fpu_save(area) copies the
 * stand-in CPU state into area, fpu_restore(area) copies area into it, fpu_task_init(area) writes
 * the clean image, and each call goes into a ledger ("S:A" save into task A's area, "R:A" restore
 * from it, "I:A" clean image into it). A program "runs" by writing its own signature into the CPU
 * state. Each case runs in its own process, and checks the ledger, the CPU state, every task's
 * saved area and task_fpu_owner():
 *   first      the first task (task_run_first_ever_task) gets the clean state restored, even though
 *              task_publish already made it the current task, and nothing is saved
 *   same       a return to the same task (task_page, task_switch, task_next with nothing else
 *              runnable) saves and restores nothing
 *   switch     A -> B -> A: A's state is saved and B's (clean) restored, then B's saved and A's back
 *   new        a task made while A's state is in the CPU starts clean, and making it touches
 *              neither the CPU state nor the owner
 *   sleep      A sleeps, B runs and sleeps, the kernel idles: nothing is saved or restored in the
 *              idle loop, and A's state comes back when it wakes
 *   alone      a lone sleeping owner: idle, then back to it without any FPU work
 *   freeowner  freeing the owner clears it; the next task is restored without saving into the
 *              freed task (the freed memory stays untouched)
 *   reuse      the owner is freed and the next task gets the same block (as the kernel heap's first
 *              fit does): that task still starts clean
 *   freeother  freeing another task leaves the owner and the CPU state alone
 *   detached   a task discarded before it was published: no FPU work, owner unchanged
 *   pagetask   task_page_task (an address-space helper) never takes ownership
 *   misaligned task_init refuses a task whose area is not 16-byte aligned, before touching it
 * Prints "fpu-owner host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kalloc.h"
#include "kernel.h"
#include "status.h"
#include "include/kernel/fpu.h"
#include "task/task.h"
#include "task/process.h"

extern struct task *current_task;

static unsigned char cpu[FPU_STATE_SIZE];
static struct
{
    char op;
    const void *area;
} events[64];
static int nevents;
static int paging_news;

#define MAX_TASKS 8
static struct task *tasks[MAX_TASKS];
static int ntasks;
// Tasks freed so far: their storage must not be written (saved into) after the free.
static struct task *dead[MAX_TASKS];
static int ndead;
static char problem[200];

static char name_of_area(const void *area)
{
    for (int i = 0; i < ntasks; i++)
    {
        if (tasks[i] && area == tasks[i]->fpu)
        {
            return 'A' + i;
        }
    }
    return '?';
}

static void record(char op, const void *area)
{
    if (nevents < 64)
    {
        events[nevents].op = op;
        events[nevents++].area = area;
    }
}

// The ledger since it was last cleared, e.g. "S:A R:B" (names looked up now, so a task's own
// clean image, written while task_new runs, is named too).
static const char *ledger_text(void)
{
    static char text[400];
    text[0] = 0;
    for (int i = 0; i < nevents; i++)
    {
        char entry[8];
        snprintf(entry, sizeof(entry), "%s%c:%c", i ? " " : "", events[i].op, name_of_area(events[i].area));
        strncat(text, entry, sizeof(text) - strlen(text) - 1);
    }
    return text;
}

// The clean image as fpu.c describes it: FCW 0x037F, MXCSR 0x1F80, everything else zero.
static void clean_image(unsigned char *area)
{
    memset(area, 0, FPU_STATE_SIZE);
    area[0] = 0x7F;
    area[1] = 0x03;
    area[24] = 0x80;
    area[25] = 0x1F;
}

void fpu_task_init(void *area)
{
    record('I', area);
    clean_image(area);
}

void fpu_save(void *area)
{
    record('S', area);
    for (int i = 0; i < ndead; i++)
    {
        if (dead[i] && area == dead[i]->fpu)
        {
            snprintf(problem, sizeof(problem), "saved into a freed task");
        }
    }
    if ((uintptr_t)area % FPU_STATE_ALIGN)
    {
        snprintf(problem, sizeof(problem), "saved into a misaligned area");
    }
    memcpy(area, cpu, FPU_STATE_SIZE);
}

void fpu_restore(void *area)
{
    record('R', area);
    if ((uintptr_t)area % FPU_STATE_ALIGN)
    {
        snprintf(problem, sizeof(problem), "restored from a misaligned area");
    }
    memcpy(cpu, area, FPU_STATE_SIZE);
}

// The first-fit heap: with reuse on, a freed task block goes to the next task_new.
void *__real_kzalloc(size_t size);
void __real_kfree(void *ptr);
static int reuse;
static void *recycled;

void *__wrap_kzalloc(size_t size)
{
    if (reuse && recycled && size == sizeof(struct task))
    {
        unsigned char *p = recycled;
        for (size_t i = 0; i < size; i++)
        {
            if (p[i] != KALLOC_POISON)
            {
                snprintf(problem, sizeof(problem), "a freed task was written to");
                break;
            }
        }
        recycled = NULL;
        memset(p, 0, size);
        return p;
    }
    return __real_kzalloc(size);
}

void __wrap_kfree(void *ptr)
{
    if (ptr && kalloc_size(ptr) == (long)sizeof(struct task) && ndead < MAX_TASKS)
    {
        dead[ndead++] = ptr;
    }
    if (reuse && ptr && kalloc_size(ptr) == (long)sizeof(struct task))
    {
        memset(ptr, KALLOC_POISON, sizeof(struct task));
        recycled = ptr;
        return;
    }
    __real_kfree(ptr);
}

// Stubs for what task.c calls outside the task code.
static int armed;
static jmp_buf dispatch_jump;
static uint32_t now;
static int idle_calls;
static int events_at_idle;

struct paging_4gb_chunk *paging_new_4gb(uint8_t flags)
{
    paging_news++;
    return __real_kzalloc(sizeof(struct paging_4gb_chunk));
}
void paging_free_4gb(struct paging_4gb_chunk *chunk) { __real_kfree(chunk); }
void paging_switch(struct paging_4gb_chunk *directory) {}
int process_user_span(struct process *p, uint32_t a, int w, uint32_t *n) { exit(4); }
int paging_user_translate(uint32_t *d, uint32_t a, int w, void **p) { exit(4); }
void *paging_get_physical_address(uint32_t *directory, void *virt) { return virt; }
void kernel_page() {}
void user_registers() {}
void task_return(struct registers *regs)
{
    if (!armed)
        exit(4);
    armed = 0;
    longjmp(dispatch_jump, 1);
}
// One halt in the idle loop, ended by a tick: no FPU work may happen while the kernel idles.
void IRQ_ON_AND_PAUSE(void)
{
    if (idle_calls++ == 0)
        events_at_idle = nevents;
    else if (events_at_idle != nevents)
        snprintf(problem, sizeof(problem), "FPU work in the idle loop: %s", ledger_text());
    if (idle_calls > 100)
        exit(5);
    task_wake_sleepers(++now);
}
struct elf_header *elf_header(struct elf_file *file) { return 0; }
void print(const char *str) {}
void PANIC(const char *msg, const char *file, int line)
{
    printf("PANIC: %s", msg);
    exit(3);
}

static struct process process;

static struct task *new_task(int publish)
{
    process.filetype = PROCESS_FILETYPE_BINARY;
    struct task *task = task_new(&process);
    if (ISERR(task) || !task)
    {
        printf("task_new failed\n");
        exit(1);
    }
    if ((uintptr_t)task->fpu % FPU_STATE_ALIGN)
    {
        printf("task area not 16-byte aligned\n");
        exit(1);
    }
    tasks[ntasks++] = task;
    if (publish)
    {
        task_publish(task);
    }
    return task;
}

// A program using the FPU: its own signature in the CPU state.
static void run(struct task *task, int n)
{
    memset(cpu, 0x10 + n, FPU_STATE_SIZE);
}

static int is_signature(const unsigned char *area, int n)
{
    for (int i = 0; i < FPU_STATE_SIZE; i++)
    {
        if (area[i] != 0x10 + n)
            return 0;
    }
    return 1;
}

static int is_clean(const unsigned char *area)
{
    unsigned char clean[FPU_STATE_SIZE];
    clean_image(clean);
    return memcmp(area, clean, FPU_STATE_SIZE) == 0;
}

// Runs a scheduler call that ends in task_return (task_next, task_sleep_until, ...).
static uint32_t sleep_deadline;
static void go_next(void) { task_next(); }
static void go_first(void) { task_run_first_ever_task(); }
static void go_sleep(void) { task_sleep_until(sleep_deadline); }
static void dispatch(void (*fn)(void))
{
    if (!setjmp(dispatch_jump))
    {
        armed = 1;
        fn();
        printf("returned without dispatching\n");
        exit(1);
    }
}

static int failed_checks;
static void expect(int ok, const char *what)
{
    if (!ok && !failed_checks++)
    {
        printf("%s (ledger \"%s\")\n", what, ledger_text());
    }
}

static void expect_ledger(const char *want, const char *what)
{
    char text[200];
    snprintf(text, sizeof(text), "%s: ledger is not \"%s\"", what, want);
    expect(strcmp(ledger_text(), want) == 0, text);
    nevents = 0;
}

// A starts first and runs with its signature in the CPU.
static struct task *first_a(void)
{
    struct task *a = new_task(1);
    nevents = 0;
    memset(cpu, 0xEE, FPU_STATE_SIZE);
    dispatch(go_first);
    expect_ledger("R:A", "first dispatch");
    expect(is_clean(cpu), "first task does not start clean");
    expect(task_fpu_owner() == a, "first task is not the owner");
    run(a, 0);
    return a;
}

static void case_first(void)
{
    first_a();
}

static void case_same(void)
{
    struct task *a = first_a();
    task_page();
    task_switch(a);
    dispatch(go_next);
    expect_ledger("", "return to the same task");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "same task lost its state");
}

static void case_switch(void)
{
    struct task *a = first_a();
    struct task *b = new_task(1);
    nevents = 0;
    dispatch(go_next);
    expect_ledger("S:A R:B", "A -> B");
    expect(is_clean(cpu) && task_fpu_owner() == b && is_signature(a->fpu, 0), "A -> B: B not clean or A not saved");
    run(b, 1);
    dispatch(go_next);
    expect_ledger("S:B R:A", "B -> A");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a && is_signature(b->fpu, 1), "B -> A: A's state not back");
}

static void case_new(void)
{
    struct task *a = first_a();
    struct task *c = new_task(1);
    expect_ledger("I:B", "new task");
    expect(is_clean(c->fpu), "new task does not start clean");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "making a task changed the CPU state or the owner");
}

static void case_sleep(void)
{
    struct task *a = first_a();
    struct task *b = new_task(1);
    nevents = 0;
    sleep_deadline = now + 5;
    dispatch(go_sleep);
    expect_ledger("S:A R:B", "A sleeps");
    run(b, 1);
    sleep_deadline = now + 10;
    dispatch(go_sleep);
    expect(idle_calls > 0, "the kernel did not idle");
    expect(current_task == a, "A did not wake first");
    expect_ledger("S:B R:A", "back to A after idling");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "A's state not back after idling");
}

static void case_alone(void)
{
    struct task *a = first_a();
    sleep_deadline = now + 5;
    dispatch(go_sleep);
    expect(idle_calls > 0, "the kernel did not idle");
    expect_ledger("", "lone sleeper back");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "lone sleeper lost its state");
}

static void case_freeowner(void)
{
    struct task *a = first_a();
    new_task(1);
    nevents = 0;
    task_free(a);
    expect_ledger("", "free the owner");
    expect(task_fpu_owner() == NULL, "freed owner still the owner");
    dispatch(go_next);
    expect_ledger("R:B", "next task after the owner was freed");
    expect(is_clean(cpu), "next task not clean");
    expect(kalloc_poison_errors() == 0, "a freed task was written to");
}

static void case_reuse(void)
{
    reuse = 1;
    struct task *a = first_a();
    task_free(a);
    struct task *c = new_task(1);
    expect(c == a, "stand-in heap did not hand the block back");
    expect_ledger("I:A", "new task in the freed block");
    dispatch(go_first);
    expect_ledger("R:A", "new task in the freed owner's block");
    expect(is_clean(cpu), "new task in the freed owner's block does not start clean");
}

static void case_freeother(void)
{
    struct task *a = first_a();
    struct task *b = new_task(1);
    nevents = 0;
    task_free(b);
    task_switch(a);
    expect_ledger("", "free another task");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "freeing another task changed the owner's state");
}

static void case_detached(void)
{
    struct task *a = first_a();
    struct task *c = new_task(0);
    nevents = 0;
    task_discard(c);
    expect_ledger("", "discard a detached task");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "discarding a detached task changed the owner");
}

static void case_pagetask(void)
{
    struct task *a = first_a();
    struct task *b = new_task(1);
    nevents = 0;
    task_page_task(b);
    expect_ledger("", "task_page_task");
    expect(is_signature(cpu, 0) && task_fpu_owner() == a, "task_page_task changed the owner");
}

static void case_misaligned(void)
{
    static unsigned char block[sizeof(struct task) + 32] __attribute__((aligned(16)));
    struct task *task = (struct task *)(block + 8);
    memset(block, 0x5A, sizeof(block));
    process.filetype = PROCESS_FILETYPE_BINARY;
    int res = task_init(task, &process);
    expect(res == -EINVARG, "misaligned area accepted");
    expect(paging_news == 0 && block[8] == 0x5A && nevents == 0, "misaligned task touched before refusing");
}

static const struct
{
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"first", case_first}, {"same", case_same}, {"switch", case_switch}, {"new", case_new},
    {"sleep", case_sleep}, {"alone", case_alone}, {"freeowner", case_freeowner}, {"reuse", case_reuse},
    {"freeother", case_freeother}, {"detached", case_detached}, {"pagetask", case_pagetask},
    {"misaligned", case_misaligned},
};

int main(void)
{
    int n = sizeof(cases) / sizeof(cases[0]), failed = 0;
    for (int i = 0; i < n; i++)
    {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
        {
            kalloc_init();
            printf("%s: ", cases[i].name);
            cases[i].fn();
            if (problem[0])
                expect(0, problem);
            if (kalloc_poison_errors() || kalloc_bad_frees())
                expect(0, "freed memory written to, or a bad free");
            printf(failed_checks ? "" : "ok\n");
            exit(failed_checks ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            if (!WIFEXITED(status) || WEXITSTATUS(status) > 1)
                printf("%s: crashed or stopped (status %d)\n", cases[i].name, status);
            failed++;
        }
    }
    printf("fpu-owner host check: %d of %d cases failed\n", failed, n);
    return failed != 0;
}
