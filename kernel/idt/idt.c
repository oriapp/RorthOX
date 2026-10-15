#include "idt.h"
#include "config.h"
#include "kernel.h"
#include "memory/memory.h"
#include "task/task.h"
#include "io/io.h"
#include "status.h"
#include "task/process.h"
#include "include/kernel/sys.h"
#include "include/kernel/time/pit.h"
#include "include/kernel/tty.h"

struct idt_desc idt_descriptors[RORTHOX_TOTAL_INTERRUPTS];
struct idtr_desc idtr_descriptor;

_Static_assert(RORTHOX_TOTAL_INTERRUPTS == 256, "x86 has 256 interrupt vectors");

extern void *interrupt_pointer_table[RORTHOX_TOTAL_INTERRUPTS];

static INTERRUPT_CALLBACK_FUNCTION interrupt_callbacks[RORTHOX_TOTAL_INTERRUPTS];

static ISR80H_COMMAND isr80h_commands[RORTHOX_MAX_ISR80H_COMMANDS];

extern void idt_load(struct idtr_desc *ptr);
extern void int21h();
extern void no_interrupt();
extern void isr80h_wrapper();

void no_interrupt_handler()
{
    outb(0x20, 0x20);
}

// EOI the slave first, then the master. Exceptions and syscalls outside both ranges get no EOI.
static void pic_send_eoi(int interrupt)
{
    if (interrupt >= RORTHOX_PIC_SLAVE_VECTOR_BASE && interrupt < RORTHOX_PIC_SLAVE_VECTOR_BASE + 8)
    {
        outb(0xA0, 0x20);
    }

    if ((interrupt >= RORTHOX_PIC_MASTER_VECTOR_BASE && interrupt < RORTHOX_PIC_MASTER_VECTOR_BASE + 8) ||
        (interrupt >= RORTHOX_PIC_SLAVE_VECTOR_BASE && interrupt < RORTHOX_PIC_SLAVE_VECTOR_BASE + 8))
    {
        outb(0x20, 0x20);
    }
}

// Vectors where the CPU pushes an error code. idt.asm has the same list.
static int idt_has_error_code(int interrupt)
{
    switch (interrupt)
    {
        case 8: case 10: case 11: case 12: case 13: case 14: case 17: case 21: case 29: case 30:
            return 1;
    }

    return 0;
}

// Set once a fatal exception is being reported. Anything that comes in after that just halts.
static volatile int exception_fatal = 0;

static void exception_put_hex(uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        dbg_putc("0123456789abcdef"[(value >> shift) & 0xf]);
    }
}

static void __attribute__((noreturn)) exception_fatal_stop(int interrupt, struct interrupt_frame *frame, uint32_t cr2, const char *msg)
{
    exception_fatal = 1;
    dbg_puts("exception: vector=0x");
    exception_put_hex(interrupt);
    dbg_puts(" error=0x");
    exception_put_hex(frame->error_code);
    if (interrupt == 14)
    {
        dbg_puts(" cr2=0x");
        exception_put_hex(cr2);
    }
    dbg_puts("\n");
    PANIC(msg, __FILE__, __LINE__);
}

// CPU exceptions (vectors below 0x20). A fault in ring 3 ends the process of the task that
// was running, the same way as an exit. A fault in the kernel and a double fault halt.
// Only a ring 0 NMI returns.
static void idt_handle_exception(int interrupt, struct interrupt_frame *frame, uint32_t cr2)
{
    // The interrupted code may have had user segments loaded. Don't touch CR3 here: the
    // IDT is live before the kernel page directory exists.
    kernel_registers();
    if (exception_fatal)
    {
        PAUSE();
    }

    int from_user = (frame->cs & 3) == 3;
    if (interrupt == 2)
    {
        // An NMI is not the task's fault. From ring 3, switch tasks as before. From ring 0,
        // go back to the kernel (the stub restores its segments).
        if (from_user)
        {
            kernel_page();
            task_current_save_state(frame);
            task_next();
        }
        return;
    }

    // #MC (18) is not special: CR4.MCE is clear, so a real machine check shuts the CPU down
    // and vector 18 only comes from a software int.
    if (interrupt == 8 || !from_user)
    {
        exception_fatal_stop(interrupt, frame, cr2, "Kernel exception");
    }

    // The task that was running, not process_current() (that is the keyboard owner).
    struct task *task = task_current();
    if (!task || !task->process || task->process->task != task ||
        process_get(task->process->id) != task->process)
    {
        exception_fatal_stop(interrupt, frame, cr2, "Invalid faulting task");
    }

    // Leave the task's page directory before process_terminate frees it
    kernel_page();
    if (process_terminate(task->process) < 0)
    {
        exception_fatal_stop(interrupt, frame, cr2, "Failed to terminate faulting process");
    }

    task_next();
}

