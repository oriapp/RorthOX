#include "include/kernel/time/pit.h"
#include "io/io.h"
#include "idt/idt.h"
#include "task/task.h"
#include "config.h"

#define INPUT_CLOCK_FREQUENCY 1193182
#define PIT_COMMAND 0x43
#define PIT_COUNTER0 0x40
#define PIT_MODE 0x36

// Round up, so a tick is never shorter than 1/PIT_FREQUENCY_HZ s. 100 Hz: 11932 (0x2E9C), 10.00015 ms.
#define PIT_DIVISOR ((INPUT_CLOCK_FREQUENCY + PIT_FREQUENCY_HZ - 1) / PIT_FREQUENCY_HZ)
// Mode 3 can't count 1, and 0 would mean 65536.
_Static_assert(PIT_DIVISOR >= 2 && PIT_DIVISOR <= 0xFFFF, "PIT divisor must fit the 16-bit count");
_Static_assert((uint32_t)PIT_DIVISOR * PIT_FREQUENCY_HZ >= INPUT_CLOCK_FREQUENCY, "PIT tick must not be shorter than nominal");

// Ticks for a sleep of ms > 0: ceil(ms * PIT_FREQUENCY_HZ / 1000), split so it can't overflow,
// plus 2. One tick may already be waiting in the PIC when the sleep starts, and the next one
// ends a period that started before it, so only the ticks after those two are whole periods.
#define PIT_SLEEP_GUARD_TICKS 2
#define PIT_MS_TO_TICKS(ms) (((ms) / 1000) * PIT_FREQUENCY_HZ + \
    (((ms) % 1000) * PIT_FREQUENCY_HZ + 999) / 1000 + PIT_SLEEP_GUARD_TICKS)
// task_wake_sleepers needs every deadline to be less than 2^31 ticks ahead.
_Static_assert(PIT_MS_TO_TICKS(0x7FFFFFFFULL) < 0x80000000ULL, "longest sleep must stay below 2^31 ticks");

static volatile uint32_t jiffies = 0;

static void pit_tick(struct interrupt_frame *frame)
{
    if ((frame->cs & 3) != 3)
    {
        // The idle loop in task_next: count, wake whoever is due and go back to it. It picks
        // the next task itself, and interrupt_handler sends the EOI.
        jiffies++;
        task_wake_sleepers(jiffies);
        return;
    }

    // task_next does not return here, so acknowledge before switching tasks.
    pic_send_eoi(RORTHOX_PIC_MASTER_VECTOR_BASE);
    jiffies++;
    task_wake_sleepers(jiffies);
    task_next();
}

static void pit_set_frequency(void)
{
    uint16_t divisor = PIT_DIVISOR;
    // Channel 0, square wave, binary count, LSB then MSB.
    outb(PIT_COMMAND, PIT_MODE);
    outb(PIT_COUNTER0, divisor & 0xFF);
    outb(PIT_COUNTER0, (divisor >> 8) & 0xFF);
}

void pit_init()
{
    idt_register_interrupt_callback(RORTHOX_PIC_MASTER_VECTOR_BASE, pit_tick);
    pit_set_frequency();
}

uint32_t pit_get_ticks()
{
    return jiffies;
}

uint32_t pit_ms_to_ticks(uint32_t milliseconds)
{
    if (milliseconds == 0)
    {
        return 0;
    }

    return PIT_MS_TO_TICKS(milliseconds);
}
