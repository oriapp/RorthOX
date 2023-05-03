#include <stdio.h>
#include "../../kernel/kernel.h"

#define UNUSED(x) (void)(x)

#if defined(__is_libk)
#include "../../kernel/include/kernel/tty.h"
#endif

/// @brief Puts a char on the screen.
/// @param x 
/// @param y 
/// @param c 
/// @param colour 
void terminal_putchar(int x, int y, char c, char colour)
{
    video_mem[(y * VGA_WIDTH) + x] = terminal_make_char(c, colour);
}

// What i do with this?
int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	UNUSED(c);
	// termian(&c, sizeof(c));
#else
	// TODO: Implement stdio and the write system call.
#endif
	return ic;
}
