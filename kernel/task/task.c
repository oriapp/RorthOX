#include "task.h"
#include "kernel.h"
#include "status.h"
#include "process.h"
#include "memory/heap/kheap.h"
#include "memory/memory.h"
#include "string/string.h"
#include "memory/paging/paging.h"
#include "loader/formats/elfloader.h"
#include "idt/idt.h"
#include "include/kernel/sys.h"

// The current task that is running
struct task *current_task = 0;

// Keep count of the tasks, night be useful (?)
static unsigned int tasks = 0;

// Task linked list
struct task *task_tail = 0;
struct task *task_head = 0;

int task_init(struct task *task, struct process *process);

struct task *task_current()
{
    return current_task;
}

struct task *task_new(struct process *process)
{
    int burstTime = 1000;
    int res = 0;
    struct task *task = kzalloc(sizeof(struct task));
    if (!task)
    {
        res = -ENOMEM;
        goto out;
    }

    res = task_init(task, process);
    if (res != RORTHOX_ALL_OK)
    {
        goto out;
    }

    task->id = tasks++;
    task->burstTime = burstTime;
    task->remainingTime = burstTime;

out:
    if (ISERR(res))
    {
        // Initialization failed before the task was linked.
        if (task)
        {
            kfree(task);
        }
        return ERROR(res);
    }

    return task;
}

void task_publish(struct task *task)
{
    if (task_head == 0)
    {
        task_head = task;
        task_tail = task;
        current_task = task;
        return;
    }

    task_tail->next = task;
    task->prev = task_tail;
    task_tail = task;
}

// The first runnable task after the current one, wrapping from the tail to the head, so the
// current task itself comes last. NULL if the list is empty or every task sleeps.
struct task *task_get_next()
{
    struct task *start = (current_task && current_task->next) ? current_task->next : task_head;
    struct task *task = start;
    while (task)
    {
        if (task->state == TASK_RUNNABLE)
        {
            return task;
        }

        task = task->next ? task->next : task_head;
        if (task == start)
        {
            break;
        }
    }

    return 0;
}

static void task_list_remove(struct task *task)
{
    if (task->prev)
    {
        task->prev->next = task->next;
    }

    if (task->next)
    {
        task->next->prev = task->prev;
    }

    if (task == task_head)
    {
        task_head = task->next;
    }

    if (task == task_tail)
    {
        task_tail = task->prev;
    }

    if (task == current_task)
    {
        current_task = task->next ? task->next : task_head;
    }

    task->prev = 0;
    task->next = 0;
}

void task_discard(struct task *task)
{
    if (!task)
    {
        return;
    }

    if (task->page_directory)
    {
        paging_free_4gb(task->page_directory);
    }

    // Finally free the task data
    kfree(task);
}

int task_free(struct task *task)
{
    if (!task)
    {
        return 0;
    }

    task_list_remove(task); // remove from the list
    task_discard(task);
    return 0;
}

void task_next()
{
    if (!task_head)
    {
        PANIC("No more tasks\n", __FILE__, __LINE__);
    }

    struct task *next_task = task_get_next();
    if (!next_task)
    {
        // Every task sleeps. Halt with interrupts on, on this kernel stack: IRQs from here
        // come back to this loop (interrupt_handler keeps the kernel addressing for ring 0), and
        // only the PIT tick wakes tasks. Any return from the halt may be a key, an NMI or a
        // tick with nothing due, so look again each time with IF=0. sti;hlt are back to back in
        // IRQ_ON_AND_PAUSE, so an IRQ that comes after the last look still ends the halt.
        // IRQ callbacks never free a task, so the list stays the same here.
        kernel_page();
        while (!(next_task = task_get_next()))
        {
            IRQ_ON_AND_PAUSE();
        }
    }

    task_switch(next_task);
    task_return(&next_task->registers);
}

void task_sleep_until(uint32_t deadline)
{
    struct task *task = task_current();
    if (!task)
    {
        PANIC("No current task to put to sleep\n", __FILE__, __LINE__);
    }

    task->wake_tick = deadline;
    task->state = TASK_SLEEPING;
    task_next();
}

void task_wake_sleepers(uint32_t now)
{
    for (struct task *task = task_head; task; task = task->next)
    {
        // Due when now is at or past the deadline, also across the 2^32 wrap: delays stay
        // below 2^31 ticks (pit.c), so a deadline still ahead is less than 2^31 away.
        if (task->state == TASK_SLEEPING && (uint32_t)(now - task->wake_tick) < 0x80000000U)
        {
            task->state = TASK_RUNNABLE;
        }
    }
}

int task_switch(struct task *task)
{
    current_task = task;
    paging_switch(task->page_directory);
    return 0;
}

void task_save_state(struct task *task, struct interrupt_frame *frame)
{
    task->registers.ip = frame->ip;
    task->registers.cs = frame->cs;
    task->registers.flags = frame->flags;
    task->registers.esp = frame->esp;
    task->registers.ss = frame->ss;
    task->registers.eax = frame->eax;
    task->registers.ebp = frame->ebp;
    task->registers.ebx = frame->ebx;
    task->registers.ecx = frame->ecx;
    task->registers.edi = frame->edi;
    task->registers.edx = frame->edx;
    task->registers.esi = frame->esi;
}

