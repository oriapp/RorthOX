#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stddef.h>
#include <stdint.h>
#include "vga.h"


#ifdef __cplusplus
extern "C" {
#endif


#pragma GCC diagnostic ignored "-Wunused-variable" // Ignore unused vars
static uint16_t *video_mem = 0;
static uint16_t terminal_row = 0;
static uint16_t terminal_col = 0;
static uint8_t terminal_color = 0;
static uint8_t termianl_background_color = 0;
#pragma GCC diagnostic pop // POP ignore unused vars

void terminal_putchar(int x, int y, char c, char colour);
void terminal_backspace();
void terminal_writechar(char c, char colour);
void update_cursor(uint8_t x, uint8_t y);
void terminal_panic(vga_color_t bg_colour);
void terminal_initialize(vga_color_t bg_colour);
void print(const char* str);
int printf(const char *format, ...);
void printc(const char* str, unsigned int color);

/*
* Debug I/O
*/
void dbg_putc(char c);
void dbg_puts(const char *str);

#ifdef __cplusplus
}
#endif

#endif
