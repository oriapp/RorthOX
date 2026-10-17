// #include "io.h"
#include "include/kernel/tty.h"
#include "task/task.h"
#include "keyboard/keyboard.h"
#include "kernel.h"
#include "string/string.h"
#include "status.h"

void *isr80h_command1_print(struct interrupt_frame *frame)
{
    uint32_t user_space_msg_buffer = 0;
    int res = task_get_stack_item(task_current(), 0, &user_space_msg_buffer);
    if (res < 0)
    {
        return ERROR(res);
    }

    char buf[1024]; // this is also the max
    res = copy_string_from_task(task_current(), (void*)user_space_msg_buffer, buf, sizeof(buf));
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
    uint32_t color = 0;
    int res = task_get_stack_item(task_current(), 1, &color);
    if (res < 0)
    {
        return ERROR(res);
    }
    if (color > VGA_COLOR_WHITE)
    {
        return ERROR(-EINVARG);
    }

    uint32_t user_space_msg_buffer = 0;
    res = task_get_stack_item(task_current(), 0, &user_space_msg_buffer);
    if (res < 0)
    {
        return ERROR(res);
    }

    char buf[1024]; // this is also the max
    res = copy_string_from_task(task_current(), (void*)user_space_msg_buffer, buf, sizeof(buf));
    if (res < 0)
    {
        return ERROR(res);
    }

    printc(buf, color);
    return 0;
}

void *isr80h_command2_getkey(struct interrupt_frame *frame)
{
    // The whole key value: a byte (1-255) or a RORTHOX_KEY_* value, 0 when there is none
    uint16_t key = keyboard_pop();
    return (void*)(uintptr_t)key;
}

void *isr80h_command3_putchar(struct interrupt_frame *frame)
{
    uint32_t c = 0;
    int res = task_get_stack_item(task_current(), 0, &c);
    if (res < 0)
    {
        return ERROR(res);
    }

    terminal_writechar((char)c, 15);
    return 0;
}

void *isr80h_command_terminal_move_cursor(struct interrupt_frame *frame)
{
    // Stack item 0 is a signed cell count; the move is checked against the screen in the tty
    uint32_t delta = 0;
    int res = task_get_stack_item(task_current(), 0, &delta);
    if (res < 0)
    {
        return ERROR(res);
    }

    res = terminal_move_cursor((int)delta);
    return ERROR(res);
}
