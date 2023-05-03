#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "config.h"

#include "string/string.h"
#include "kernel.h"
#include "io/io.h"
#include "string/string.h"

#include "include/kernel/tty.h"
#include "include/kernel/vga.h"

int terminal_count = 0;
static int cursor_pos;

/// @brief Puts a char into video memory
/// @param x pos
/// @param y pos
/// @param c character
/// @param colour VGA color
void terminal_putchar(int x, int y, char c, char colour)
{
    const size_t index = (y * VGA_WIDTH) + x;
    video_mem[index] = vga_entry(c, vga_entry_color(colour, termianl_background_color));
    // Whats the math for background color and text color?
    update_cursor(x, y);
}

/// @brief Simulates delete key
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
    terminal_writechar(' ', terminal_color); // terminal_color?
    terminal_col--;
    update_cursor(terminal_col, terminal_row);
}

/// @brief Formats given char and puts into screen
/// @param c
/// @param colour
void terminal_writechar(char c, char colour)
{
    if (c == '\n')
    {
        terminal_row++;
        terminal_col = 0;
        update_cursor(terminal_col, terminal_row);
        return;
    }

    if (c == 0x08)
    {
        terminal_backspace();
        return;
    }

    terminal_putchar(terminal_col, terminal_row, c, colour);
    terminal_col++;
    if (terminal_col >= VGA_WIDTH)
    {
        terminal_col = 0;
        terminal_row++;
    }
    update_cursor(terminal_col, terminal_row);
}

/// @brief Enables cursor
/// @param cursor_start  pos
/// @param cursor_end pos
void enable_cursor(uint8_t cursor_start, uint8_t cursor_end)
{
    outb(0x3D4, 0x0A);
    outb(0x3D5, (insb(0x3D5) & 0xC0) | cursor_start);

    outb(0x3D4, 0x0B);
    outb(0x3D5, (insb(0x3D5) & 0xE0) | cursor_end);
}

/// @brief moves cursor to coordinates
/// @param x pos
/// @param y pos
void update_cursor(uint8_t x, uint8_t y)
{
    const size_t pos = y * VGA_WIDTH + x;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

#define VGA_CTRL_PORT 0x3D4
#define VGA_CTRL_DISABLE_CURSOR 0x0A
#define VGA_DATA_PORT 0x3D5

// void disable_cursor(void) {
//     uint8_t cursor_start, cursor_end;

//     // Read current cursor start and end positions
//     outb(VGA_CTRL_PORT, 0x0B);
//     cursor_start = insb(VGA_DATA_PORT);
//     outb(VGA_CTRL_PORT, 0x0C);
//     cursor_end = insb(VGA_DATA_PORT);

//     // Disable cursor by setting both start and end positions to 0
//     outb(VGA_CTRL_PORT, VGA_CTRL_DISABLE_CURSOR);
//     outb(VGA_DATA_PORT, cursor_start & 0xE0);
//     outb(VGA_CTRL_PORT, VGA_CTRL_DISABLE_CURSOR);
//     outb(VGA_DATA_PORT, cursor_end & 0x1F);
// }

void console_sync_cursor(void)
{
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(cursor_pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((cursor_pos >> 8) & 0xFF));
}

/// @brief Initilize values and screen, also deletes BIOS info and etc
void terminal_background_color(vga_color_t colour);

/// @brief call on system panic, shutdown!
/// @param bg_colour 
void terminal_panic(vga_color_t bg_colour)
{
    // disable_cursor();
    terminal_initialize(bg_colour);
}

void terminal_initialize(vga_color_t bg_colour)
{
    // Clear the BIOS stuff
    video_mem = (uint16_t *)(0xB8000);
    int x, y;
    terminal_row = 0;
    terminal_col = 0;
    termianl_background_color = bg_colour;
    terminal_color = vga_entry_color(15, termianl_background_color);

    for (y = 0; y < VGA_HEIGHT; y++)
    {
        for (x = 0; x < VGA_WIDTH; x++)
        {
            const size_t index = y * VGA_WIDTH + x; // x and y cordinate to into index
            video_mem[index] = vga_entry(0x00, terminal_color);

            char copyright[] = "All rights reserved | R C N";
            if ((x < sizeof(copyright) / sizeof(char)) && y == 0)
            {
                (video_mem[index] = vga_entry(copyright[x], terminal_color));
                terminal_col++;
            }
            else if ((x <= sizeof(copyright) / sizeof(char)) && y == 0)
            {
                terminal_col = 0;
                terminal_row++;
                cursor_pos++;
            }
        }
    }

    // if (!terminal_init)
    // {
    //     // print("At First\n");
    //     enable_cursor(0, 0);
    // } else {
    //     // Fix printc
    //     // print("At second!\n");
    //     console_sync_cursor();
    // }

    terminal_count++; // 0 = First init

    // console_sync_cursor();
    // update_cursor(0, 0);
}

/// @brief prints a string to the screen
/// @param str string
void print(const char *str)
{
#if IODBG
    dbg_puts("\33[31m");
    dbg_puts("DBG I/O: ");
    dbg_puts("\033[0m");
#endif
    size_t len = strlen(str);
    int i;
    for (i = 0; i < len; i++)
    {
#if IODBG
        dbg_putc(str[i]);
#endif
        terminal_writechar(str[i], 15);
    }
#if IODBG
    dbg_puts("\n");
#endif
}

/// @brief prints in a specific color
/// @param str
/// @param color
void printc(const char *str, unsigned int color)
{
    size_t len = strlen(str);
    int i;
    for (i = 0; i < len; i++)
    {
        terminal_writechar(str[i], color);
    }
}

/// @brief call when using video memory directly, avoid overflowing a row
void checkIndent()
{
    if (terminal_col >= VGA_WIDTH)
    {
        terminal_col = 0;
        terminal_row++;
    }
}

int vprintf(const char *format, va_list args);

int printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vprintf(format, args);
    va_end(args);
    return count;
}

