#include "misc.h"
#include "idt/idt.h"
#include "task/task.h"
#include "kernel.h"
#include "include/kernel/time/pit.h"

void *isr80h_command0_sum(struct interrupt_frame *frame)
{
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    int res = task_get_stack_item(task_current(), 0, &v1);
    if (res == 0)
    {
        res = task_get_stack_item(task_current(), 1, &v2);
    }
    if (res < 0)
    {
        return ERROR(res);
    }

    return (void*)(v1 + v2);
}

void *isr80h_command19_get_ticks(struct interrupt_frame *frame)
{
    return (void*)pit_get_ticks();
}
