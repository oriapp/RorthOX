// #include "io.h"
#include "include/kernel/tty.h"
#include "task/task.h"
#include "keyboard/keyboard.h"
#include "kernel.h"
#include "string/string.h"
#include "status.h"

void *isr80h_command1_print(struct interrupt_frame *frame)
{
    void *user_space_msg_buffer = task_get_stack_item(task_current(), 0);
    char buf[1024]; // this is also the max
    buf[0] = 0;
    int res = copy_string_from_task(task_current(), user_space_msg_buffer, buf, sizeof(buf));
    if (res < 0)
    {
        return ERROR(res);
    }
    
    print(buf);
    return 0;
}

void *isr80h_command11_printc(struct interrupt_frame *frame)
{
    // The colour is an integer (stack item 1). Only the 16 foreground colours are valid.
    uint32_t color = (uint32_t)task_get_stack_item(task_current(), 1);
    if (color > VGA_COLOR_WHITE)
    {
        return ERROR(-EINVARG);
    }

    void *user_space_msg_buffer = task_get_stack_item(task_current(), 0);
    char buf[1024]; // this is also the max
    buf[0] = 0;
    int res = copy_string_from_task(task_current(), user_space_msg_buffer, buf, sizeof(buf));
    if (res < 0)
    {
        return ERROR(res);
    }

    printc(buf, color);
    return 0;
}

void *isr80h_command2_getkey(struct interrupt_frame *frame)
{
    char c = keyboard_pop();
    return (void*)((int)c);
}

void *isr80h_command3_putchar(struct interrupt_frame *frame)
{
    char c = (char)((int) task_get_stack_item(task_current(), 0));
    terminal_writechar(c, 15);
    return 0;
}