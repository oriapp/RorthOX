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

struct idt_desc idt_descriptors[POTONGOS_TOTAL_INTERRUPTS];
struct idtr_desc idtr_descriptor;

extern void *interrupt_pointer_table[POTONGOS_TOTAL_INTERRUPTS];

static INTERRUPT_CALLBACK_FUNCTION interrupt_callbacks[POTONGOS_TOTAL_INTERRUPTS];

static ISR80H_COMMAND isr80h_commands[POTONGOS_MAX_ISR80H_COMMANDS];

extern void idt_load(struct idtr_desc *ptr);
extern void int21h();
extern void no_interrupt();
extern void isr80h_wrapper();

void no_interrupt_handler()
{
    outb(0x20, 0x20);
}

void interrupt_handler(int interrupt, struct interrupt_frame *frame)
{
    kernel_page();
    if (interrupt_callbacks[interrupt] != 0)
    {
        task_current_save_state(frame);
        interrupt_callbacks[interrupt](frame);
    }

    task_page();
    // ack interrupt
    outb(0x20, 0x20);
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
    desc->type_attr = 0xEE;
    desc->offset_2 = (uint32_t) address >> 16;
}

void idt_handle_exception()
{
    //// AHHH NOT LINE THIS
    /// process_terminate(task_current()->process);
    task_next();
}

static uint32_t jiffies = 0;
static uint16_t hz = 0;

void idt_clock()
{
    // Send an end-of-interrupt signal to the PIC to let it know that we have handled the interrupt
    outb(0x20, 0x20);
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

    for(int i = 0; i <POTONGOS_TOTAL_INTERRUPTS; i++)
    {
        idt_set(i, interrupt_pointer_table[i]);
    }

    // idt_set(0, idt_zero);
    idt_set(0x80, isr80h_wrapper);

    // set PIT (Programmable Interval Timer) frequency to 1000 Hz
    // set_frequency(100);
    // pit_configure(1000);
    // pit_init();

    int i;
    for (i = 0; i < 0x20; i++)
    {
        idt_register_interrupt_callback(i, idt_handle_exception);
    }

    idt_register_interrupt_callback(0x20, idt_clock); // timer interrupt

    // Load the interrupt descriptor table
    idt_load(&idtr_descriptor);
    set_frequency(100);
}

int idt_register_interrupt_callback(int interrupt, INTERRUPT_CALLBACK_FUNCTION interrupt_callback)
{
    if (interrupt < 0 || interrupt >= POTONGOS_TOTAL_INTERRUPTS)
    {
        return -EINVARG;
    }

    interrupt_callbacks[interrupt] = interrupt_callback;
    return 0;
}

void isr80h_register_command(int command_id, ISR80H_COMMAND command)
{
    if (command_id < 0 || command_id >= POTONGOS_MAX_ISR80H_COMMANDS)
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

    if(command < 0 || command >= POTONGOS_MAX_ISR80H_COMMANDS)
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
