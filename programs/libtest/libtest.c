/*
 * User library test program for scripts/tests/printc.sh. Only copied into that test's image.
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

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (strncmp(mode, "printc", 7) == 0)
    {
        return colours();
    }
    print("libtest: usage libtest.elf printc\n");
    return 1;
}
