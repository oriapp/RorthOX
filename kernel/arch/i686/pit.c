// #include "include/kernel/time/pit.h"
// #include "io/io.h"
// #include <stdint.h>

// volatile static unsigned int freq = 0;

// /// @brief Sends the command byte to the PIT
// /// @param command byte to send
// void pit_send_command(uint8_t command)
// {
//     // I think i do it somewhere else already
//     outb(PIT_COMMAND, command);
// }

// /// @brief Sends the data byte to the PIT
// /// @param data byte to send
// void pit_send_data(uint16_t data, uint8_t counter_number)
// {
//     uint8_t port = (counter_number == 0) ? PIT_COUNTER0 : PIT_COUNTER0 + 2;
//     outb(port, (uint8_t)data);
//     outb(port, (uint8_t)(data >> 8));
// }

// /// @brief Initializes the PIT
// void pit_init()
// {
//     pit_send_command(PIT_MODE);
//     pit_send_data(PIT_FREQUENCY, 0);
// }

// /// @brief Sleeps for the specified number of milliseconds
// /// @param milliseconds to sleep
// void pit_sleep(uint32_t milliseconds)
// {
//     uint32_t ticks = milliseconds * PIT_FREQUENCY / 1000;
    
//     uint32_t current_tick = 0;

//     // pit_init();
//     while (current_tick < ticks)
//     {
//         // Wait for the next tick
//         // ...

//         ++current_tick;
//     }
// }

// void pit_configure(uint16_t frequency)
// {
//     uint16_t divisor = 1193180 / frequency;
//     uint8_t l = (uint8_t)(divisor & 0xFF);
//     uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

//     pit_send_command(0x36); // set mode and access mode for channel 0
//     pit_send_data(l, 0);
//     pit_send_data(h, 0);
// }

#include "include/kernel/time/pit.h"
#include "io/io.h"
#include <stdint.h>

typedef struct listnode {
	struct listnode * prev;
	struct listnode * next;
	void * val;
}listnode_t;

typedef struct list{
	listnode_t * head;
	listnode_t * tail;
	uint32_t size;
}list_t;

// Number of ticks since system booted
uint32_t jiffies = 0;
uint16_t hz = 0;
#define INPUT_CLOCK_FREQUENCY 1193180
#define TIMER_COMMAND 0x43
#define TIMER_DATA 0x40
#define TIMER_ICW 0x36
// Functions that want to be woke up
list_t * wakeup_list;
/*
 * Init timer by register irq
 * */
void timer_init() {
    // IRQ0 will fire 100 times per second
    // set_frequency(100);
    // register_interrupt_handler(32, timer_handler);
    // wakeup_list = list_create();
}

/*
 * Simulate sleep() in C, bad implementation, can use it for debug maybe ?
 * */
// void sleep(int sec) {
//     uint32_t end = jiffies + sec * hz;
//     while(jiffies < end);
// }

/*
 * Originally, the frequency is 18 hz, u can change it to whatever you want
 * */
// void set_frequency(uint16_t h) {
//     hz = h;
//     uint16_t divisor = INPUT_CLOCK_FREQUENCY / h;
//     // Init, Square Wave Mode, non-BCD, first transfer LSB then MSB
//     outb(TIMER_COMMAND, TIMER_ICW);
//     outb(TIMER_DATA, divisor & 0xFF);
//     outb(TIMER_DATA, (divisor >> 8) & 0xFF);

// }
