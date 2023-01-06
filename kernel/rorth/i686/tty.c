#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "string/string.h"

#define MAINFILE
#include "kernel.h"
#include "include/kernel/tty.h"


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
