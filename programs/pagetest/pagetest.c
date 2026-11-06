/*
 * User page test for scripts/tests/user-pages.sh (only copied into its test image, never into
 * bin/os.bin).
 *
 * malloc() hands out whole heap blocks. Every byte of them must be zero when the program gets
 * them, not only the bytes it asked for. The test fills a block with 0xA5, frees it and asks for
 * a smaller size: first fit gives back the same block (checked, so a different block can't pass
 * silently), and the whole block must read as zero. Then the same for three blocks and a request
 * of two blocks and 5 bytes. Syscalls still own only the requested bytes, not the padding, and
 * sizes of 0, sizes that can't be rounded up and sizes bigger than the heap still give NULL
 * without keeping a block.
 *
 * One print call per line; the run ends with "pagetest: PASS" or "pagetest: FAIL <what>", then
 * "pagetest: done".
 */
#include <stdint.h>
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "config.h"

#define BLOCK RORTHOX_HEAP_BLOCK_SIZE

static char line[128];

// "pagetest: <what> <value>\n" in one print call.
static void say(const char *what, int value)
{
    strcpy(line, "pagetest: ");
    strcpy(line + strlen(line), what);
    strcpy(line + strlen(line), " ");
    strcpy(line + strlen(line), itoa(value));
    strcpy(line + strlen(line), "\n");
    print(line);
}

static int fail(const char *what, int value)
{
    static char failure[96];
    strcpy(failure, "FAIL ");
    strcpy(failure + strlen(failure), what);
    say(failure, value);
    print("pagetest: done\n");
    return 1;
}

// Fill <blocks> blocks with 0xA5, free them, then ask for <request> bytes. Returns the new
// allocation, or NULL after printing the failure.
static unsigned char *reuse(int blocks, unsigned int request, const char *name)
{
    volatile unsigned char *old = malloc(blocks * BLOCK);
    if (!old)
    {
        fail("setup allocation", blocks);
        return 0;
    }
    for (int i = 0; i < blocks * BLOCK; i++)
    {
        old[i] = 0xA5;
    }
    free((void *)old);

    volatile unsigned char *now = malloc(request);
    if (!now)
    {
        fail("request refused", (int)request);
        return 0;
    }
    if (now != old)
    {
        // Without the same block the zero check below would prove nothing.
        fail("freed block not reused", (int)request);
        return 0;
    }
    for (int i = 0; i < blocks * BLOCK; i++)
    {
        if (now[i])
        {
            fail("stale byte at offset", i);
            return 0;
        }
    }
    say(name, (int)request);
    return (unsigned char *)now;
}

// The last requested byte is owned, the byte after it (page padding) is not.
static int exact(int fd, unsigned char *ptr, unsigned int request)
{
    if (rorthox_read(fd, ptr + request - 1, 2) != -EINVARG)
    {
        return fail("padding accepted by read, size", (int)request);
    }
    if (rorthox_read(fd, ptr + request - 1, 1) != 1)
    {
        return fail("last byte refused by read, size", (int)request);
    }
    say("exact ownership", (int)request);
    return 0;
}

int main(int argc, char **argv)
{
    int fd = rorthox_open("0:/shell.elf", RORTHOX_FILE_READ);
    if (fd < 0)
    {
        return fail("open shell.elf", fd);
    }

    unsigned char *one = reuse(1, 1, "one block zero");
    if (!one || exact(fd, one, 1))
    {
        return 1;
    }
    free(one);

    unsigned int request = 2 * BLOCK + 5;
    unsigned char *three = reuse(3, request, "three blocks zero");
    if (!three || exact(fd, three, request))
    {
        return 1;
    }
    free(three);

    // The first free block: a refused request that kept a block would move the next malloc.
    void *first = malloc(1);
    free(first);
    static const unsigned int refused[] = {0, 0xFFFFF001U, 0xFFFFFFFFU, RORTHOX_HEAP_SIZE_BYTES};
    for (int i = 0; i < 4; i++)
    {
        if (malloc(refused[i]))
        {
            return fail("request accepted, case", i);
        }
    }
    void *again = malloc(1);
    if (!first || again != first)
    {
        return fail("refused request kept a block", 0);
    }
    free(again);
    say("refused sizes", 4);

    rorthox_close(fd);
    print("pagetest: PASS\n");
    print("pagetest: done\n");
    return 0;
}
