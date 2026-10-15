/*
 * ELF segment test for scripts/tests/elf-segments.sh. Only copied into that test's image.
 *
 * Has real initialised data (.data) and a .bss array over several pages with an odd-sized tail.
 * Checks that the data has its initial values and that every .bss byte is zero, then writes
 * every byte of both and reads it back. Run it twice: the second run must see fresh initial
 * values and zeroes again.
 *
 * Usage: elftest.elf <tag>. Prints a "elftest <tag>: FAIL ..." line for each check that fails,
 * then "elftest <tag>: PASS data bss write" or "elftest <tag>: FAILED". Few lines, so the whole
 * test fits on the screen.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

#define BSS_SIZE (3 * 4096 + 123)

// .data: initial values that are not zero and not a simple pattern.
volatile unsigned int data_words[8] = { 0x13579bdf, 0x2468ace0, 0xdeadbeef, 0x01020304,
                                        0xfedcba98, 0x0badf00d, 0x55aa55aa, 0x80000001 };
volatile char data_text[] = "elftest initial data";

// .bss
volatile unsigned char bss_bytes[BSS_SIZE];
volatile unsigned int bss_tail;

static const unsigned int want_words[8] = { 0x13579bdf, 0x2468ace0, 0xdeadbeef, 0x01020304,
                                            0xfedcba98, 0x0badf00d, 0x55aa55aa, 0x80000001 };

// One whole line per print call: the shell runs alongside and may print between two calls.
static void say(const char *tag, const char *text, int number)
{
    char line[100];
    strcpy(line, "elftest ");
    strncpy(line + strlen(line), tag, 16);
    strcpy(line + strlen(line), ": ");
    strncpy(line + strlen(line), text, 50);
    if (number >= 0)
    {
        strcpy(line + strlen(line), itoa(number));
    }
    strcpy(line + strlen(line), "\n");
    print(line);
}

static unsigned char pattern(int i)
{
    return (unsigned char)(i * 31 + 7) | 1;
}

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "run";
    const char *text = "elftest initial data";
    int failed = 0;

    int bad = -1;
    for (int i = 0; i < 8; i++)
    {
        if (data_words[i] != want_words[i])
        {
            bad = i;
            break;
        }
    }
    for (int i = 0; bad < 0 && i <= strlen(text); i++)
    {
        if (data_text[i] != text[i])
        {
            bad = 100 + i;
        }
    }
    if (bad >= 0)
    {
        say(tag, "FAIL data differs at item ", bad);
        failed = 1;
    }

    bad = -1;
    for (int i = 0; i < BSS_SIZE; i++)
    {
        if (bss_bytes[i] != 0)
        {
            bad = i;
            break;
        }
    }
    if (bad < 0 && bss_tail != 0)
    {
        bad = BSS_SIZE;
    }
    if (bad >= 0)
    {
        say(tag, "FAIL bss not zero at byte ", bad);
        failed = 1;
    }

    for (int i = 0; i < BSS_SIZE; i++)
    {
        bss_bytes[i] = pattern(i);
    }
    bss_tail = 0xa5a5a5a5;
    for (int i = 0; i < 8; i++)
    {
        data_words[i] = ~want_words[i];
    }
    bad = -1;
    for (int i = 0; i < BSS_SIZE; i++)
    {
        if (bss_bytes[i] != pattern(i))
        {
            bad = i;
            break;
        }
    }
    for (int i = 0; bad < 0 && i < 8; i++)
    {
        if (data_words[i] != ~want_words[i])
        {
            bad = 100000 + i;
        }
    }
    if (bad < 0 && bss_tail != 0xa5a5a5a5)
    {
        bad = BSS_SIZE;
    }
    if (bad >= 0)
    {
        say(tag, "FAIL write did not read back at ", bad);
        failed = 1;
    }

    say(tag, failed ? "FAILED" : "PASS data bss write", -1);
    return failed;
}
