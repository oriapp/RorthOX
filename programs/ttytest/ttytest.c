/*
 * Terminal test program for scripts/tests/tty-backspace.sh. Only copied into that test's image.
 *
 * ttytest.elf bs
 *     Prints "ttytest bs: armed" and waits for a key. Then, in one print: a newline, a row of
 *     exactly 80 characters ("ttytest-bs-" and digits, the last one 'Z'), a backspace and 'R'.
 *     The backspace at column 0 must go back to the last column of the full row, so 'R' replaces
 *     the 'Z'. Waits for a key, prints "ttytest bs: done" and ends.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

#define WIDTH 80

static int backspace(void)
{
    char buf[WIDTH + 4];
    print("ttytest bs: armed\n");
    rorthox_getkeyblock();

    // One print, so nothing else can land between the row and the backspace.
    const char *start = "ttytest-bs-";
    buf[0] = '\n';
    for (int i = 0; i < WIDTH; i++)
    {
        buf[1 + i] = i < strlen(start) ? start[i] : '0' + i % 10;
    }
    buf[WIDTH] = 'Z';
    buf[WIDTH + 1] = 0x08;
    buf[WIDTH + 2] = 'R';
    buf[WIDTH + 3] = 0;
    print(buf);
    rorthox_getkeyblock();

    print("ttytest bs: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strncmp(mode, "bs", 3) == 0)
    {
        return backspace();
    }
    print("ttytest: usage ttytest.elf bs\n");
    return 1;
}