void print_bool(int b)
{
    if (b)
    {
        print("True");
    }
    else
    {
        print("False");
    }
}

void print_bin(unsigned int x)
{
    print("0b");
    char buf[33];
    buf[32] = '\0';
    for (int i = 31; i >= 0; i--)
    {
        buf[i] = (x & 1) + '0';
        x >>= 1;
    }
    print(buf);
}

void print_hex(unsigned int x)
{
    print("0x");
    char buf[9];
    buf[8] = '\0';
    int i = 0;
    for (i = 7; i >= 0; i--)
    {
        buf[i] = "0123456789ABCDEF"[x & 0xF];
        // x >> 4;
    }
    print(buf);
}

// void print_hex(int num) {
//     char hex[16];
//     int i = 0;
//     while (num > 0) {
//         int rem = num % 16;
//         if (rem < 10) {
//             hex[i] = rem + 48;
//         } else {
//             hex[i] = rem + 55;
//         }
//         i++;
//         num /= 16;
//     }
//     hex[i] = '\0';
//     strrev(hex); // Reverse the order of characters in the string
//     int len = strlen(hex);
//     for (int j = 0; j < 4 - len; j++) {
//         hex[len+j] = '0';
//     }
//     strrev(hex); // Reverse the order of characters in the string again
//     hex[4] = '\0'; // Make sure the string is null-terminated
//     hex[0] = '0';
//     hex[1] = 'x';
// }

int vprintf(const char *format, va_list args)
{
    int count = 0;
    for (const char *p = format; *p; p++)
    {
        if (*p != '%')
        {
            if (*p == '\n')
            {
                terminal_row++;
                terminal_col = 0;
            }
            else
            {
                terminal_putchar(terminal_col, terminal_row, *p, 15);
                terminal_col++;
                checkIndent();
                count++;
            }
            continue;
        }

        switch (*++p)
        {
        case 'i': // Integer to ASCII
            int ival = va_arg(args, int);
            print(itoa(ival));
            count += strlen(itoa(ival));
            break;

        case 's': // String
            char *sval = va_arg(args, char *);
            print(sval);
            count += strlen(sval);
            break;
        case 'b': // Boolean value
            int bval = va_arg(args, int);
            print_bool(bval);
            count += strlen(bval ? "true" : "false");
            break;
        case 'x': // Hexadecimal value
            // unsigned int xval = va_arg(args, unsigned int);
            // char hexstr[100];
            // itoa_hex(xval, hexstr, 16);
            // print(hexstr);
            // count += strlen(hexstr);
            unsigned int xval = va_arg(args, unsigned int);
            print_hex(xval);
            char x[100];
            count += strlen(itoa_hex(xval, x, 16));
            break;
        case 'B': // Binary value
            unsigned int bival = va_arg(args, unsigned int);
            print_bin(bival);
            count += 32;
            break;
        // case 'n': // New line
        //     terminal_row++;
        //     terminal_col = 0;
        //     break;
        default:
            terminal_putchar(terminal_col, terminal_row, *p, 15);
            terminal_col++;
            checkIndent();
            count++;
            break;
        }
    }
    return count;
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
