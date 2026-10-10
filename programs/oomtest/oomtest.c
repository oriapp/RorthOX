/*
 * Out-of-memory test for scripts/tests/heap-oom.sh and scripts/tests/open-oom.sh (only copied
 * into the test images).
 *
 * Leaves an exact number of free kernel heap blocks, then asks the kernel to start blank.elf
 * (syscall 7). Loading blank.elf allocates, in this order (in 4 KiB blocks, E = blocks of the
 * whole blank.elf file):
 *
 *   command node 1 (user malloc before the syscall), process 3, ELF object 1, parsed path 3
 *   (root 1, name 1, path part 1), file descriptors 4 (FAT descriptor 1, FAT item 1, copy of the
 *   directory entry 1, file descriptor 1; freed again by fclose), ELF file E, stack 4, task 1,
 *   page directory 1, 1024 page tables, paging chunk 1.
 *
 * So the number of free blocks picks which allocation is the first to fail. The kernel must
 * return an error instead of crashing, and must give back what the failed step allocated: the
 * test counts the free blocks again right after the syscall. The allocations made before the
 * failing step are known leaks and are expected to stay taken. The "open..." cases (5 to 11 free
 * blocks) make one allocation of the file open fail, so the syscall returns -EIO; only the
 * command node, the process and the finished parsed path may stay taken.
 *
 * The free blocks are counted straight from the kernel's heap table at RORTHOX_HEAP_TABLE_ADDRESS.
 * That only works because user programs can read all memory today; it is a test-only shortcut.
 *
 * Usage: oomtest.elf <case> <size of blank.elf in bytes>. The program prints
 * "oomtest <case>: ready" and waits for a key, so the test script can make sure the shell is idle
 * first. While the heap is nearly full nothing else must allocate, so the only line printed then
 * is "armed" (print borrows one block and gives it back). After "armed" it waits for a second
 * key before the syscall. Every run ends with "oomtest <case>: done"; the result is
 * "oomtest <case>: PASS" or "oomtest <case>: FAIL ..." lines.
 *
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

// say() leaves out a number that is NONE
#define NONE -1000000

// The block counts at the top only hold for this geometry.
_Static_assert(RORTHOX_HEAP_BLOCK_SIZE == 4096, "oomtest counts 4 KiB heap blocks");
_Static_assert(USER_PROGRAM_STACK_SIZE == 4 * RORTHOX_HEAP_BLOCK_SIZE, "oomtest expects a four-block user stack");
_Static_assert(sizeof(struct command_argument) <= RORTHOX_HEAP_BLOCK_SIZE, "oomtest expects one-block command nodes");

struct oom_case
{
    const char *name;
    // Free blocks before the syscall: free_base + free_per_e * E
    int free_base;
    int free_per_e;
    int result;
    // Blocks still taken after the syscall returns: kept_base + kept_per_e * E
    int kept_base;
    int kept_per_e;
};

// One line per print call: the shell runs alongside and may print between two calls.
static void say(const char *name, const char *text, int a, int b)
{
    char line[100];
    strcpy(line, "oomtest ");
    strncpy(line + strlen(line), name, 16);
    strcpy(line + strlen(line), ": ");
    strncpy(line + strlen(line), text, 40);
    if (a != NONE)
    {
        strcpy(line + strlen(line), itoa(a));
    }
    if (b != NONE)
    {
        strcpy(line + strlen(line), " expected ");
        strcpy(line + strlen(line), itoa(b));
    }
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

static int number(const char *s)
{
    int n = 0;
    if (!*s)
    {
        return -1;
    }
    for (; *s; s++)
    {
        if (*s < '0' || *s > '9' || n > 10000000)
        {
            return -1;
        }
        n = n * 10 + (*s - '0');
    }
    return n;
}

int main(int argc, char **argv)
{
    // The order the test script runs them in: the later page table first.
    const struct oom_case cases[] = {
        { "table",     100,  0, -EIO,    12, 1 },
        { "task",       12,  1, -ENOMEM, 12, 1 },
        { "directory",  13,  1, -EIO,    12, 1 },
        { "first",      14,  1, -EIO,    12, 1 },
        { "wrapper",  1038,  1, -EIO,    12, 1 },
        { "elfobject",   4,  0, -ENOMEM,  4, 0 },
        { "elfdata",    11,  1, -ENOMEM,  7, 0 },
        { "process",     3,  0, -ENOMEM,  1, 0 },
        // Opening blank.elf (scripts/tests/open-oom.sh): the first allocation to fail is
        { "openroot",    5,  0, -EIO,     4, 0 },  // the parsed path's root
        { "openpart",    6,  0, -EIO,     4, 0 },  // the first name
        { "opennode",    7,  0, -EIO,     4, 0 },  // the first path part
        { "openfat",     8,  0, -EIO,     7, 0 },  // the FAT descriptor
        { "openitem",    9,  0, -EIO,     7, 0 },  // the FAT item
        { "openclone",  10,  0, -EIO,     7, 0 },  // the copy of the directory entry
        { "openvfs",    11,  0, -EIO,     7, 0 },  // the file descriptor
    };
    const struct oom_case *c = 0;
    void *plugs[MAX_PLUGS];
    int plug_count = 0;
    void *filler = 0;
    int failed = 0;

    if (argc != 3)
    {
        print("oomtest: FAIL usage: oomtest.elf <case> <blank.elf size>\n");
        return 1;
    }
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        if (strlen(argv[1]) == strlen(cases[i].name) && strncmp(argv[1], cases[i].name, strlen(argv[1])) == 0)
        {
            c = &cases[i];
        }
    }
    int size = number(argv[2]);
    int e = (size + BLOCK - 1) / BLOCK;
    if (!c || size <= 0)
    {
        print("oomtest: FAIL unknown case or bad size\n");
        return 1;
    }
    // 86 - E page tables fit in the "table" case; it must stay a table in the middle.
    if (e < 1 || e > 85)
    {
        say(c->name, "FAIL blank.elf blocks out of range: ", e, NONE);
        say(c->name, "done", NONE, NONE);
        return 1;
    }
    int want_free = c->free_base + c->free_per_e * e;
    int want_kept = c->kept_base + c->kept_per_e * e;

    say(c->name, "ready", NONE, NONE);
    rorthox_getkeyblock();

    // Fill every free run but the last one exactly (first fit takes the lowest run that is big
    // enough, which is the lowest run), then fill the last run up to want_free blocks.
    int setup = 0;
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
    if (!setup && length < want_free)
    {
        setup = 3;
    }
    if (!setup && length > want_free)
    {
        filler = malloc((length - want_free) * BLOCK);
        if (filler != (void *)(RORTHOX_HEAP_ADDRESS + start * BLOCK))
        {
            setup = 4;
        }
    }
    // Now exactly one free run of want_free blocks must be left.
    int run_start = 0;
    int free_before = count_free();
    if (!setup && (free_before != want_free || next_run(&run_start) != want_free))
    {
        setup = 5;
    }

    // Say the setup is right before the syscall, so a crash still shows which case was armed.
    // print takes one scratch block from the start of the free run and gives it back, so the
    // run must be the same afterwards.
    if (!setup)
    {
        say(c->name, "armed with free blocks ", free_before, NONE);
        run_start = 0;
        if (count_free() != want_free || next_run(&run_start) != want_free)
        {
            setup = 6;
        }
        else
        {
            // Second key: the script has seen "armed", so it is on screen even if the kernel dies next.
            rorthox_getkeyblock();
        }
    }

    int result = 0;
    int free_after = free_before;
    if (!setup)
    {
        result = rorthox_system_run("blank.elf");
        free_after = count_free();
    }

    // Give the memory back before printing the results.
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
        say(c->name, "FAIL setup step ", setup, NONE);
        say(c->name, "FAIL setup free blocks ", free_before, want_free);
        say(c->name, "done", NONE, NONE);
        return 1;
    }
    say(c->name, "free blocks after the syscall ", free_after, NONE);
    if (result != c->result)
    {
        say(c->name, "FAIL result ", result, c->result);
        failed = 1;
    }
    if (free_before - free_after != want_kept)
    {
        say(c->name, "FAIL blocks kept ", free_before - free_after, want_kept);
        failed = 1;
    }
    if (!failed)
    {
        say(c->name, "PASS", NONE, NONE);
    }
    say(c->name, "done", NONE, NONE);
    return failed;
}
