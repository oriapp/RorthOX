/*
 * Host check of the sleep syscall handler (syscall 12) in kernel/isr80h/process.c, for
 * scripts/tests/sleep.sh. The real isr80h_command_sleep() is included here and built with
 * AddressSanitizer and UndefinedBehaviorSanitizer (signed overflow is an error); the rest of
 * process.c is dropped by the linker. Stand-ins give it the stack operand, the tick counter
 * and the tick conversion (an independent model: 0 for 0, else ceil(ms / 10) + 2), and record
 * the call that puts the task to sleep (it does not return, like the real one).
 *
 *   fetch     the operand is read once, as stack item 0 of the current task; a failed read
 *             returns that error
 *   zero      0 returns 0 at once
 *   reject    -1, INT32_MIN, 0x80000001 and every value above INT32_MAX return -EINVARG
 *   sleep     1, 9, 10, 11, 999, 1000, 1001 and INT32_MAX put the task to sleep until
 *             now + ticks (mod 2^32, also across the wrap and at 0x7FFFFFF0), with the saved
 *             eax already 0 and the other saved registers untouched
 *
 * Refused and zero requests must not sleep, call task_next or change the task. Prints
 * "sleep host check: <failed> of <cases> cases failed".
 */
#include <setjmp.h>
#include "isr80h/process.c"
#include "host.h"

unsigned int alarm(unsigned int seconds);
static struct task caller;
static uint32_t operand;
static int fetch_error;
static int fetches, fetch_bad;
static uint32_t now;
static int conversions;
static uint32_t converted;
static int slept, scheduled;
static uint32_t slept_deadline;
static struct registers registers_at_sleep;
static jmp_buf asleep;

struct task *task_current()
{
    return &caller;
}

int task_get_stack_item(struct task *task, int index, uint32_t *value)
{
    fetches++;
    if (task != &caller || index != 0)
        fetch_bad = 1;
    if (fetch_error)
        return fetch_error;
    *value = operand;
    return 0;
}

uint32_t pit_get_ticks(void)
{
    return now;
}

static uint32_t model_ticks(uint32_t ms)
{
    if (ms == 0)
        return 0;
    return (uint32_t)(((unsigned long long)ms * 100 + 999) / 1000) + 2;
}

uint32_t pit_ms_to_ticks(uint32_t milliseconds)
{
    conversions++;
    converted = milliseconds;
    return model_ticks(milliseconds);
}

void task_sleep_until(uint32_t deadline)
{
    slept++;
    slept_deadline = deadline;
    registers_at_sleep = caller.registers;
    longjmp(asleep, 1);
}

void task_next()
{
    scheduled++;
}

static int failed, cases;

static void set_caller(void)
{
    unsigned char *p = (unsigned char *)&caller;
    for (unsigned i = 0; i < sizeof(caller); i++)
        p[i] = (unsigned char)(0x40 + i);
    caller.registers.eax = SYSCALL_SLEEP; // the syscall number, as the entry saved it
    caller.state = TASK_RUNNABLE;
}

static int same_task(const struct task *a, const struct task *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (unsigned i = 0; i < sizeof(*a); i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

// One call. want_sleep: 1 = must sleep, 0 = must return want.
static void run(const char *what, uint32_t value, int error, uint32_t clock, int want_sleep, int want)
{
    cases++;
    set_caller();
    struct task before = caller;
    operand = value;
    fetch_error = error;
    now = clock;
    fetches = fetch_bad = conversions = slept = scheduled = 0;
    converted = 0;
    void *result = (void *)0x5a5a5a5a;
    if (!setjmp(asleep))
        result = isr80h_command_sleep(0);

    int ok = fetches == 1 && !fetch_bad && !scheduled;
    if (want_sleep)
    {
        struct registers expect = before.registers;
        expect.eax = 0;
        uint32_t deadline = clock + model_ticks(value);
        const unsigned char *x = (const unsigned char *)&expect, *y = (const unsigned char *)&registers_at_sleep;
        int regs_ok = 1;
        for (unsigned i = 0; i < sizeof(expect); i++)
            regs_ok &= x[i] == y[i];
        ok &= slept == 1 && slept_deadline == deadline && regs_ok && conversions == 1 && converted == value;
        if (!ok)
            printf("  FAIL %s: value %u now %u: slept %d deadline %u (want %u), saved eax %u, registers %s, "
                   "conversions %d, fetches %d\n", what, value, clock, slept, slept_deadline, deadline,
                   registers_at_sleep.eax, regs_ok ? "kept" : "changed", conversions, fetches);
    }
    else
    {
        ok &= !slept && (int)(intptr_t)result == want && same_task(&caller, &before);
        if (!ok)
            printf("  FAIL %s: value 0x%x error %d: result %d (want %d), slept %d, task %s, task_next %d, "
                   "fetches %d\n", what, value, error, (int)(intptr_t)result, want, slept,
                   same_task(&caller, &before) ? "unchanged" : "changed", scheduled, fetches);
    }
    if (!ok)
        failed++;
}

int main(void)
{
    // A handler that waits for the clock in a loop never returns here: stop after 20 s.
    alarm(20);
    static const uint32_t clocks[] = { 0, 5, 0x7FFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFFFu };
    static const uint32_t good[] = { 1, 9, 10, 11, 999, 1000, 1001, 0x7FFFFFFFu };
    static const uint32_t bad[] = { 0xFFFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFF9Cu };

    run("fetch", 1000, -EINVARG, 5, 0, -EINVARG);
    run("fetch", 1000, -EIO, 5, 0, -EIO);
    for (unsigned c = 0; c < sizeof(clocks) / sizeof(clocks[0]); c++)
    {
        run("zero", 0, 0, clocks[c], 0, 0);
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            run("reject", bad[i], 0, clocks[c], 0, -EINVARG);
        for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
            run("sleep", good[i], 0, clocks[c], 1, 0);
    }
    printf("sleep host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
