#include "include/kernel/time/pit.h"
#include "io/io.h"
#include "idt/idt.h"
#include "task/task.h"
#include "config.h"

#define INPUT_CLOCK_FREQUENCY 1193180
#define PIT_COMMAND 0x43
#define PIT_COUNTER0 0x40
#define PIT_MODE 0x36

_Static_assert(PIT_FREQUENCY_HZ >= 19, "PIT frequency must fit the 16-bit divisor");

static volatile uint32_t jiffies = 0;

static void pit_tick()
{
    // task_next does not return here, so acknowledge before switching tasks.
    pic_send_eoi(RORTHOX_PIC_MASTER_VECTOR_BASE);
    jiffies++;
    task_next();
}

static void pit_set_frequency(uint16_t hz)
{
    uint16_t divisor = INPUT_CLOCK_FREQUENCY / hz;
    // Channel 0, square wave, binary count, LSB then MSB.
    outb(PIT_COMMAND, PIT_MODE);
    outb(PIT_COUNTER0, divisor & 0xFF);
    outb(PIT_COUNTER0, (divisor >> 8) & 0xFF);
}

void pit_init()
{
    idt_register_interrupt_callback(RORTHOX_PIC_MASTER_VECTOR_BASE, pit_tick);
    pit_set_frequency(PIT_FREQUENCY_HZ);
}

void pit_sleep(uint32_t ms)
{
    uint32_t ticks = (ms / 1000) * PIT_FREQUENCY_HZ +
                     ((ms % 1000) * PIT_FREQUENCY_HZ + 999) / 1000;
    uint32_t start = jiffies;
    while ((uint32_t)(jiffies - start) < ticks);
}