static int task_user_fragment(struct task *task, uint32_t address, uint32_t remaining,
    int write, void **physical, uint32_t *size)
{
    uint32_t available = 0;
    int res = process_user_span(task->process, address, write, &available);
    if (res < 0)
    {
        return res;
    }
    res = paging_user_translate(task->page_directory->directory_entry, address, write, physical);
    if (res < 0)
    {
        return res;
    }

    uint32_t page_remaining = PAGING_PAGE_SIZE - (address & (PAGING_PAGE_SIZE - 1));
    *size = remaining;
    if (*size > available)
    {
        *size = available;
    }
    if (*size > page_remaining)
    {
        *size = page_remaining;
    }
    return 0;
}

// Kernel CR3 and IF=0 keep ownership and mappings stable across both passes.
static int task_copy_user(struct task *task, uint32_t address, void *buffer, uint32_t size, int write)
{
    if (!task || !task->process || !task->page_directory || !buffer || !address || !size ||
        size - 1 > UINT32_MAX - address)
    {
        return -EINVARG;
    }

    // Preflight every byte before changing either destination.
    for (int copy = 0; copy < 2; copy++)
    {
        uint32_t remaining = size;
        uint32_t current = address;
        char *position = buffer;
        while (remaining)
        {
            void *physical = 0;
            uint32_t fragment = 0;
            int res = task_user_fragment(task, current, remaining, write, &physical, &fragment);
            if (res < 0)
            {
                return res;
            }
            if (copy)
            {
                if (write)
                {
                    memcpy(physical, position, fragment);
                }
                else
                {
                    memcpy(position, physical, fragment);
                }
            }
            remaining -= fragment;
            if (remaining)
            {
                current += fragment;
                position += fragment;
            }
        }
    }

    return 0;
}

int task_copy_from_user(struct task *task, void *destination, uint32_t source, uint32_t size)
{
    return task_copy_user(task, source, destination, size, 0);
}

int task_copy_to_user(struct task *task, uint32_t destination, const void *source, uint32_t size)
{
    return task_copy_user(task, destination, (void*)source, size, 1);
}

// Copies a NUL-terminated string from user memory through the same ownership and page checks as
// task_copy_from_user, one fragment at a time, so it may end anywhere inside owned memory.
// A string longer than max - 1 is cut and still terminated.
int copy_string_from_task(struct task *task, void *virtual, void *phys, int max)
{
    uint32_t address = (uint32_t)virtual;
    if (!task || !task->process || !task->page_directory || !phys || max <= 0 || !address)
    {
        return -EINVARG;
    }

    char *out = phys;
    uint32_t remaining = (uint32_t)max - 1;
    while (remaining)
    {
        void *physical = 0;
        uint32_t fragment = 0;
        int res = task_user_fragment(task, address, remaining, 0, &physical, &fragment);
        if (res < 0)
        {
            *out = 0;
            return res;
        }

        const char *in = physical;
        for (uint32_t i = 0; i < fragment; i++)
        {
            out[i] = in[i];
            if (!in[i])
            {
                return 0;
            }
        }

        if (fragment > UINT32_MAX - address)
        {
            break;
        }
        out += fragment;
        address += fragment;
        remaining -= fragment;
    }

    *out = 0;
    return 0;
}

void task_current_save_state(struct interrupt_frame *frame)
{
    if (!task_current())
    {
        PANIC("No current task to save\n", __FILE__, __LINE__);
    }

    struct task *task = task_current();
    task_save_state(task, frame);
}

/*
    Take us out of the kernel page (page directory)
    and loads us into the task page directory.
*/
int task_page()
{
    user_registers();
    task_switch(current_task);
    return 0;
}

int task_page_task(struct task *task)
{
    user_registers();
    paging_switch(task->page_directory);
    return 0;
}

/*
    The current system is single-threded
    There is no ability for multiple tasks to be running at once.
    the way have have written this code allows for it to be implemented more easily later on.
    Multitasking is just flipping the register back and forth.
*/
void task_run_first_ever_task()
{
    if (!current_task)
    {
        PANIC("task_run_first_ever_task(): No current task exists!\n", __FILE__, __LINE__);
    }

    task_switch(task_head);
    task_return(&task_head->registers);
}

int task_init(struct task *task, struct process *process)
{
    memset(task, 0, sizeof(struct task));
    // Map the entire 4GB address space to its self
    task->page_directory = paging_new_4gb(PAGING_IS_PRESENT | PAGING_ACCESS_FROM_ALL); // Is it vulnerable? should it be access from all?
    if (!task->page_directory)
    {
        return -EIO;
    }

    task->registers.ip = RORTHOX_PROGRAM_VIRTUAL_ADDRESS;
    if (process->filetype == PROCESS_FILETYPE_ELF)
    {
        task->registers.ip = elf_header(process->elf_file)->e_entry;
    }

    task->registers.ss = USER_DATA_SEGMENT;
    task->registers.cs = USER_CODE_SEGMENT;
    task->registers.esp = RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START;

    task->process = process;
    task->state = TASK_RUNNABLE;
    task->wake_tick = 0;

    return 0;
}

// Reads syscall argument `index` (a 32-bit word above the saved user esp) through a checked copy.
int task_get_stack_item(struct task *task, int index, uint32_t *value)
{
    if (!task || index < 0 || !value)
    {
        return -EINVARG;
    }

    uint32_t offset = (uint32_t)index * sizeof(uint32_t);
    if (offset > UINT32_MAX - task->registers.esp)
    {
        return -EINVARG;
    }

    return task_copy_from_user(task, value, task->registers.esp + offset, sizeof(*value));
}

void *task_virtual_address_to_physical(struct task *task, void *virtual_address)
{
    return paging_get_physical_address(task->page_directory->directory_entry, virtual_address);
}
