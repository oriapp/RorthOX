/*
 * User library test program for scripts/tests/printc.sh and scripts/tests/strtok.sh. Only
 * copied into those tests' images.
 *
 * libtest.elf printc
 *     Prints "libtest printc: armed" and waits for a key. Then prints a newline (the shell's
 *     prompt can land after the armed line) and "lt-c:", calls printc with
 *     "#0" ... "#f" in colours 0 to 15 (each text names its colour in hex), calls printc with
 *     "XX" and the invalid colours 16, 255, 256, 0x80000000 and 0xffffffff, and prints "|end"
 *     and a newline. On the screen that is one row: "lt-c:#0#1...#f|end", every "#n" in colour
 *     n on black, the rest white on black, and no "X". Every printc call runs with marker
 *     values in ebx, esi, edi and ebp, which the wrapper must keep (cdecl). Then one line per
 *     printc result that is wrong ("libtest printc: FAIL ..."), "libtest printc: FAIL registers
 *     changed ..." or "libtest printc: ebx esi edi ebp kept", "libtest printc: results checked",
 *     and "libtest printc: done"; waits for a key and ends.
 *
 * libtest.elf strtok
 *     Runs strtok on a few strings, starting a new string while the last one isn't finished
 *     (that must start over), and checks where every token starts and what it reads. Prints
 *     "libtest strtok: start", a FAIL line for every wrong step and
 *     "libtest strtok: <failed> of <checks> checks failed", and ends.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "status.h"

// Registers that printc changed in any call so far: 1 ebx, 2 esi, 4 edi, 8 ebp.
static unsigned int registers_changed;

// printc(str, color), called with marker values in ebx, esi, edi and ebp, which a cdecl
// function must give back unchanged. Returns the int the syscall leaves in eax.
static int printc_checked(const char *str, unsigned int color)
{
    int result;
    unsigned int changed = color;
    __asm__ volatile("push %%ebp\n\t"
                     "push %%ebx\n\t"
                     "push %%esi\n\t"
                     "push %%edi\n\t"
                     "push %%ecx\n\t"
                     "push %%edx\n\t"
                     "mov $0x5eb15eb1, %%ebx\n\t"
                     "mov $0x5e515e51, %%esi\n\t"
                     "mov $0x5ed15ed1, %%edi\n\t"
                     "mov $0x5eb05eb0, %%ebp\n\t"
                     "call printc\n\t"
                     "add $8, %%esp\n\t"
                     "xor %%ecx, %%ecx\n\t"
                     "cmp $0x5eb15eb1, %%ebx\n\t"
                     "je 1f\n\t"
                     "or $1, %%ecx\n"
                     "1:\n\t"
                     "cmp $0x5e515e51, %%esi\n\t"
                     "je 2f\n\t"
                     "or $2, %%ecx\n"
                     "2:\n\t"
                     "cmp $0x5ed15ed1, %%edi\n\t"
                     "je 3f\n\t"
                     "or $4, %%ecx\n"
                     "3:\n\t"
                     "cmp $0x5eb05eb0, %%ebp\n\t"
                     "je 4f\n\t"
                     "or $8, %%ecx\n"
                     "4:\n\t"
                     "pop %%edi\n\t"
                     "pop %%esi\n\t"
                     "pop %%ebx\n\t"
                     "pop %%ebp"
                     : "=a"(result), "+c"(changed), "+d"(str)
                     :
                     : "memory", "cc");
    registers_changed |= changed;
    return result;
}

static void fail(const char *mode, const char *what, unsigned int value, int got, int expected)
{
    char line[120];
    strcpy(line, "libtest ");
    strcpy(line + strlen(line), mode);
    strcpy(line + strlen(line), ": FAIL ");
    strcpy(line + strlen(line), what);
    strcpy(line + strlen(line), " ");
    strcpy(line + strlen(line), itoa((int)value));
    strcpy(line + strlen(line), " got ");
    strcpy(line + strlen(line), itoa(got));
    strcpy(line + strlen(line), " expected ");
    strcpy(line + strlen(line), itoa(expected));
    strcpy(line + strlen(line), "\n");
    print(line);
}

static int colours(void)
{
    static const unsigned int bad[] = {16, 255, 256, 0x80000000u, 0xffffffffu};
    int valid[16];
    int invalid[5];
    int failed = 0;

    print("libtest printc: armed\n");
    rorthox_getkeyblock();

    // Start the colour row on a line of its own, even if the shell's "> " came after "armed".
    print("\nlt-c:");
    for (int c = 0; c < 16; c++)
    {
        char text[3];
        text[0] = '#';
        text[1] = c < 10 ? '0' + c : 'a' + c - 10;
        text[2] = 0;
        valid[c] = printc_checked(text, c);
    }
    for (int i = 0; i < 5; i++)
    {
        invalid[i] = printc_checked("XX", bad[i]);
    }
    print("|end\n");

    for (int c = 0; c < 16; c++)
    {
        if (valid[c] != 0)
        {
            fail("printc", "colour", c, valid[c], 0);
            failed = 1;
        }
    }
    for (int i = 0; i < 5; i++)
    {
        if (invalid[i] != -EINVARG)
        {
            fail("printc", "colour", bad[i], invalid[i], -EINVARG);
            failed = 1;
        }
    }
    if (registers_changed)
    {
        fail("printc", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    else
    {
        print("libtest printc: ebx esi edi ebp kept\n");
    }
    print("libtest printc: results checked\n");
    print("libtest printc: done\n");
    rorthox_getkeyblock();
    return failed;
}

static int token_checks;
static int token_failed;

// One strtok call: the token must start at want (0: no token) and read want_text.
static void token(int step, char *got, char *want, const char *want_text)
{
    token_checks++;
    if (got == want && (!want || strncmp(got, want_text, 32) == 0))
    {
        return;
    }
    token_failed++;
    // got: the token's distance from want, -1 for no token, 1000 for a token where none was expected
    fail("strtok", "step", step, !got ? -1 : want ? (int)(got - want) : 1000, 0);
}

static int tokens(void)
{
    char old[] = "old tail";
    char words[] = "new words";
    char single[] = "single";
    char next[] = "next one";
    char empty[] = "";
    char after_empty[] = "x y";
    char unfinished[] = "aa bb";
    char spaced[] = "  lead  and   repeat ";
    char mixed[] = ",a,;b;;c;";
    char again[] = "again";

    print("libtest strtok: start\n");
    // A new string while the last one isn't finished starts over.
    token(1, strtok(old, " "), old, "old");
    token(2, strtok(words, " "), words, "new");
    token(3, strtok(0, " "), words + 4, "words");
    token(4, strtok(0, " "), 0, 0);
    // The same after a string with one token.
    token(5, strtok(single, " "), single, "single");
    token(6, strtok(next, " "), next, "next");
    token(7, strtok(0, " "), next + 5, "one");
    token(8, strtok(0, " "), 0, 0);
    // Empty input, then a new string.
    token(9, strtok(empty, " "), 0, 0);
    token(10, strtok(0, " "), 0, 0);
    token(11, strtok(after_empty, " "), after_empty, "x");
    token(12, strtok(0, " "), after_empty + 2, "y");
    token(13, strtok(0, " "), 0, 0);
    // An empty string while another one isn't finished.
    token(14, strtok(unfinished, " "), unfinished, "aa");
    token(15, strtok(empty, " "), 0, 0);
    token(16, strtok(0, " "), 0, 0);
    // Leading, repeated and trailing delimiters, two delimiter characters.
    token(17, strtok(spaced, " "), spaced + 2, "lead");
    token(18, strtok(0, " "), spaced + 8, "and");
    token(19, strtok(0, " "), spaced + 14, "repeat");
    token(20, strtok(0, " "), 0, 0);
    token(21, strtok(mixed, ",;"), mixed + 1, "a");
    token(22, strtok(0, ",;"), mixed + 4, "b");
    token(23, strtok(0, ",;"), mixed + 7, "c");
    token(24, strtok(0, ",;"), 0, 0);
    // After the end, more calls give nothing until a new string comes.
    token(25, strtok(0, ",;"), 0, 0);
    token(26, strtok(again, " "), again, "again");
    token(27, strtok(0, " "), 0, 0);

    char line[80];
    strcpy(line, "libtest strtok: ");
    strcpy(line + strlen(line), itoa(token_failed));
    strcpy(line + strlen(line), " of ");
    strcpy(line + strlen(line), itoa(token_checks));
    strcpy(line + strlen(line), " checks failed\n");
    print(line);
    return token_failed != 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strncmp(mode, "printc", 7) == 0)
    {
        return colours();
    }
    if (strncmp(mode, "strtok", 7) == 0)
    {
        return tokens();
    }
    print("libtest: usage libtest.elf printc|strtok\n");
    return 1;
}
