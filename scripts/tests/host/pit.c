/* Pin the real driver's channel-0 programming and non-returning tick order. */
#include "arch/i686/pit.c"
#include "host.h"

static int writes, registrations, phase;
static INTERRUPT_CALLBACK_FUNCTION callback;

void outb(unsigned short port, unsigned char value)
{
    const unsigned short ports[] = {0x43, 0x40, 0x40};
    const unsigned char values[] = {0x36, 0x9b, 0x2e};
    if (writes >= 3 || port != ports[writes] || value != values[writes])
        exit(1);
    printf("outb(0x%x, 0x%x)\n", port, value);
    writes++;
}

int idt_register_interrupt_callback(int vector, INTERRUPT_CALLBACK_FUNCTION cb)
{
    if (vector != RORTHOX_PIC_MASTER_VECTOR_BASE || !cb || registrations++)
        exit(2);
    callback = cb;
    return 0;
}

void pic_send_eoi(int vector)
{
    if (vector != RORTHOX_PIC_MASTER_VECTOR_BASE || phase != 0 || jiffies != 0)
        exit(3);
    phase = 1;
}

void task_next()
{
    if (phase != 1 || jiffies != 1)
        exit(4);
    printf("PIT: divisor 11931, mode 3, IRQ0, EOI then tick then task_next: passed\n");
    exit(0); // Like the real task switch, this must not return to the callback.
}

int main(void)
{
    pit_init();
    if (writes != 3 || registrations != 1 || !callback)
        return 5;
    pit_sleep(0);
    callback();
    return 6;
}
