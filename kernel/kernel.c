#include <kernel.h>
#include <stdint.h>
#include <stddef.h>
// #include "../libc/include/stdio.h" // Should not use that.
#include "include/kernel/tty.h" // Use that.
#include "include/kernel/sys.h" // Use that.
#include "include/kernel/time/pit.h"
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
#include "time/rtc/rtc.h"

// #define UNUSED(x) (void)(x)

static struct paging_4gb_chunk *kernel_chunk = 0;

void panic(const char *msg)
{
    PANIC(msg, __FILE__, __LINE__);
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

void cas()
{
    kernel_main();
}

void kernel_main()
{
    terminal_initialize(KERNEL_BACKGROUND_COLOR);
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

    // Search and initialize the disks
    disk_search_and_init();
    // disk_search_and_init();

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

    // TODO: implement this in the coller way
    // pit_sleep(1000);
    dbg_puts("\33[31m");
    dbg_puts("Thats cool");
    dbg_puts("\033[0m\n");

    struct process *process = 0;
    int res = process_load_switch("0:/shell.elf", &process);
    if (res != POTONGOS_ALL_OK)
    {
        panic("Failed to load shell.elf\n");
    }

    // int fd = fopen("0:/hello.txt", "r");
    // if (fd)
    // {
    //     struct file_stat s;
    //     char buff[26];
    //     fread(buff, 26, 1, fd);
    //     print(buff);
    //     print("Lol\n");
    //     fstat(fd, &s);
    //     fclose(fd);
    // }

    printc(";- >", VGA_COLOR_LIGHT_CYAN);
    printf("%s", __TIMESTAMP__);
    printc("< -;\n", VGA_COLOR_LIGHT_MAGENTA);
    printtime();

    // pit_sleep(1000);

    // print(gettime());

    task_run_first_ever_task();
    print("Hello savta!\n");

    // Enable the system interrupts
    // enable_interrupts();

    // pit_configure(100);
    // pit_init();
    pit_sleep(100);
    print("Hello\n");

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