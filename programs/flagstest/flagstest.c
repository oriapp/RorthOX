/*
 * EFLAGS test for scripts/tests/task-flags.sh (only copied into the test image).
 *
 * After "flagstest: armed" it waits for a key and then for the test's debugger (wait_for_debugger),
 * then sets the arithmetic flags, DF, AC and ID to a few patterns (all set, all clear, two halves)
 * and enters the timer vector with "int", so the kernel saves this task, runs the next one and
 * comes back through task_return. The flags must come back as they were, with IF on and IOPL 0.
 * The same patterns go through a syscall too. Prints "flagstest: PASS" or "flagstest: FAIL".
 *
 * No globals: the loader doesn't map .bss.
 */
#include <stdint.h>
#include "rorthox.h"

#define TESTED_FLAGS 0x00240cd5
#define FLAG_IF 0x00000200
#define FLAG_IOPL_NT 0x00007000

uint32_t flags_after_timer(uint32_t flags, uint32_t *before);
uint32_t flags_after_syscall(uint32_t flags, uint32_t *before);
void wait_for_debugger(void);

static int flags_ok(uint32_t want, uint32_t before, uint32_t after)
{
    return (before & TESTED_FLAGS) == want && (after & TESTED_FLAGS) == want &&
           (after & FLAG_IF) && !(after & FLAG_IOPL_NT);
}

int main(int argc, char **argv)
{
    uint32_t patterns[4] = { TESTED_FLAGS, 0, 0x00040491, 0x00200844 };
    uint32_t before = 0;
    uint32_t after = 0;
    int ok = 1;

    print("flagstest: armed\n");
    rorthox_getkeyblock();
    wait_for_debugger();

    for (int i = 0; i < 4; i++)
    {
        after = flags_after_timer(patterns[i], &before);
        if (!flags_ok(patterns[i], before, after))
        {
            ok = 0;
        }
    }

    for (int i = 0; i < 4; i++)
    {
        after = flags_after_syscall(patterns[i], &before);
        if (!flags_ok(patterns[i], before, after))
        {
            ok = 0;
        }
    }

    print(ok ? "flagstest: PASS\n" : "flagstest: FAIL\n");
    return 0;
}
