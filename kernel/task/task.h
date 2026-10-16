#ifndef TASK_H
#define TASK_H

#include <stddef.h>
#include "config.h"
#include "memory/paging/paging.h"

#ifdef __cplusplus
extern "C" {
#endif

struct interrupt_frame;

// This will hold the CPU registers
// Because if we decide making it multi-tasking we will need to be able to flik between them
struct registers
{
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;

    uint32_t ip; // command that was last executed
    uint32_t cs;
    uint32_t flags;
    uint32_t esp;
    uint32_t ss;
};

// task.asm (task_return, restore_general_purpose_registers) reads these by offset
_Static_assert(offsetof(struct registers, ip) == 28, "task.asm reads ip at 28");
_Static_assert(offsetof(struct registers, cs) == 32, "task.asm reads cs at 32");
_Static_assert(offsetof(struct registers, flags) == 36, "task.asm reads flags at 36");
_Static_assert(offsetof(struct registers, esp) == 40, "task.asm reads esp at 40");
_Static_assert(offsetof(struct registers, ss) == 44, "task.asm reads ss at 44");
_Static_assert(sizeof(struct registers) == 48, "struct registers must stay 48 bytes");

struct process;

struct task
{
    /*
    *   Task Identifier
    */
    unsigned int id;

    /*
    *   The page directory of the task
    */
    struct paging_4gb_chunk *page_directory;

    /*
    *   The registers of the task when the task is not running
    */
    struct registers registers;

    /*
    *   The process of the task
    */
    struct process *process;

    /*
    *   [Scheduler] Time required to complete the task
    */
    int burstTime;

    /*
    *   [Scheduler] Remaining time to complete the task
    */
    int remainingTime;

    /*
    *   The next task in the linked list
    */
    struct task *next;

    /*
    *   Previous in the linked list
    */
   struct task *prev;
};

// Construct a detached task; publish it exactly once after setup succeeds.
struct task *task_new(struct process *process);
void task_publish(struct task *task);
// Destroy a detached task whose page directory is inactive.
void task_discard(struct task *task);
struct task *task_current();
struct task *task_get_next();
// Unlink and destroy a published task.
int task_free(struct task *task);

void task_next();
int task_switch(struct task *task);
int task_page();
int task_page_task(struct task *task);

void task_run_first_ever_task();

// Will drop us into user land
void task_return(struct registers *regs);

void restore_general_purpose_registers(struct registers *regs);
void user_registers();

void task_current_save_state(struct interrupt_frame *frame);
// Checked copies require kernel CR3, IF=0 and a kernel-owned buffer.
int task_copy_from_user(struct task *task, void *destination, uint32_t source, uint32_t size);
int task_copy_to_user(struct task *task, uint32_t destination, const void *source, uint32_t size);
int copy_string_from_task(struct task *task, void *virtual, void *phys, int max);
int task_get_stack_item(struct task *task, int index, uint32_t *value);
void *task_virtual_address_to_physical(struct task *task, void *virtual_address);

#ifdef __cplusplus
}
#endif

#endif // !TASK_H