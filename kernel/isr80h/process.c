#include "kernel.h"
#include "process.h"
#include "task/task.h"
#include "task/process.h"
#include "status.h"
#include "config.h"
#include "string/string.h"

void *isr80h_command6_process_load_start(struct interrupt_frame *frame)
{
    void *filename_user_ptr = task_get_stack_item(task_current(), 0);
    char filename[RORTHOX_MAX_PATH];
    int res = copy_string_from_task(task_current(), filename_user_ptr, filename, sizeof(filename));
    if (res < 0)
    {
        goto out;
    }

    char path[RORTHOX_MAX_PATH]; // TODO: look programs up through a PATH environment variable instead of always using 0:/
    if (3 + strnlen(filename, sizeof(filename)) + 1 > sizeof(path))
    {
        return ERROR(-EINVARG);
    }

    strcpy(path, "0:/");
    strncpy(path+3, filename, sizeof(path) - 3);

    struct process *process = 0;
    res = process_load_switch(path, &process);
    if (res < 0)
    {
        goto out;
    }

    task_switch(process->task);
    task_return(&process->task->registers); // change all the CPU registers and drop us to lower privilege state


out:
    return 0;
}

// Change this when multitasking
void *isr80h_command7_invoke_system_command(struct interrupt_frame *frame)
{
    struct command_argument *arguments = task_virtual_address_to_physical(task_current(), task_get_stack_item(task_current(), 0));
    if (!arguments || arguments[0].argument[0] == 0)
    {
        return ERROR(-EINVARG);
    }

    struct command_argument *root_command_argument = &arguments[0];
    const char  *program_name = root_command_argument->argument;

    char path[RORTHOX_MAX_PATH];
    if (3 + strnlen(program_name, sizeof(root_command_argument->argument)) + 1 > sizeof(path))
    {
        return ERROR(-EINVARG);
    }

    strcpy(path, "0:/");
    strncpy(path+3, program_name, sizeof(path) - 3);
    
    struct process *process = 0;
    int res = process_load_switch(path, &process);
    if (res < 0)
    {
        return ERROR(res);
    }

    res = process_inject_arguments(process, root_command_argument);
    if (res < 0)
    {
        return ERROR(res);
    }

    task_switch(process->task);
    task_return(&process->task->registers);

    return 0;
}

void *isr80h_command8_get_program_arguments(struct interrupt_frame *frame)
{
    struct process *process = task_current()->process;
    struct process_arguments *arguments = task_virtual_address_to_physical(task_current(), task_get_stack_item(task_current(), 0));

    process_get_arguments(process, &arguments->argc, &arguments->argv);

    return 0;
}

void *isr80h_command9_exit(struct interrupt_frame *frame)
{
    struct process *process = task_current()->process;
    process_terminate(process);
    task_next();

    return 0;
}
