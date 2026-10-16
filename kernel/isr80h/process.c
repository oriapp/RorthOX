#include "kernel.h"
#include "process.h"
#include "task/task.h"
#include "task/process.h"
#include "status.h"
#include "config.h"
#include "string/string.h"
#include "memory/heap/kheap.h"

void *isr80h_command6_process_load_start(struct interrupt_frame *frame)
{
    uint32_t filename_user_ptr = 0;
    char filename[RORTHOX_MAX_PATH];
    int res = task_get_stack_item(task_current(), 0, &filename_user_ptr);
    if (res < 0)
    {
        goto out;
    }
    res = copy_string_from_task(task_current(), (void*)filename_user_ptr, filename, sizeof(filename));
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

static struct command_argument *isr80h_copy_command_arguments(struct task *task, uint32_t address)
{
    if (!address)
    {
        return ERROR(-EINVARG);
    }

    struct command_argument *arguments = kzalloc(RORTHOX_MAX_COMMAND_ARGUMENTS * sizeof(struct command_argument));
    if (!arguments)
    {
        return ERROR(-ENOMEM);
    }

    int res = 0;
    int count = 0;
    while (address)
    {
        // A non-NULL continuation at the bound rejects cycles without reading another node.
        if (count >= RORTHOX_MAX_COMMAND_ARGUMENTS)
        {
            res = -EINVARG;
            goto out;
        }

        struct command_argument *argument = &arguments[count];
        res = task_copy_from_user(task, argument, address, sizeof(*argument));
        if (res < 0)
        {
            goto out;
        }
        if (strnlen(argument->argument, sizeof(argument->argument)) == sizeof(argument->argument))
        {
            res = -EINVARG;
            goto out;
        }

        address = (uint32_t)argument->next;
        argument->next = 0;
        if (count > 0)
        {
            arguments[count - 1].next = argument;
        }
        count++;
    }

    return arguments;

out:
    kfree(arguments);
    return ERROR(res);
}

void *isr80h_command7_invoke_system_command(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t root = 0;
    int res = task_copy_from_user(task, &root, task->registers.esp, sizeof(root));
    if (res < 0)
    {
        return ERROR(res);
    }

    struct command_argument *arguments = isr80h_copy_command_arguments(task, root);
    if (ISERR(arguments))
    {
        return arguments;
    }

    const char *program_name = arguments->argument;
    char path[RORTHOX_MAX_PATH];
    if (!program_name[0] || 3 + strnlen(program_name, sizeof(arguments->argument)) + 1 > sizeof(path))
    {
        kfree(arguments);
        return ERROR(-EINVARG);
    }

    strcpy(path, "0:/");
    strncpy(path+3, program_name, sizeof(path) - 3);

    struct process *process = 0;
    res = process_load_with_arguments(path, &process, arguments);
    // task_return does not return through this stack, so release the snapshot first.
    kfree(arguments);
    if (res < 0)
    {
        return ERROR(res);
    }

    process_switch(process);
    task_switch(process->task);
    task_return(&process->task->registers);

    return 0;
}

void *isr80h_command8_get_program_arguments(struct interrupt_frame *frame)
{
    struct task *task = task_current();
    uint32_t destination = 0;
    int res = task_copy_from_user(task, &destination, task->registers.esp, sizeof(destination));
    if (res < 0)
    {
        return ERROR(res);
    }

    struct process_arguments arguments = {0};
    process_get_arguments(task->process, &arguments.argc, &arguments.argv);
    res = task_copy_to_user(task, destination, &arguments, sizeof(arguments));
    if (res < 0)
    {
        return ERROR(res);
    }

    return 0;
}

void *isr80h_command9_exit(struct interrupt_frame *frame)
{
    struct process *process = task_current()->process;
    process_terminate(process);
    task_next();

    return 0;
}
