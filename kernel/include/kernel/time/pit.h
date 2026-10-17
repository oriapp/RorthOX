#ifndef _KERNEL_TIME_PIT
#define _KERNEL_TIME_PIT
#include <stdint.h>

#define PIT_FREQUENCY_HZ 100

void pit_init();
// Ticks since boot (jiffies), one per IRQ0 that reached the kernel. Wraps at 2^32.
uint32_t pit_get_ticks();
// Ticks to wait for a sleep of at least milliseconds (0..INT32_MAX): ceil(ms * PIT_FREQUENCY_HZ
// / 1000) + 2, and 0 for 0. "At least" is counted in ticks as delivered, each at least 1/PIT_FREQUENCY_HZ s of
// the nominal 1193182 Hz clock. Dropped ticks only make a sleep longer. Not calibrated time: under
// QEMU TCG a late tick can make a sleep up to one tick short of the virtual clock, and KVM's
// in-kernel PIT replays lost ticks, which this does not cover.
uint32_t pit_ms_to_ticks(uint32_t milliseconds);

#endif // !_KERNEL_TIME_PIT
