#include "heap_snapshot.h"
#include "memory.h"

// Page-aligned in .bss: the program's own memory, so taking a copy changes nothing in the heap.
static unsigned char heap_copy[HEAP_SNAPSHOT_BLOCKS] __attribute__((aligned(4096)));
static volatile unsigned int heap_ack;
static unsigned int heap_sequence;

// The debugger breaks here. It must stay a real call with these arguments on the stack.
__attribute__((noinline, noipa)) void heap_snapshot_wait(unsigned char *buffer, volatile unsigned int *ack,
    unsigned int sequence, unsigned int size, void *wrapper)
{
    __asm__ volatile("" ::: "memory");
}

__attribute__((noinline, noipa)) void heap_snapshot_end(unsigned int requests)
{
    __asm__ volatile("" ::: "memory");
}

int heap_snapshot_before(void *wrapper)
{
    // Not a valid table entry pattern for "free": a copy the debugger didn't write shows no free block.
    memset(heap_copy, 0xee, sizeof(heap_copy));
    heap_ack = 0;
    heap_sequence++;
    heap_snapshot_wait(heap_copy, &heap_ack, heap_sequence, sizeof(heap_copy), wrapper);
    return heap_ack == heap_sequence ? 0 : -1;
}

int heap_snapshot(void)
{
    return heap_snapshot_before(0);
}

void heap_snapshot_done(void)
{
    heap_snapshot_end(heap_sequence);
}

int heap_snapshot_free(void)
{
    int n = 0;
    for (int i = 0; i < HEAP_SNAPSHOT_BLOCKS; i++)
    {
        if (heap_copy[i] == 0)
        {
            n++;
        }
    }
    return n;
}

int heap_snapshot_next_run(int *start)
{
    int i = *start;
    while (i < HEAP_SNAPSHOT_BLOCKS && heap_copy[i] != 0)
    {
        i++;
    }
    int j = i;
    while (j < HEAP_SNAPSHOT_BLOCKS && heap_copy[j] == 0)
    {
        j++;
    }
    *start = i;
    return j - i;
}
