/*
 * Terminal test program for scripts/tests/tty-backspace.sh and scripts/tests/tty-scroll.sh. Only
 * copied into those tests' images.
 *
 * ttytest.elf bs
 *     Prints "ttytest bs: armed" and waits for a key. Then, in one print: a newline, a row of
 *     exactly 80 characters ("ttytest-bs-" and digits, the last one 'Z'), a backspace and 'R'.
 *     The backspace at column 0 must go back to the last column of the full row, so 'R' replaces
 *     the 'Z'. Waits for a key, prints "ttytest bs: done" and ends.
 *
 * ttytest.elf scroll
 *     Prints "ttytest scroll: armed" and waits for a key. Phase 1: a newline, 64 numbered lines
 *     ("scroll line NN ..."), then "ttytest scroll: lines end" without a newline, and waits for a
 *     key. Phase 2: a newline, 40 numbered blocks of exactly 80 characters without newlines
 *     ("wrap block NN ..."), then "ttytest scroll: wrap end", and waits for a key. Then prints
 *     "ttytest scroll: done" and ends. The test script rebuilds the same lines.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

#define WIDTH 80

static void two_digits(char *out, int n)
{
    out[0] = '0' + n / 10 % 10;
    out[1] = '0' + n % 10;
}

// "scroll line NN " and letters up to 20 + (7 * n) % 50 characters, then a newline.
static void scroll_line(char *out, int n)
{
    int len = 20 + (7 * n) % 50;
    strcpy(out, "scroll line NN ");
    two_digits(out + 12, n);
    for (int i = strlen(out); i < len; i++)
    {
        out[i] = 'a' + (n + i) % 26;
    }
    out[len] = '\n';
    out[len + 1] = 0;
}

// "wrap block NN " and capitals up to exactly 80 characters, no newline.
static void wrap_block(char *out, int n)
{
    strcpy(out, "wrap block NN ");
    two_digits(out + 11, n);
    for (int i = strlen(out); i < WIDTH; i++)
    {
        out[i] = 'A' + (n + i) % 26;
    }
    out[WIDTH] = 0;
}

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

static int scroll(void)
{
    char line[WIDTH + 2];
    print("ttytest scroll: armed\n");
    rorthox_getkeyblock();

    print("\n");
    for (int n = 1; n <= 64; n++)
    {
        scroll_line(line, n);
        print(line);
    }
    print("ttytest scroll: lines end");
    rorthox_getkeyblock();

    print("\n");
    for (int n = 1; n <= 40; n++)
    {
        wrap_block(line, n);
        print(line);
    }
    print("ttytest scroll: wrap end");
    rorthox_getkeyblock();

    print("\nttytest scroll: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strncmp(mode, "bs", 3) == 0)
    {
        return backspace();
    }
    if (strncmp(mode, "scroll", 7) == 0)
    {
        return scroll();
    }
    print("ttytest: usage ttytest.elf bs|scroll\n");
    return 1;
}
