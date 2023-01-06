#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <stdio.h>
#include "kernel.h"

#define EOF (-1)

static uint16_t *video_mem = 0;
static uint16_t terminal_row = 0;
static uint16_t terminal_col = 0;

#ifdef __cplusplus
extern "C" {
#endif

uint16_t terminal_make_char(char c, char colour);
void terminal_putchar(int x, int y, char c, char colour);
void terminal_backspace();
void terminal_writechar(char c, char colour);
void terminal_initialize();
void print(const char* str);

// int printf(const char* __restrict, ...);
// int putchar(int);
// int puts(const char*);

#ifdef __cplusplus
}
#endif

#endif
