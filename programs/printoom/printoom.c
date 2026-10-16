/*
 * Print under heap pressure, for scripts/tests/print-oom.sh (only copied into the test image).
 *
 * The print and printc syscalls copy the user's string into a buffer on the kernel stack through a
 * checked copy that needs no kernel heap. So with the heap full they must still work: print the
 * text and return 0, and never print whatever a kernel buffer held before.
 *
 * The program leaves exactly one free heap block and prints a line that ends up in the kernel's
 * print buffer ("printoom: last good line"). Then it takes that block too, checks from the heap
 * table that no block is free, and calls print and printc with other text. Both texts must show,
 * and no second copy of the last good line. Then it frees everything and prints the
 * syscall results. (Before the checked copy, the copy borrowed a heap block and failed here.)
 *
 * The free blocks are counted straight from the kernel's heap table at RORTHOX_HEAP_TABLE_ADDRESS.
 * That only works because user programs can read all memory today; it is a test-only shortcut.
 *
 * It prints "printoom: ready" and waits for a key, so the test script can make sure the shell is
 * idle first. Ends with "printoom: PASS" or "printoom: FAIL ..." lines, then "printoom: done".
 * No globals: the loader doesn't map .bss.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "config.h"
#include "status.h"

#define BLOCK RORTHOX_HEAP_BLOCK_SIZE
#define TOTAL_BLOCKS (RORTHOX_HEAP_SIZE_BYTES / RORTHOX_HEAP_BLOCK_SIZE)
#define TABLE ((const volatile unsigned char *)RORTHOX_HEAP_TABLE_ADDRESS)
#define MAX_PLUGS 64

// The same wrappers as print and printc, declared with the int the syscall leaves in eax.
int print_result(const char *message) __asm__("print");
int printc_result(const char *str, size_t color) __asm__("printc");

static void say(const char *text, int a, int b)
{
    char line[100];
    strcpy(line, "printoom: ");
    strncpy(line + strlen(line), text, 60);
    strcpy(line + strlen(line), itoa(a));
    strcpy(line + strlen(line), " expected ");
    strcpy(line + strlen(line), itoa(b));
    strcpy(line + strlen(line), "\n");
    print(line);
}

static int count_free(void)
{
    int n = 0;
    for (int i = 0; i < TOTAL_BLOCKS; i++)
    {
        if (TABLE[i] == 0)
        {
            n++;
        }
    }
    return n;
}

// Find the free run starting at or after block *start. Returns its length, 0 if there is none.
static int next_run(int *start)
{
    int i = *start;
    while (i < TOTAL_BLOCKS && TABLE[i] != 0)
    {
        i++;
    }
    int j = i;
    while (j < TOTAL_BLOCKS && TABLE[j] == 0)
    {
        j++;
    }
    *start = i;
    return j - i;
}

int main(int argc, char **argv)
{
    void *plugs[MAX_PLUGS];
    int plug_count = 0;
    void *filler = 0;
    void *last = 0;
    int setup = 0;

    print("printoom: ready\n");
    rorthox_getkeyblock();

    // Fill every free run but the last one exactly (first fit takes the lowest run that is big
    // enough), then fill the last run up to one block.
    int start = 0;
    int length = next_run(&start);
    while (length > 0)
    {
        int after = start + length;
        int next_length = next_run(&after);
        if (next_length == 0)
        {
            break;
        }
        if (plug_count == MAX_PLUGS)
        {
            setup = 1;
            break;
        }
        plugs[plug_count] = malloc(length * BLOCK);
        if (plugs[plug_count] != (void *)(RORTHOX_HEAP_ADDRESS + start * BLOCK))
        {
            plug_count++;
            setup = 2;
            break;
        }
        plug_count++;
        start = after;
        length = next_length;
    }
    if (!setup && length < 1)
    {
        setup = 3;
    }
    if (!setup && length > 1)
    {
        filler = malloc((length - 1) * BLOCK);
        if (filler != (void *)(RORTHOX_HEAP_ADDRESS + start * BLOCK))
        {
            setup = 4;
        }
    }
    if (!setup && count_free() != 1)
    {
        setup = 5;
    }

    int print_res = 1;
    int printc_res = 1;
    int free_blocks = -1;
    if (!setup)
    {
        // Borrows the last block and gives it back; the text stays in the kernel's buffer.
        print("printoom: last good line\n");
        last = malloc(BLOCK);
        free_blocks = count_free();
        if (!last || free_blocks != 0)
        {
            setup = 6;
        }
        else
        {
            print_res = print_result("printoom: print got through\n");
            printc_res = printc_result("printoom: printc got through\n", 15);
        }
    }

    // Give the memory back before printing the results.
    if (last)
    {
        free(last);
    }
    if (filler)
    {
        free(filler);
    }
    for (int i = 0; i < plug_count; i++)
    {
        free(plugs[i]);
    }

    if (setup)
    {
        say("FAIL setup step ", setup, 0);
        say("FAIL free blocks ", free_blocks, 0);
        print("printoom: done\n");
        return 1;
    }
    int failed = 0;
    if (print_res != 0)
    {
        say("FAIL print result ", print_res, 0);
        failed = 1;
    }
    if (printc_res != 0)
    {
        say("FAIL printc result ", printc_res, 0);
        failed = 1;
    }
    if (!failed)
    {
        print("printoom: PASS\n");
    }
    print("printoom: done\n");
    return failed;
}
