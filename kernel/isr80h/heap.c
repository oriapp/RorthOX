#include "heap.h"
#include "task/task.h"
#include "task/process.h"
#include "kernel.h"
#include <stddef.h>

void *isr80h_command4_malloc(struct interrupt_frame *frame)
{
    uint32_t size = 0;
    if (task_get_stack_item(task_current(), 0, &size) < 0)
    {
        return 0; // malloc's failure value
    }
    return process_malloc(task_current()->process, size);
}

void *isr80h_command5_free(struct interrupt_frame *frame)
{
    uint32_t ptr_to_free = 0;
    int res = task_get_stack_item(task_current(), 0, &ptr_to_free);
    if (res < 0)
    {
        return ERROR(res);
    }
    process_free(task_current()->process, (void*)ptr_to_free);
    return 0;
}
