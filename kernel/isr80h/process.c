#include "kernel.h"
#include "process.h"
#include "task/task.h"
#include "task/process.h"
#include "status.h"
#include "config.h"
#include "string/string.h"
#include "memory/heap/kheap.h"

static const char *const program_search_paths[] = { RORTHOX_PROGRAM_SEARCH_PATHS };

static int isr80h_load_program(const char *name, struct process **process, struct command_argument *arguments)
{
    char path[RORTHOX_MAX_PATH];
    int name_length = strnlen(name, sizeof(path));
    int bare = name_length > 0;
    for (int i = 0; i < name_length; i++)
    {
        if (name[i] == '/' || name[i] == ':')
        {
            bare = 0;
            break;
        }
    }

    int count = bare ? sizeof(program_search_paths) / sizeof(program_search_paths[0]) : 1;
    for (int i = 0; i < count; i++)
    {
        const char *prefix = bare ? program_search_paths[i] : "0:/";
        int prefix_length = strnlen(prefix, sizeof(path));
        if (prefix_length >= sizeof(path) || name_length >= sizeof(path) - prefix_length)
        {
            continue;
        }

        strcpy(path, prefix);
        strncpy(path + prefix_length, name, sizeof(path) - prefix_length);
        int res = process_load_with_arguments(path, process, arguments);
        if (res != -ENOENT)
        {
            return res;
        }
    }

    return -EIO;
}

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

    if (3 + strnlen(filename, sizeof(filename)) + 1 > RORTHOX_MAX_PATH)
    {
        return ERROR(-EINVARG);
    }

    struct process *process = 0;
    res = isr80h_load_program(filename, &process, NULL);
    if (res < 0)
    {
        goto out;
    }

    process_switch(process);
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
    if (!program_name[0] || 3 + strnlen(program_name, sizeof(arguments->argument)) + 1 > RORTHOX_MAX_PATH)
    {
        kfree(arguments);
        return ERROR(-EINVARG);
    }

    struct process *process = 0;
    res = isr80h_load_program(program_name, &process, arguments);
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
