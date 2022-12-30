#ifndef KERNEL_IO_H
#define KERNEL_IO_H
#include <stdint.h>

uint16_t terminal_make_char(char c, char colour);

void terminal_putchar(int x, int y, char c, char colour);

void terminal_backspace();

void terminal_writechar(char c, char colour);

void terminal_initialize();

void print(const char* str);

#endif // !KERNEL_IO_H