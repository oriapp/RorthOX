#include "kheap.h"
#include "heap.h"
#include "config.h"
#include "kernel.h"
#include "memory/memory.h"

struct heap kernel_heap;
struct heap_table kernel_heap_table;

void kheap_init()
{
    // 104857600 (heap size in byte) / 4096 (block size) = 25600
    int total_table_entries = POTONGOS_HEAP_SIZE_BYTES / POTONGOS_HEAP_BLOCK_SIZE;
    kernel_heap_table.entries = (HEAP_BLOCK_TABLE_ENTRY*)(POTONGOS_HEAP_TABLE_ADDRESS) ;
    kernel_heap_table.total = total_table_entries;

    void *end = (void*) (POTONGOS_HEAP_ADDRESS + POTONGOS_HEAP_SIZE_BYTES); // End of table
    int res = heap_create(&kernel_heap, (void*)(POTONGOS_HEAP_ADDRESS), end, &kernel_heap_table);
    if (res < 0)
    {
        print("Falied to create heap\n");
    }
}

void *kmalloc(size_t size)
{
    return heap_malloc(&kernel_heap, size);
}

void *kzalloc(size_t size)
{
    void *ptr = kmalloc(size);
    if (!ptr)
    {
        return 0;
    }
    
    memset(ptr, 0x00, size);
    return ptr;
}

void kfree(void *ptr)
{
    heap_free(&kernel_heap, ptr);
}


/*
Resources:
    https://wiki.osdev.org/Memory_Map_(x86)


Tets:
    1. Build
    2. "gdb"
    2.2. "add-symbol-file ./bin/kernelfull.o 0x100000"
    3. "target remote |  qemu-system-i386 -hda ./build/os.bin -S -gdb stdio"
        The reason we use i386 this time is because we use
        32-bit symbols cause a few problems (sometimes).
    3.3. "add-symbol-file ../build/kernelfull.o 0x100000"
    4. "break kernel.c:line"
        Press "C" to continue
    5. type "print ptr"
*/