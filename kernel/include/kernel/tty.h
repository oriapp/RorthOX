#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ANSI_COLOR_RED,
    ANSI_COLOR_GREEN,
    ANSI_COLOR_YELLOW,
    ANSI_COLOR_BLUE,
    ANSI_COLOR_MAGENTA,
    ANSI_COLOR_CYAN,
    ANSI_COLOR_RESET
} ansi_color_t;


#ifdef MAINFILE
    static uint16_t *video_mem = 0;
    static uint16_t terminal_row = 0;
    static uint16_t terminal_col = 0;
#endif

uint16_t terminal_make_char(char c, char colour);
void terminal_putchar(int x, int y, char c, char colour);
void terminal_backspace();
void terminal_writechar(char c, char colour);
void terminal_initialize();
void print(const char* str);

/*
* Debug I/O
*/
void dbg_putc(char c);
void dbg_puts(const char *str);

// void terminal_initialize(void);
// void terminal_putchar(char c);
// void terminal_write(const char* data, size_t size);
// void terminal_writestring(const char* data);

#endif
