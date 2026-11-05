/*
 * Host check of the tick syscall handler (syscall 19) in kernel/isr80h/misc.c, for
 * scripts/tests/clock.sh. The real isr80h_command19_get_ticks() is included here; the rest of
 * misc.c is dropped by the linker. The handler takes no operands: it must call pit_get_ticks()
 * once and return its value unchanged, with no stack item read, no task lookup and no access to
 * the interrupt frame (it gets a frame pointer into an unmapped page, so any read faults).
 * Values: 0, 1, 0x7fffffff and the high-bit values 0x80000000, 0xfffffffe (-EINVARG as an
 * int), 0xffffffff. Compared in 32 bits, the width of a result register in the guest.
 * Prints "PASS <value>" or "FAIL <value>" and "clock host check: <failed> of <cases> cases failed".
 */
#include "isr80h/misc.c"
#include "host.h"

void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
#define HOST_PROT_NONE 0
#define HOST_MAP_PRIVATE_ANON 0x22

static uint32_t now;
static int clock_reads, task_lookups, stack_reads;

uint32_t pit_get_ticks(void)
{
    clock_reads++;
    return now;
}

struct task *task_current()
{
    task_lookups++;
    return 0;
}

int task_get_stack_item(struct task *task, int index, uint32_t *value)
{
    stack_reads++;
    *value = 0x5a5a5a5a;
    return 0;
}

int main(void)
{
    static const uint32_t values[] = { 0, 1, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu };
    struct interrupt_frame *frame = mmap(0, 4096, HOST_PROT_NONE, HOST_MAP_PRIVATE_ANON, -1, 0);
    if (frame == (void *)-1)
    {
        perror("mmap");
        return 2;
    }
    int failed = 0, cases = 0;
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        cases++;
        now = values[i];
        clock_reads = task_lookups = stack_reads = 0;
        void *result = isr80h_command19_get_ticks(frame);
        uint32_t got = (uint32_t)(uintptr_t)result;
        int ok = got == values[i] && clock_reads == 1 && task_lookups == 0 && stack_reads == 0;
        printf("%s 0x%x: returned 0x%x, %d clock read(s), %d task lookup(s), %d stack read(s)\n",
               ok ? "PASS" : "FAIL", values[i], got, clock_reads, task_lookups, stack_reads);
        failed += !ok;
    }
    printf("clock host check: %d of %d cases failed\n", failed, cases);
    fflush(0);
    return failed ? 1 : 0;
}
