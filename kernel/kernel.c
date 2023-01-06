#include <kernel.h>
#include <stdint.h>
#include <stddef.h>
#include "../libc/include/stdio.h"
#include "idt/idt.h"
#include "io/io.h"
#include "memory/heap/kheap.h"
#include "memory/paging/paging.h"
#include "memory/memory.h"
#include "keyboard/keyboard.h"
#include "string/string.h"
#include "isr80h/isr80h.h"
#include "task/task.h"
#include "task/process.h"
#include "fs/file.h"
#include "disk/disk.h"
#include "fs/pparser.h"
#include "disk/streamer.h"
#include "task/tss.h"
#include "gdt/gdt.h"
#include "config.h"
#include "status.h"

#define UNUSED(x) (void)(x)

static struct paging_4gb_chunk *kernel_chunk = 0;

void panic(const char *msg)
{
    print(msg);
    while(1) {}
    // can call the halt function
}

// switch the page directory to the kernel page directory
// including the registers
void kernel_page()
{
    kernel_registers();
    paging_switch(kernel_chunk);
}

struct tss tss;
struct gdt gdt_real[POTONGOS_TOTAL_GDT_SEGMENTS];
struct gdt_structured gdt_structured[POTONGOS_TOTAL_GDT_SEGMENTS] = {
    {.base = 0x00, .limit = 0x00, .type = 0x00},                    // NULL Segment
    {.base = 0x00, .limit = 0xffffffff, .type = 0x9A},              // Kernel Code Segment
    {.base = 0x00, .limit = 0xffffffff, .type = 0x92},              // Kernel Data Segment
    {.base = 0x00, .limit = 0xffffffff, .type = 0xF8},              // User Code Segment
    {.base = 0x00, .limit = 0xffffffff, .type = 0xF2},              // User Data Segment
    {.base = (uint32_t)&tss, .limit = sizeof(tss), .type = 0xE9},   // TSS Segment
};

void kernel_main()
{
    terminal_initialize();
    // print("Hello World!\n");
    // video_mem[0] = 0x0341;  // This is reversed because endianness
    // video_mem[0] = terminal_make_char('B', 15);
    // Read mote about endianness: https://en.wikipedia.org/wiki/Endianness

    memset(gdt_real, 0x00, sizeof(gdt_real));
    gdt_structured_to_gdt(gdt_real, gdt_structured, POTONGOS_TOTAL_GDT_SEGMENTS);

    // Load the gdt
    gdt_load(gdt_real, sizeof(gdt_real));

    // Initialize the heap
    kheap_init();

    // Initialize filesystems
    fs_init();

    // SeRORTH and initialize the disks
    disk_seRORTH_and_init();

    // Initialize the interrupt descriptor table
    idt_init();

    // Setup the TSS
    memset(&tss, 0x00, sizeof(tss)); // memse the whole tss to 0
    tss.esp0 = 0x600000; // Where the kernel stack located
    tss.ss0 = KERNEL_DATA_SELECTOR;

    // Load the TSS
    tss_load(0x28); // GDT real offset

    // Setup paging
    kernel_chunk = paging_new_4gb(PAGING_IS_WRITEABLE | PAGING_IS_PRESENT | PAGING_ACCESS_FROM_ALL);

    // Swtich to kernel paging chunk
    paging_switch(kernel_chunk);

    // Enable paging
    enable_paging();
    
    // Register the kernel commands
    isr80h_register_commands();

    // Initialize all the system keyboards
    keyboard_init();

    struct process *process = 0;
    int res = process_load_switch("0:/blank.elf", &process);
    if (res != POTONGOS_ALL_OK)
    {
        panic("Failed to load blank.elf\n");
    }

    struct command_argument arg;
    arg.next = 0x00;
    strcpy(arg.argument, "A");

    process_inject_arguments(process, &arg);

    // Another process
    res = process_load_switch("0:/blank.elf", &process);
    if (res != POTONGOS_ALL_OK)
    {
        panic("Failed to load blank.elf\n");
    }

    strcpy(arg.argument, "B");
    arg.next = 0x00;
    process_inject_arguments(process, &arg);

    task_run_first_ever_task();
    print("Hello savta!\n");

    // Enable the system interrupts
    // enable_interrupts();

    while(1){}
}


/*
General Resources:
    https://wiki.osdev.org/Tutorials

Getting To Ring 3:
    https://wiki.osdev.org/Getting_to_Ring_3

GDT:
    https://wiki.osdev.org/GDT_Tutorial
    https://wiki.osdev.org/Global_Descriptor_Table

Counter:
    git ls-files | xargs wc -l
*/