void interrupt_handler(int interrupt, struct interrupt_frame *frame, uint32_t cr2)
{
    if (interrupt < 0x20)
    {
        idt_handle_exception(interrupt, frame, cr2);
        return;
    }

    kernel_page();
    if (interrupt_callbacks[interrupt] != 0)
    {
        task_current_save_state(frame);
        interrupt_callbacks[interrupt](frame);
    }

    task_page();
    // ack interrupt
    pic_send_eoi(interrupt);
}

void idt_zero()
{
    print("Devide by zero error\n");
}

void idt_set(int interrupt_no, void *address)
{
    struct idt_desc *desc = &idt_descriptors[interrupt_no];
    desc->offset_1 = (uint32_t) address & 0x0000ffff;
    desc->selector = KERNEL_CODE_SELECTOR;
    desc->zero = 0x00;
    // 0xEE: present, DPL 3, 32-bit interrupt gate. Error code vectors get DPL 0 (0x8E):
    // a software int from ring 3 pushes no error code, so it gets #GP instead.
    desc->type_attr = idt_has_error_code(interrupt_no) ? 0x8E : 0xEE;
    desc->offset_2 = (uint32_t) address >> 16;
}

static uint32_t jiffies = 0;
static uint16_t hz = 0;

void idt_clock()
{
    // Send an end-of-interrupt signal to the PIC to let it know that we have handled the interrupt
    pic_send_eoi(RORTHOX_PIC_MASTER_VECTOR_BASE);
    jiffies++;

    // Switch to the next task
    task_next();
}

#define INPUT_CLOCK_FREQUENCY 1193180
#define TIMER_COMMAND 0x43
#define TIMER_DATA 0x40
#define TIMER_ICW 0x36

void pit_sleep(int sec) {
    uint32_t end = jiffies + sec * hz;
    while(jiffies < end);
}

void set_frequency(uint16_t h) {
    hz = h;
    uint16_t divisor = INPUT_CLOCK_FREQUENCY / h;
    // Init, Square Wave Mode, non-BCD, first transfer LSB then MSB
    outb(TIMER_COMMAND, TIMER_ICW);
    outb(TIMER_DATA, divisor & 0xFF);
    outb(TIMER_DATA, (divisor >> 8) & 0xFF);

}

void idt_init()
{
    memset(idt_descriptors, 0, sizeof(idt_descriptors));
    idtr_descriptor.limit = sizeof(idt_descriptors) - 1;
    idtr_descriptor.base = (uint32_t) idt_descriptors;

    for(int i = 0; i <RORTHOX_TOTAL_INTERRUPTS; i++)
    {
        idt_set(i, interrupt_pointer_table[i]);
    }

    // idt_set(0, idt_zero);
    idt_set(RORTHOX_SYSCALL_VECTOR, isr80h_wrapper);

    // set PIT (Programmable Interval Timer) frequency to 1000 Hz
    // set_frequency(100);
    // pit_configure(1000);
    // pit_init();

    idt_register_interrupt_callback(RORTHOX_PIC_MASTER_VECTOR_BASE, idt_clock); // timer interrupt

    // Load the interrupt descriptor table
    idt_load(&idtr_descriptor);
    set_frequency(100);
}

int idt_register_interrupt_callback(int interrupt, INTERRUPT_CALLBACK_FUNCTION interrupt_callback)
{
    if (interrupt < 0 || interrupt >= RORTHOX_TOTAL_INTERRUPTS)
    {
        return -EINVARG;
    }

    interrupt_callbacks[interrupt] = interrupt_callback;
    return 0;
}

void isr80h_register_command(int command_id, ISR80H_COMMAND command)
{
    if (command_id < 0 || command_id >= RORTHOX_MAX_ISR80H_COMMANDS)
    {
       PANIC("The command is out of bounds\n", __FILE__, __LINE__); 
    }

    if (isr80h_commands[command_id])
    {
        PANIC("You are attempting to overwrite an existing command\n", __FILE__, __LINE__);
    }

    isr80h_commands[command_id] = command;
}

void *isr80h_handle_command(int command, struct interrupt_frame *frame)
{
    void *result = 0;

    if(command < 0 || command >= RORTHOX_MAX_ISR80H_COMMANDS)
    {
        // invalid command
        return 0;
    }

    ISR80H_COMMAND command_func = isr80h_commands[command];
    if (!command_func)
    {
        // Command does not exist
        return 0;
    }

    result = command_func(frame);
    return result;
}

void *isr80h_handler(int command, struct interrupt_frame *frame)
{
    void *res = 0;
    kernel_page();
    task_current_save_state(frame); // save the registers from this frame into the task
    res = isr80h_handle_command(command, frame);
    task_page();
    return res;
}
