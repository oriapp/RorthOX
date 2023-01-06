#include <stdio.h>
#include "include/stdio.h"
#include "kernel.h"

#if defined(__is_libk)
#include "../../kernel/include/kernel/tty.h"
#endif

void terminal_putchar(int x, int y, char c, char colour)
{
    video_mem[(y * VGA_WIDTH) + x] = terminal_make_char(c, colour);
}

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	terminal_write(&c, sizeof(c));
#else
	// TODO: Implement stdio and the write system call.
#endif
	return ic;
}
