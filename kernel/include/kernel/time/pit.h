#ifndef _KERNEL_TIME_PIT
#define _KERNEL_TIME_PIT
#include <stdint.h>

#define PIT_FREQUENCY_HZ 100

void pit_init();
// Busy-waits on jiffies in milliseconds, with +/- one tick precision. No caller.
// Not usable from kernel code yet: the tick switches tasks and does not return to ring 0.
void pit_sleep(uint32_t ms);

#endif // !_KERNEL_TIME_PIT
