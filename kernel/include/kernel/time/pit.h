#ifndef _KERNEL_TIME_PIT
#define _KERNEL_TIME_PIT
#include <stdint.h>

#define PIT_COMMAND 0x43
#define PIT_COUNTER0 0x40
#define PIT_FREQUENCY 1000
#define PIT_MODE 0x36

void pit_send_command(uint8_t command);
void pit_send_data(uint16_t data, uint8_t counter_number);
void pit_init();
// Unused busy-wait stub; the implementation's unit is seconds.
void pit_sleep(int sec);
void pit_configure(uint16_t frequency);

#endif // !_KERNEL_TIME_PIT