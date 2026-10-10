/*
 * Heap test for scripts/tests/heap.sh (only copied into the test image).
 *
 * Asks for more memory than the free run at the end of the kernel heap holds. The kernel
 * must refuse, and a small allocation must still work afterwards.
 *
 * Before the big request it leaves one free block between the used blocks and a "fence"
 * block, so the kernel's print syscall still finds a scratch block even if the big request
 * wrongly takes every block up to the end of the heap table.
 *
 * Every run ends with "heaptest: done"; the result is "heaptest: PASS" or "heaptest: FAIL ...".
 */
#include "rorthox.h"
#include "stdlib.h"
#include "stdio.h"
#include "config.h"

#define BLOCK RORTHOX_HEAP_BLOCK_SIZE
// The whole heap less 1024 blocks: 96 MiB with the 100 MiB heap. More than is free once the
// kernel, the shell and this program are loaded, and still inside 128 MiB of RAM if a broken
// allocator hands it out anyway.
#define BIG_REQUEST (RORTHOX_HEAP_SIZE_BYTES - 1024U * RORTHOX_HEAP_BLOCK_SIZE)

// One print call per line: the shell runs alongside and may print between two calls.
static int fail(const char *line)
{
    print(line);
    print("heaptest: done\n");
    return 1;
}

int main(int argc, char **argv)
{
    char *hole = malloc(BLOCK);
    char *fence = malloc(BLOCK);
    if (!hole || !fence || fence != hole + BLOCK)
    {
        // The heap layout is not what the test needs, so print without the kernel's
        // print syscall (it allocates): printf with a plain string writes one char at a time.
        printf("heaptest: FAIL setup allocations\nheaptest: done\n");
        return 1;
    }
    free(hole);

    print("heaptest: requesting more than is free\n");
    void *big = malloc(BIG_REQUEST);
    if (big)
    {
        // Don't touch or free it: the kernel gave out blocks past the end of its heap table.
        return fail("heaptest: FAIL oversized request accepted\n");
    }
    print("heaptest: refused\n");

    // Two blocks don't fit in the hole, so this comes from the free run after the fence.
    volatile unsigned char *small = malloc(2 * BLOCK);
    if (!small)
    {
        return fail("heaptest: FAIL small allocation after refusal\n");
    }
    // First fit: the first free run of two blocks starts right after the fence.
    if (small != (volatile unsigned char *)fence + BLOCK)
    {
        return fail("heaptest: FAIL small allocation not right after the fence\n");
    }

    small[0] = 0x11;
    small[BLOCK - 1] = 0x22;
    small[BLOCK] = 0x33;
    small[2 * BLOCK - 1] = 0x44;
    if (small[0] != 0x11 || small[BLOCK - 1] != 0x22 || small[BLOCK] != 0x33 || small[2 * BLOCK - 1] != 0x44)
    {
        return fail("heaptest: FAIL small allocation readback\n");
    }

    free((void *)small);
    free(fence);
    print("heaptest: small allocation usable\n");
    print("heaptest: PASS\n");
    print("heaptest: done\n");
    return 0;
}
