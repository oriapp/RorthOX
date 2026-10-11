/*
 * Multi-cluster file test for scripts/tests/fat-chain.sh (only copied into the test image).
 *
 * pattern.asm puts 200 KiB of pattern words into .rodata, so fatchain.elf is several FAT clusters
 * long and the words come from every cluster after the first. The program checks every word
 * against the formula and prints "fatchain: PASS <words> words" or "fatchain: FAIL ...", then
 * "fatchain: done". No globals: the loader doesn't map .bss.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

#define FATCHAIN_WORDS 51200

extern const unsigned int fatchain_pattern[];
extern const unsigned int fatchain_pattern_end[];

// One line per print call: the shell runs alongside and may print between two calls.
static void print_line(const char *start, int value, const char *end)
{
    char line[80];
    strcpy(line, start);
    strcpy(line + strlen(line), itoa(value));
    strcpy(line + strlen(line), end);
    print(line);
}

int main(int argc, char **argv)
{
    int words = fatchain_pattern_end - fatchain_pattern;
    int bad = -1;
    if (words != FATCHAIN_WORDS)
    {
        print_line("fatchain: FAIL ", words, " pattern words\n");
    }
    else
    {
        for (int i = 0; i < words; i++)
        {
            if (fatchain_pattern[i] != (unsigned int)i * 2654435761u + 0x9E3779B9u)
            {
                bad = i;
                break;
            }
        }
        if (bad < 0)
        {
            print_line("fatchain: PASS ", words, " words\n");
        }
        else
        {
            print_line("fatchain: FAIL word ", bad, " differs\n");
        }
    }
    print("fatchain: done\n");
    return 0;
}
