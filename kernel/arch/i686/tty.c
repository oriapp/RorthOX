#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "string/string.h"

#define MAINFILE
#include "kernel.h"
#include "include/kernel/tty.h"
#include "io/io.h"

uint16_t terminal_make_char(char c, char colour)
{
    return (colour << 8) | c; 
}

void terminal_putchar(int x, int y, char c, char colour)
{
    video_mem[(y * VGA_WIDTH) + x] = terminal_make_char(c, colour);
}

void terminal_backspace()
{
    if (terminal_row == 0 && terminal_col == 0)
    {
        return;
    }

    if (terminal_col == 0)
    {
        terminal_row--;
        terminal_col = VGA_WIDTH;
    }

    terminal_col--;
    terminal_writechar(' ', 15);
    terminal_col--;
}

void terminal_writechar(char c, char colour)
{
    if(c == '\n')
    {
        terminal_row++;
        terminal_col = 0;
        return;
    }

    if (c == 0x08)
    {
        terminal_backspace();
        return;
    }
    
    terminal_putchar(terminal_col, terminal_row, c, colour);
    terminal_col++;
    if(terminal_col >= VGA_WIDTH)
    {
        terminal_col = 0;
        terminal_row++;
    }
}

void terminal_initialize()
{
    // Clear the BIOS stuff
    video_mem = (uint16_t*)(0xB8000);
    int x, y;
    terminal_row = 0;
    terminal_col = 0;
    for(y = 0; y < VGA_HEIGHT; y++)
    {
        for(x = 0; x < VGA_WIDTH; x++)
        {
            // video_mem[(y * VGA_WIDTH) + x] =  terminal_make_char(' ', 0 ); // x and y cordinate to into index
            terminal_putchar(x, y, ' ', 0);
        }
    }
}

void print(const char* str)
{
    size_t len = strlen(str);
    int i;
    for(i = 0; i < len; i++)
    {
        terminal_writechar(str[i], 15);
    }
}

char *itoa(int i)
{
    static char text[12];
    int loc = 11;
    text[11] = 0;
    char neg = 1;
    if (i >= 0)
    {
        neg = 0;
        i = -i;
    }

    while (i)
    {
        text[--loc] = '0' - (i % 10);
        i /= 10;
    }

    if (loc == 11)
    {
        text[--loc] = '0';
    }

    if (neg)
    {
        text[--loc] = '-';
    }

    return &text[loc];
}

int printf(const char *format, ...)
{
    va_list ap;
    const char *p;
    char *sval;
    int ival;

    va_start(ap, format);
    for (p = format; *p; p++)
    {
        if (*p != '%')
        {
            terminal_putchar(terminal_col, terminal_row, *p, 0);
            continue;
        }

        switch (*++p)
        {
        case 'i':
            ival = va_arg(ap, int);
            print(itoa(ival));
            break;

        case 's':
            sval = va_arg(ap, char *);
            print(sval);
            break;

        default:
            terminal_putchar(terminal_col, terminal_row, *p, 0);
            break;
        }
    }

    va_end(ap);
    return 0;
}

/*
* Debug I/O
*/

void dbg_putc(char c)
{
    outb(0xE9, c);
}

void dbg_puts(const char *str)
{
    while (*str)
    {
        dbg_putc(*str);
        str++;
    }
}
